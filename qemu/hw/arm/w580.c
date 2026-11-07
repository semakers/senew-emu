/* Sony Ericsson W580i / DB2020: máquina de arranque del main original R8BE001.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Pista E del proyecto W580. Derivada en MÉTODO de la máquina w600 (DB2010, a su vez de la w800 de
 * SEW800iEmu), pero escrita de nuevo: aquí solo se modela lo que la traza de este firmware ha
 * pedido. Todo lo demás cae en el «cerrojo exploratorio»: se guarda lo escrito, se devuelve tal
 * cual y se registra (W580_ES) las primeras veces. Nada de ROM ni de pantalla inventadas.
 */
#include "qemu/osdep.h"
#include "system/system.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/visitor.h"
#include "qobject/qbool.h"
#include "qobject/qnum.h"
#include "qobject/qstring.h"
#include "hw/boards.h"
#include "hw/arm/machines-qom.h"
#include "hw/loader.h"
#include "ui/input.h"
#include "ui/console.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/address-spaces.h"
#include "chardev/char-fe.h"
#include "qemu/notify.h"
#include <math.h>

#define TYPE_W580_MACHINE MACHINE_TYPE_NAME("w580")
typedef struct W580State W580State;
DECLARE_INSTANCE_CHECKER(W580State, W580_MACHINE, TYPE_W580_MACHINE)

#define W580_RESET_PC 0x44140000u
#define W580_N_BANCOS 8          /* bancos del controlador de interrupciones (w580-intc.inc) */
#define W580_USB_FIFO_TAM 1024   /* pista UEMU: búfer por EP y sentido (w580-usb.inc) */

struct W580Nor;
typedef struct W580LCD W580LCD;
typedef struct W580UsbEp W580UsbEp;
typedef struct W580Usb W580Usb;
struct W580State {
    MachineState parent;
    ARMCPU *cpu;
    MemoryRegion unknown, bram, iram, itcm, dtcm;
    struct W580Nor *nor;
    W580LCD *lcd;
    GHashTable *latches, *seen;
    uint32_t es_veces;                 /* cuántas veces se registra cada (dirección, sentido) */
    uint32_t pag_r[4096], pag_w[4096]; /* accesos no modelados por megabyte */
    uint64_t es_total;
    uint32_t tcm[2];
    uint32_t ram_base;
    bool sin_ram;
    char *volcado;
#include "w580-estado.inc"
};

static void w580_irq_update(W580State *s);
static bool w580_reloj_activo(W580State *s);         /* pista E6: reloj de CPU según SYSCON sobre -icount (w580-reloj.inc) */
static void w580_perif_lineas(W580State *s);
static void w580_ms_es_syscon(W580State *s, uint32_t addr, uint32_t value);     /* pista E9: Memory Stick (w580-ms.inc) */
static void w580_usb_vbus(W580State *s, bool antes);  /* pista UEMU: VBUS al controlador USB (w580-usb.inc) */
static void w580_luz_muestra(W580State *s);          /* pista E5: historial de luces y panel (w580-luz.inc) */
static void w580_luz_chip_w(W580State *s, uint8_t reg, uint8_t value);   /* pista S5: una escritura en el chip de LED (rampa del PWM; w580-luz.inc) */
#include "w580-estricto.inc"
#include "w580-nor.inc"
#include "w580-perif.inc"

static void w580_reloj_syscon(W580State *s);
static void w580_get_u32(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
static void w580_set_u32(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
/* ------------------------------------------------------------------ cerrojo exploratorio */
static void w580_es_log(W580State *s, hwaddr addr, unsigned size, bool write, uint64_t value)
{
    uint64_t key = ((uint64_t)write << 32) | (uint32_t)addr;
    unsigned count = GPOINTER_TO_UINT(g_hash_table_lookup(s->seen, &key));
    s->es_total++;
    if (write) { s->pag_w[addr >> 20]++; } else { s->pag_r[addr >> 20]++; }
    if (count < s->es_veces && g_hash_table_size(s->seen) < 200000) {
        uint64_t *k = g_new(uint64_t, 1); *k = key;
        g_hash_table_insert(s->seen, k, GUINT_TO_POINTER(count + 1));
        qemu_log_mask(LOG_UNIMP, "W580_ES %s addr=0x%08" HWADDR_PRIx " size=%u value=0x%" PRIx64 " pc=0x%08x lr=0x%08x\n",
                      write ? "write" : "read", addr, size, value, s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
}
static uint64_t w580_latch_get(W580State *s, hwaddr addr, unsigned size)
{
    uint64_t result = 0;
    for (unsigned i = 0; i < size; i++) {
        result |= (uint64_t)GPOINTER_TO_UINT(g_hash_table_lookup(s->latches,
                  GUINT_TO_POINTER((uint32_t)addr + i))) << (i * 8);
    }
    return result;
}
static void w580_latch_set(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        g_hash_table_insert(s->latches, GUINT_TO_POINTER((uint32_t)addr + i),
                            GUINT_TO_POINTER((value >> (i * 8)) & 255));
    }
}
static uint64_t w580_read(void *opaque, hwaddr addr, unsigned size)
{
    W580State *s = opaque;
    uint64_t result = 0;
    if (s->panel_real && ((addr & ~3) == 0xf70011f4 || (addr & ~3) == 0xf7001408)) { w580_pr_lee_estado(s, addr & ~3); }   /* pista E6: bits de ocupado del PDI y del PDICON */
    if (w580_es_acceso(s, addr, size, false, &result)) { return result; }       /* modo estricto (pista E4) */
    if (w580_perif_read(s, addr, size, &result)) { return result; }
    result = w580_latch_get(s, addr, size);
    w580_perif_read_fix(s, addr, size, &result);
    if (addr == 0xf9090080) { w580_es_pll_lee(s, &result); }
    w580_es_log(s, addr, size, false, result);
    return result;
}
static void w580_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    W580State *s = opaque;
    if (w580_es_acceso(s, addr, size, true, &value)) { return; }                 /* modo estricto (pista E4) */
    if (w580_perif_write(s, addr, size, value)) { return; }
    w580_es_log(s, addr, size, true, value);
    w580_latch_set(s, addr, size, value);
    if (addr >= 0xf9000000 && addr < 0xf9000032) { w580_irq_update(s); }     /* GPIO: las entradas forzadas (gpio-in) interrumpen */
    if (addr >= 0xf9000000 && addr < 0xf9000080) { w580_luz_muestra(s); }    /* pista E5: PWM y pines de las luces */
    if ((addr & ~1) == 0xf909001c) { w580_reloj_syscon(s); }                 /* pista E6: reloj de CPU según SYSCON */
    if (addr <= 0xf9090005 && addr + size > 0xf9090004) { w580_bt_syscon(s); }  /* pista BT: nRESET del Bluetooth (bit 14) */
    if (addr <= 0xf909001b && addr + size > 0xf909001a) { w580_bt_arma(s); }    /* pista BT: la puerta 35 (UART 3) */
    if ((addr & ~(hwaddr)0x3f) == 0xfb080000) { w580_bt_tras_escribir(s, addr & 0x3f, value); }   /* pista BT: la línea de la UART 3 */
}
#include "w580-reloj.inc"
static const MemoryRegionOps w580_unknown_ops = {
    .read = w580_read, .write = w580_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

/* ------------------------------------------------------------------ TCM (cp15 c9,c1)
 * ARM DDI0198E 2-29: base física [31:12], tamaño [5:2], habilitación [0]. El ARM926 genérico de
 * QEMU no tiene estos registros. */
static uint64_t w580_tcm_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    W580State *s = W580_MACHINE(qdev_get_machine());
    return s->tcm[ri->opc2];
}
static void w580_tcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    W580State *s = W580_MACHINE(qdev_get_machine());
    MemoryRegion *mr = ri->opc2 ? &s->itcm : &s->dtcm;
    s->tcm[ri->opc2] = value;
    qemu_log_mask(LOG_UNIMP, "W580_TCM %s valor=0x%08x pc=0x%08x\n", ri->opc2 ? "ITCM" : "DTCM",
                  (unsigned)value, env->regs[15]);
    memory_region_transaction_begin();
    memory_region_set_address(mr, value & 0xfffff000);
    memory_region_set_enabled(mr, value & 1);
    memory_region_transaction_commit();
}
static const ARMCPRegInfo w580_tcm_regs[] = {
    { .name = "W580_DTCM", .cp = 15, .crn = 9, .crm = 1, .opc1 = 0, .opc2 = 0,
      .access = PL1_RW, .type = ARM_CP_NO_RAW | ARM_CP_IO,
      .readfn = w580_tcm_read, .writefn = w580_tcm_write },
    { .name = "W580_ITCM", .cp = 15, .crn = 9, .crm = 1, .opc1 = 0, .opc2 = 1,
      .access = PL1_RW, .type = ARM_CP_NO_RAW | ARM_CP_IO,
      .readfn = w580_tcm_read, .writefn = w580_tcm_write },
};

static void w580_reset(void *opaque)
{
    W580State *s = opaque;
    cpu_reset(CPU(s->cpu));
    s->cpu->env.regs[15] = W580_RESET_PC;
    s->tcm[0] = s->tcm[1] = 0x1c;       /* tamaño 64 KiB (0b0111 << 2) */
    memory_region_set_enabled(&s->itcm, false);
    memory_region_set_enabled(&s->dtcm, false);
    g_hash_table_remove_all(s->latches);
    w580_nor_reset(s->nor);
    w580_perif_reset(s);
    w580_es_reset(s);
    w580_reloj_reset(s);
    qemu_log_mask(LOG_UNIMP, "W580_RESET t=%" PRId64 "\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
static void w580_init(MachineState *machine)
{
    W580State *s = W580_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    s->latches = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->seen = g_hash_table_new(g_int64_hash, g_int64_equal);
    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));
    memory_region_init_io(&s->unknown, OBJECT(machine), &w580_unknown_ops, s, "w580.sin-modelo", UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(sysmem, 0, &s->unknown, -100);
    /* BRAM: las pilas de IRQ/SVC del prólogo (0x44140C64) viven en 0xF30002xx. [observado en el pseudo-C] */
    memory_region_init_ram(&s->bram, NULL, "w580.bram", 0x4000, &error_fatal);     /* MEMMAP_IO_BRAM: 0xF3000000 + 16 KiB */
    memory_region_add_subregion(sysmem, 0xf3000000, &s->bram);
    /* MEMMAP_IO_IRAM: 0xF6000000 + 0x3000; el prólogo (0x44140D1C) la borra byte a byte. */
    memory_region_init_ram(&s->iram, NULL, "w580.iram", 0x3000, &error_fatal);
    memory_region_add_subregion(sysmem, 0xf6000000, &s->iram);
    if (!s->sin_ram) { memory_region_add_subregion(sysmem, s->ram_base, machine->ram); }
    s->nor = w580_nor_init(s, sysmem, machine->firmware);
    define_arm_cp_regs(s->cpu, w580_tcm_regs);
    memory_region_init_ram(&s->itcm, NULL, "w580.itcm", 0x10000, &error_fatal);
    memory_region_init_ram(&s->dtcm, NULL, "w580.dtcm", 0x10000, &error_fatal);
    memory_region_add_subregion_overlap(sysmem, 0, &s->itcm, 10);
    memory_region_add_subregion_overlap(sysmem, 0x10000, &s->dtcm, 10);
    w580_perif_init(s, sysmem);
    w580_es_init(s);
    qemu_register_reset(w580_reset, s);
}

/* ------------------------------------------------------------------ propiedades */
static void w580_get_u32(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint32(v, name, (uint32_t *)((char *)obj + (uintptr_t)opaque), errp);
}
static void w580_set_u32(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint32(v, name, (uint32_t *)((char *)obj + (uintptr_t)opaque), errp);
}
static void w580_get_virtual_time(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    visit_type_int64(v, name, &now, errp);
}
static char *w580_get_paginas(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    GString *g = g_string_new("");
    for (unsigned i = 0; i < 4096; i++) {
        if (s->pag_r[i] || s->pag_w[i]) {
            g_string_append_printf(g, "%03X00000\t%u\t%u\n", i, s->pag_r[i], s->pag_w[i]);
        }
    }
    return g_string_free(g, false);
}
static char *w580_get_volcado(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->volcado); }
static void w580_set_volcado(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_free(s->volcado); s->volcado = g_strdup(value);
    if (s->nor && value && *value && !w580_nor_volcar(s->nor, value)) {
        error_setg(errp, "no se pudo escribir %s", value);
    }
}
static char *w580_get_nor_cfi_pon(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->nor_cfi_pon); }
static void w580_set_nor_cfi_pon(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_free(s->nor_cfi_pon); s->nor_cfi_pon = g_strdup(value);
}
static bool w580_get_sin_ram(Object *obj, Error **errp) { return W580_MACHINE(obj)->sin_ram; }
static void w580_set_sin_ram(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->sin_ram = value; }
static void w580_instance_init(Object *obj)
{
    W580State *s = W580_MACHINE(obj);
    s->es_veces = 3; s->cpu_mhz = 13;
    s->ram_base = 0x20000000;
    s->nor_id = 0x0089887e;
    w580_perif_instance_init(s);
}
static void w580_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "W580i (DB2020) machine model for senew (unofficial, no manufacturer software)";
    mc->init = w580_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm926");
    mc->default_ram_size = 16 * 1024 * 1024;   /* MEMMAP_RAM_SDRAM_SIZE (tabla de 0x45A8EC4C) */
    mc->default_ram_id = "w580.ram";
    mc->max_cpus = 1;
    object_class_property_add(oc, "virtual-time-ns", "int64", w580_get_virtual_time, NULL, NULL, NULL);
    object_class_property_add(oc, "es-veces", "uint32", w580_get_u32, w580_set_u32, NULL,
                              (void *)offsetof(W580State, es_veces));
    object_class_property_set_description(oc, "es-veces", "Veces que se registra cada (direccion, sentido) de E/S no modelada");
    object_class_property_add(oc, "ram-base", "uint32", w580_get_u32, w580_set_u32, NULL,
                              (void *)offsetof(W580State, ram_base));
    object_class_property_add_bool(oc, "sin-ram", w580_get_sin_ram, w580_set_sin_ram);
    object_class_property_add_str(oc, "paginas", w580_get_paginas, NULL);
    object_class_property_set_description(oc, "paginas", "Accesos de E/S no modelada por megabyte: pagina, lecturas, escrituras");
    object_class_property_add_str(oc, "volcar-nor", w580_get_volcado, w580_set_volcado);
    object_class_property_set_description(oc, "volcar-nor", "Al escribir una ruta, vuelca ahi los 64 MiB de la NOR");
    object_class_property_add(oc, "nor-id", "uint32", w580_get_u32, w580_set_u32, NULL, (void *)offsetof(W580State, nor_id));
    object_class_property_set_description(oc, "nor-id", "Pista E7: fabricante << 16 | dispositivo que contesta la NOR a la orden 90 (por omision 0x0089887E). Solo al lanzar");
    object_class_property_add_str(oc, "nor-cfi-pon", w580_get_nor_cfi_pon, w580_set_nor_cfi_pon);
    object_class_property_set_description(oc, "nor-cfi-pon", "Pista E7: altera palabras del CFI de la NOR: PALABRA=VALOR[:PALABRA=VALOR] (p. ej. 0x10=0xFF: sin la Q de QRY). Solo al lanzar");
    w580_perif_class_init(oc);
    w580_es_class_init(oc);
    w580_reloj_class_init(oc);
}
static const TypeInfo w580_type = {
    .name = TYPE_W580_MACHINE, .parent = TYPE_MACHINE,
    .interfaces = arm_machine_interfaces,
    .instance_size = sizeof(W580State), .class_init = w580_class_init,
    .instance_init = w580_instance_init,
};
static void w580_register_types(void) { type_register_static(&w580_type); }
type_init(w580_register_types)
