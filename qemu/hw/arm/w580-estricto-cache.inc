/* W580 (pista E5): MODO ESTRICTO, caché de datos frente al hardware que lee o escribe la SDRAM él solo (PDICON,
 * MMIACC) y frente a la NOR. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * QEMU no tiene cachés: lo que la CPU escribe está en la RAM en el acto. En el aparato (ARM926EJ-S, líneas de 32
 * bytes), una escritura en una página mapeada C = 1, B = 1 («copy_back», caché de escritura diferida) puede quedarse
 * en la caché hasta que el firmware LIMPIA la línea (cp15 c7,c10,1; c7,c14,1 limpia e invalida) o la caché la
 * desaloja sola. El PDICON (F7001400) y el MMIACC (F7001800) leen la SDRAM física sin pasar por la CPU: si la línea
 * sigue sucia, reciben lo VIEJO.
 *
 * El modelo (aproximación CONSERVADORA; no simula la caché: ni su tamaño, ni los desalojos, ni si la línea estaba
 * cargada cuando se escribió —un ARM926 no carga la línea en un fallo de escritura—):
 *  - una VENTANA de la SDRAM, la memoria gráfica EXT_GRAM 0x20E80000 + 1,5 MiB (la única zona de la que el original
 *    manda cuadros), se monta como «ROM con escritura por E/S»: las lecturas van directas y cada escritura de la CPU
 *    pasa por w580_ca_write, que marca su línea de 32 bytes como SUCIA y EN CACHÉ si en ese momento SCTLR tiene la
 *    MMU y la caché de datos encendidas;
 *  - dos copias: «vista» (lo que ve la CPU; es el respaldo de la región) y «fisica» (lo que hay en la SDRAM según
 *    el modelo: se pone al día al limpiar la línea);
 *  - las operaciones de c7 se interceptan: limpiar por dirección baja la línea a «fisica»; invalidar una línea sucia
 *    sin limpiarla se cuenta (lo escrito se pierde); por índice, una pasada entera (W580_CA_PASADA seguidas) o
 *    «test and clean» = todo limpio;
 *  - cuando el PDICON o el MMIACC leen un tramo, las líneas sucias cuya página está mapeada con escritura diferida
 *    (se recorre la tabla de páginas de TTBR0 al revés: de física a virtual) se entregan de «fisica» —lo viejo— y,
 *    si difieren de lo que ve la CPU, aviso [cache].
 * Fuera de la ventana no se vigila nada: un cuadro mandado desde otro sitio es su propio aviso.
 *
 * Con «estricto-aprender» no cambia nada de lo que se ve (el hardware lee siempre la vista de la CPU): solo cuenta.
 * Sin estricto ni aprender la ventana no se monta y los registros de c7 no se tocan. */
#include "qemu/bitmap.h"
#include "qemu/bitops.h"
#define W580_CA_BASE 0x20e80000u
#define W580_CA_TAM  0x00180000u
#define W580_CA_LIN  32u
#define W580_CA_NLIN (W580_CA_TAM / W580_CA_LIN)
#define W580_CA_PASADA 128      /* operaciones por índice seguidas que se toman por «la caché entera» [inferido: 4 KiB de 4 vías; el tamaño real: sin saber] */
enum { CA_M = 1, CA_DIF = 2, CA_C = 4, CA_NOC = 8, CA_ALIAS = 16 };   /* mapeada · algún alias C=1 B=1 · algún alias C=1 · algún alias sin caché · más de una virtual */
typedef struct { uint32_t base, npag; uint8_t *f; uint32_t *va; } W580CaZona;
typedef struct { uint32_t bytes, fuera, lineas, sucias, distintas, resueltas, primera, pa0; bool hay_pa0; } W580CaLectura;
enum { CA_Z_VENT, CA_Z_RAM, CA_Z_NOR, CA_Z_OTRA, CA_NZ };
static const char *const w580_ca_zona_nombre[CA_NZ] = { "ventana_grafica", "resto_SDRAM", "NOR", "otra" };

static struct {
    MemoryRegion mr;
    bool hay, hw, sordo;             /* sordo: prueba negativa, las limpiezas por dirección no hacen nada */
    uint8_t *vista, *fisica;
    unsigned long *sucia, *en_cache;
    W580CaZona zona[2];              /* la ventana gráfica y la NOR (0x44000000 + 64 MiB) */
    uint32_t mapa_ttbr; int64_t mapa_t; bool mapa_ok, mapa_mmu; uint32_t mapa_firma; unsigned mapa_n;
    bool cache_on;
    /* contadores */
    uint64_t c7[16][8], c7_zona[16][8][CA_NZ], c7_pc[16][8];
    uint64_t esc_cache, esc_sin, limpias_utiles, limpias_vacias, perdidas, n_indice, n_pasadas, n_tc, n_dren;
    unsigned indice_seguidas;
    bool sin_drenar;
    uint64_t pd_n, pd_sucias, pd_distintas, pd_fuera, pd_sin_drenar, pd_lineas, pd_lin_sucias, pd_lin_resueltas, pd_no_diferida;
    uint64_t mm_n, mm_org_sucias, mm_org_distintas, mm_dst_sucias, mm_dst_en_cache, mm_fuera, mm_sin_drenar, mm_lin_dst_en_cache, mm_lin_dst;
    uint64_t limp_desde, esc_desde;  /* limpiezas útiles y escrituras con caché desde la última transferencia */
    unsigned n_log;
    struct { uint32_t pc; uint8_t crm; uint64_t n, utiles; } pcs[24];
    /* NOR */
    bool lecturas; uint64_t lect_cache;      /* pista E6: estricto-cache-lecturas */
    bool nor_pend; uint32_t nor_pend_dir; uint8_t nor_pend_cmd;
    uint64_t nor_ord_diferida, nor_ord_sin_limpiar, nor_prog_con_cache, nor_prog_sin_invalidar;
    bool nor_inv_pend; uint32_t nor_inv_ini, nor_inv_fin;
} w580_ca;

static bool w580_ca_sctlr_cache(W580State *s) { return (s->cpu->env.cp15.sctlr_ns & 5) == 5; }
static uint32_t w580_ca_r32(uint32_t pa)
{
    uint32_t d = 0;
    if (address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, &d, 4) != MEMTX_OK) { return 0; }
    return le32_to_cpu(d);
}
/* tabla de páginas de ARMv5 (descriptores cortos): de virtual a física. false = sin mapear */
static bool w580_ca_fisica_de(W580State *s, uint32_t va, uint32_t *pa)
{
    CPUARMState *env = &s->cpu->env;
    if (!(env->cp15.sctlr_ns & 1)) { *pa = va; return true; }
    uint32_t d = w580_ca_r32(((uint32_t)env->cp15.ttbr0_ns & ~0x3fffu) + (va >> 20) * 4), e;
    switch (d & 3) {
    case 2: *pa = (d & 0xfff00000) | (va & 0xfffff); return true;
    case 1:
        e = w580_ca_r32((d & ~0x3ffu) + ((va >> 12) & 0xff) * 4);
        if ((e & 3) == 1) { *pa = (e & 0xffff0000) | (va & 0xffff); return true; }
        if (e & 2) { *pa = (e & 0xfffff000) | (va & 0xfff); return true; }
        return false;
    case 3:
        e = w580_ca_r32((d & ~0xfffu) + ((va >> 10) & 0x3ff) * 4);
        if ((e & 3) == 1) { *pa = (e & 0xffff0000) | (va & 0xffff); return true; }
        if ((e & 3) == 2) { *pa = (e & 0xfffff000) | (va & 0xfff); return true; }
        if ((e & 3) == 3) { *pa = (e & 0xfffffc00) | (va & 0x3ff); return true; }
        return false;
    }
    return false;
}
/* de física a virtual: qué atributos de caché tienen las páginas de las dos zonas en la tabla de TTBR0 */
static void w580_ca_anota(uint32_t pa, uint32_t len, uint32_t va, uint32_t d)
{
    for (unsigned k = 0; k < 2; k++) {
        W580CaZona *z = &w580_ca.zona[k];
        uint64_t ini = MAX((uint64_t)pa, z->base), fin = MIN((uint64_t)pa + len, (uint64_t)z->base + ((uint64_t)z->npag << 12));
        for (uint64_t p = ini & ~UINT64_C(0xfff); p < fin; p += 4096) {
            unsigned i = (p - z->base) >> 12;
            uint8_t f = z->f[i]; uint32_t v = va + (uint32_t)(p - pa);
            if ((f & CA_M) && z->va[i] != v) { f |= CA_ALIAS; } else if (!(f & CA_M)) { z->va[i] = v; }
            z->f[i] = f | CA_M | ((d & 0xc) == 0xc ? CA_DIF : 0) | ((d & 8) ? CA_C : CA_NOC);
        }
    }
}
static void w580_ca_mapa(W580State *s)
{
    CPUARMState *env = &s->cpu->env;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t ttbr = (uint32_t)env->cp15.ttbr0_ns & ~0x3fffu;
    bool mmu = env->cp15.sctlr_ns & 1;
    if (w580_ca.mapa_ok && ttbr == w580_ca.mapa_ttbr && mmu == w580_ca.mapa_mmu && now - w580_ca.mapa_t < 50 * 1000 * 1000) { return; }
    w580_ca.mapa_ok = true; w580_ca.mapa_ttbr = ttbr; w580_ca.mapa_mmu = mmu; w580_ca.mapa_t = now;
    for (unsigned k = 0; k < 2; k++) { memset(w580_ca.zona[k].f, 0, w580_ca.zona[k].npag); }
    if (!mmu) { return; }
    static uint32_t l1[4096], l2[1024];
    if (address_space_read(&address_space_memory, ttbr, MEMTXATTRS_UNSPECIFIED, l1, sizeof(l1)) != MEMTX_OK) { return; }
    for (unsigned i = 0; i < 4096; i++) {
        uint32_t d = le32_to_cpu(l1[i]);
        if ((d & 3) == 2) { w580_ca_anota(d & 0xfff00000, 0x100000, i << 20, d); }
        else if ((d & 3) == 1) {
            if (address_space_read(&address_space_memory, d & ~0x3ffu, MEMTXATTRS_UNSPECIFIED, l2, 1024) != MEMTX_OK) { continue; }
            for (unsigned j = 0; j < 256; j++) {
                uint32_t e = le32_to_cpu(l2[j]);
                if ((e & 3) == 1) { w580_ca_anota((e & 0xffff0000) | ((j & 15) << 12), 4096, (i << 20) | (j << 12), e); }
                else if (e & 2) { w580_ca_anota(e & 0xfffff000, 4096, (i << 20) | (j << 12), e); }
            }
        } else if ((d & 3) == 3) {
            if (address_space_read(&address_space_memory, d & ~0xfffu, MEMTXATTRS_UNSPECIFIED, l2, 4096) != MEMTX_OK) { continue; }
            for (unsigned j = 0; j < 1024; j++) {
                uint32_t e = le32_to_cpu(l2[j]);
                if ((e & 3) == 1) { w580_ca_anota((e & 0xffff0000) | ((j & 63) << 10), 1024, (i << 20) | (j << 10), e); }
                else if ((e & 3) == 2) { w580_ca_anota((e & 0xfffff000) | ((j & 3) << 10), 1024, (i << 20) | (j << 10), e); }
                else if ((e & 3) == 3) { w580_ca_anota(e & 0xfffffc00, 1024, (i << 20) | (j << 10), e); }
            }
        }
    }
    /* para la bitácora: el mapa de la ventana cada vez que cambia */
    uint32_t firma = ttbr; unsigned n[5] = { 0 };
    W580CaZona *z = &w580_ca.zona[0];
    for (unsigned i = 0; i < z->npag; i++) {
        uint8_t f = z->f[i]; firma = firma * 31 + f + z->va[i] * (f & CA_M);
        n[0] += !!(f & CA_M); n[1] += !!(f & CA_DIF); n[2] += (f & CA_C) && !(f & CA_DIF); n[3] += !!(f & CA_NOC); n[4] += !!(f & CA_ALIAS);
    }
    if (firma != w580_ca.mapa_firma && w580_ca.mapa_n++ < 40) {
        unsigned nd = 0; W580CaZona *zn = &w580_ca.zona[1];
        for (unsigned i = 0; i < zn->npag; i++) { nd += !!(zn->f[i] & CA_DIF); }
        w580_es_suceso("cache", "mapa TTBR0=0x%08x ventana 0x%08x+0x%x: %u paginas de 4K, mapeadas=%u diferida(C=1,B=1)=%u solo_C=%u sin_cache=%u con_alias=%u "
                       "va[0]=0x%08x f[0]=0x%02x va[38]=0x%08x f[38]=0x%02x | NOR: paginas con alias de escritura diferida=%u de %u", ttbr, W580_CA_BASE, W580_CA_TAM,
                       z->npag, n[0], n[1], n[2], n[3], n[4], z->va[0], z->f[0], z->va[38], z->f[38], nd, zn->npag);
    }
    w580_ca.mapa_firma = firma;
}
static bool w580_ca_diferida(W580State *s, unsigned lin)
{
    w580_ca_mapa(s);
    return w580_ca.mapa_mmu && (w580_ca.zona[0].f[(lin * W580_CA_LIN) >> 12] & CA_DIF);
}
static void w580_ca_baja(unsigned lin)       /* la línea pasa de la caché a la SDRAM */
{
    memcpy(w580_ca.fisica + lin * W580_CA_LIN, w580_ca.vista + lin * W580_CA_LIN, W580_CA_LIN);
    clear_bit(lin, w580_ca.sucia);
}
static void w580_ca_todo_limpio(bool invalida)
{
    for (unsigned l = find_first_bit(w580_ca.sucia, W580_CA_NLIN); l < W580_CA_NLIN; l = find_next_bit(w580_ca.sucia, W580_CA_NLIN, l + 1)) {
        w580_ca_baja(l); w580_ca.limpias_utiles++; w580_ca.limp_desde++; w580_ca.sin_drenar = true;
    }
    if (invalida) { bitmap_zero(w580_ca.en_cache, W580_CA_NLIN); }
}

/* escritura de la CPU en la ventana (las del hardware llevan w580_ca.hw) */
static void w580_ca_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    W580State *s = opaque;
    if (off + size > W580_CA_TAM) { return; }
    for (unsigned i = 0; i < size; i++) { w580_ca.vista[off + i] = value >> (8 * i); }
    unsigned l0 = off / W580_CA_LIN, l1 = (off + size - 1) / W580_CA_LIN;
    if (w580_ca.hw || !w580_ca_sctlr_cache(s)) {
        if (!w580_ca.hw) { w580_ca.esc_sin++; }
        memcpy(w580_ca.fisica + off, w580_ca.vista + off, size);       /* sin caché (o el hardware): va derecho a la SDRAM */
        return;
    }
    w580_ca.esc_cache++; w580_ca.esc_desde++;
    set_bit(l0, w580_ca.sucia); set_bit(l0, w580_ca.en_cache);
    if (l1 != l0) { set_bit(l1, w580_ca.sucia); set_bit(l1, w580_ca.en_cache); }
}
/* Lecturas de la CPU. En modo ROMD (lo normal) van directas y esto no se llama. Pista E6 (pendiente 4 de E5): con
 * `estricto-cache-lecturas=on` la ventana deja de ser ROMD y cada lectura de la CPU pasa por aquí: una línea LEÍDA con la
 * caché encendida por una página con caché también queda «en caché» (y rancia si después el MMIACC escribe debajo).
 * Es lento (cada píxel que lee el firmware es un acceso de E/S): por eso es opcional. */
static uint64_t w580_ca_read(void *opaque, hwaddr off, unsigned size)
{
    W580State *s = opaque;
    uint64_t v = 0;
    for (unsigned i = 0; i < size && off + i < W580_CA_TAM; i++) { v |= (uint64_t)w580_ca.vista[off + i] << (8 * i); }
    if (w580_ca.lecturas && !w580_ca.hw && off + size <= W580_CA_TAM && w580_ca_sctlr_cache(s)) {
        unsigned l0 = off / W580_CA_LIN, l1 = (off + size - 1) / W580_CA_LIN;
        w580_ca_mapa(s);
        if (w580_ca.mapa_mmu && (w580_ca.zona[0].f[off >> 12] & CA_C)) {
            w580_ca.lect_cache++; set_bit(l0, w580_ca.en_cache);
            if (l1 != l0) { set_bit(l1, w580_ca.en_cache); }
        }
    }
    return v;
}
static const MemoryRegionOps w580_ca_ops = {
    .read = w580_ca_read, .write = w580_ca_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

/* ------------------------------------------------------------------------------------------------ cp15 c7 */
static unsigned w580_ca_zona_de(uint32_t pa)
{
    if (pa >= W580_CA_BASE && pa < W580_CA_BASE + W580_CA_TAM) { return CA_Z_VENT; }
    if (pa >= 0x20000000 && pa < 0x21000000) { return CA_Z_RAM; }
    if ((pa >= 0x44000000 && pa < 0x48000000) || (pa >= 0x04000000 && pa < 0x08000000)) { return CA_Z_NOR; }
    return CA_Z_OTRA;
}
static void w580_ca_c7_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    W580State *s = W580_MACHINE(qdev_get_machine());
    unsigned crm = ri->crm & 15, op2 = ri->opc2 & 7;
    if (!w580_ca.hay) { return; }
    CPU(s->cpu)->mem_io_pc = 0;          /* para que un aviso desde aquí lleve el PC de ESTA instrucción (regs[15] está al día: ARM_CP_RAISES_EXC) */
    if (!w580_ca.c7[crm][op2]++) { w580_ca.c7_pc[crm][op2] = env->regs[15]; }
    bool por_dir = op2 == 1 && (crm == 10 || crm == 14 || crm == 6);
    bool por_ind = op2 == 2 && (crm == 10 || crm == 14);
    if (!por_ind) { w580_ca.indice_seguidas = 0; }
    if (por_dir) {
        uint32_t pa = 0;
        if (!w580_ca_fisica_de(s, value, &pa)) { w580_ca.c7_zona[crm][op2][CA_Z_OTRA]++; return; }
        unsigned z = w580_ca_zona_de(pa);
        w580_ca.c7_zona[crm][op2][z]++;
        if (z == CA_Z_NOR) {
            uint32_t ab = (pa & 0x03ffffff) & ~(W580_CA_LIN - 1);
            if (w580_ca.nor_pend && crm != 6 && ab == (w580_ca.nor_pend_dir & ~(W580_CA_LIN - 1))) { w580_ca.nor_pend = false; }
            if (w580_ca.nor_inv_pend && crm != 10 && ab >= (w580_ca.nor_inv_ini & ~(W580_CA_LIN - 1)) && ab < w580_ca.nor_inv_fin) { w580_ca.nor_inv_pend = false; }
            return;
        }
        if (z != CA_Z_VENT || (w580_ca.sordo && crm != 6)) { return; }
        unsigned lin = (pa - W580_CA_BASE) / W580_CA_LIN, k;
        for (k = 0; k < G_N_ELEMENTS(w580_ca.pcs) - 1 && w580_ca.pcs[k].n && !(w580_ca.pcs[k].pc == env->regs[15] && w580_ca.pcs[k].crm == crm); k++) { }
        w580_ca.pcs[k].pc = env->regs[15]; w580_ca.pcs[k].crm = crm; w580_ca.pcs[k].n++;
        if (test_bit(lin, w580_ca.sucia)) { w580_ca.pcs[k].utiles++; }
        if (crm == 6) {                                  /* invalidar sin limpiar */
            if (test_bit(lin, w580_ca.sucia)) {
                if (w580_ca_diferida(s, lin)) {
                    w580_ca.perdidas++;
                    w580_es_aviso(s, "cache", 0x600, "se INVALIDA (cp15 c7,c6,1) la línea 0x%08x de la memoria gráfica, que está escrita y sin limpiar: en el aparato "
                                  "lo escrito en la caché se pierde (salvo que la caché ya la hubiera desalojado). Antes hay que limpiarla (c7,c10,1) o usar c7,c14,1",
                                  W580_CA_BASE + lin * W580_CA_LIN);
                }
                w580_ca_baja(lin);
            }
            clear_bit(lin, w580_ca.en_cache);
            return;
        }
        if (test_bit(lin, w580_ca.sucia)) { w580_ca_baja(lin); w580_ca.limpias_utiles++; w580_ca.limp_desde++; w580_ca.sin_drenar = true; }
        else { w580_ca.limpias_vacias++; }
        if (crm == 14) { clear_bit(lin, w580_ca.en_cache); }
    } else if (por_ind) {
        w580_ca.n_indice++;
        if (++w580_ca.indice_seguidas == W580_CA_PASADA) { w580_ca.n_pasadas++; w580_ca_todo_limpio(crm == 14); }
    } else if (crm == 10 && op2 == 4) {
        w580_ca.n_dren++; w580_ca.sin_drenar = false;
    } else if ((crm == 6 || crm == 7) && op2 == 0) {     /* invalidar toda la caché de datos */
        unsigned n = 0;
        for (unsigned l = find_first_bit(w580_ca.sucia, W580_CA_NLIN); l < W580_CA_NLIN; l = find_next_bit(w580_ca.sucia, W580_CA_NLIN, l + 1)) {
            if (w580_ca_diferida(s, l)) { n++; }
            w580_ca_baja(l);
        }
        bitmap_zero(w580_ca.en_cache, W580_CA_NLIN);
        if (n) {
            w580_ca.perdidas += n;
            w580_es_aviso(s, "cache", 0x601, "se INVALIDA toda la caché de datos (cp15 c7,c%u,0) con %u líneas de la memoria gráfica escritas y sin limpiar: "
                          "en el aparato eso se pierde", crm, n);
        }
    }
}
static uint64_t w580_ca_c7_tc_read(CPUARMState *env, const ARMCPRegInfo *ri)     /* «test and clean»: el bucle del firmware acaba al ver Z = 1 */
{
    if (w580_ca.hay) {
        w580_ca.c7[ri->crm & 15][ri->opc2 & 7]++; w580_ca.n_tc++; w580_ca.indice_seguidas = 0;
        /* (AUD4) «test and clean» limpia UNA línea por operación y da Z = 1 solo cuando no queda ninguna sucia (ARM926EJ-S): antes
         * una sola lectura lo dejaba todo limpio, y un firmware que la hiciera una vez, sin bucle, pasaba. */
        unsigned l = find_first_bit(w580_ca.sucia, W580_CA_NLIN);
        if (l < W580_CA_NLIN) {
            w580_ca_baja(l); w580_ca.limpias_utiles++; w580_ca.limp_desde++; w580_ca.sin_drenar = true;
            if (ri->crm == 14) { clear_bit(l, w580_ca.en_cache); }
            return 0;
        }
        if (ri->crm == 14) { bitmap_zero(w580_ca.en_cache, W580_CA_NLIN); }
    }
    return 1u << 30;
}
#define W580_CA_C7(n, m, o) { .name = n, .cp = 15, .crn = 7, .crm = m, .opc1 = 0, .opc2 = o, .access = PL1_W, \
      .type = ARM_CP_NO_RAW | ARM_CP_OVERRIDE | ARM_CP_IO | ARM_CP_RAISES_EXC, .writefn = w580_ca_c7_write }
static const ARMCPRegInfo w580_ca_c7_regs[] = {
    W580_CA_C7("W580_DC_CLEAN_MVA", 10, 1), W580_CA_C7("W580_DC_CLEAN_IDX", 10, 2), W580_CA_C7("W580_DRAIN_WB", 10, 4),
    W580_CA_C7("W580_DC_CLINV_MVA", 14, 1), W580_CA_C7("W580_DC_CLINV_IDX", 14, 2),
    W580_CA_C7("W580_DC_INV_ALL", 6, 0), W580_CA_C7("W580_DC_INV_MVA", 6, 1), W580_CA_C7("W580_DC_INV_IDX", 6, 2), W580_CA_C7("W580_IDC_INV_ALL", 7, 0),
    W580_CA_C7("W580_IC_INV_ALL", 5, 0), W580_CA_C7("W580_IC_INV_MVA", 5, 1),
    { .name = "W580_TC_DCACHE", .cp = 15, .crn = 7, .crm = 10, .opc1 = 0, .opc2 = 3, .access = PL1_R,
      .type = ARM_CP_NO_RAW | ARM_CP_OVERRIDE | ARM_CP_IO, .readfn = w580_ca_c7_tc_read },
    { .name = "W580_TCI_DCACHE", .cp = 15, .crn = 7, .crm = 14, .opc1 = 0, .opc2 = 3, .access = PL1_R,
      .type = ARM_CP_NO_RAW | ARM_CP_OVERRIDE | ARM_CP_IO, .readfn = w580_ca_c7_tc_read },
};

/* ------------------------------------------------------------------------------------------------ el hardware lee
 * El PDICON llama con cada tramo que acaba de leer de la SDRAM (dirección física y datos). En estricto, las líneas
 * sucias se sustituyen por lo que hay en la SDRAM física según el modelo (lo viejo). */
static void w580_ca_hw_lee(W580State *s, uint32_t pa, uint8_t *buf, unsigned n, W580CaLectura *r)
{
    if (!w580_ca.hay || !n) { return; }
    r->bytes += n;
    if (!r->hay_pa0) { r->hay_pa0 = true; r->pa0 = pa; }
    if (pa < W580_CA_BASE || (uint64_t)pa + n > W580_CA_BASE + W580_CA_TAM) { r->fuera += n; return; }
    uint32_t o = pa - W580_CA_BASE;
    for (unsigned l = o / W580_CA_LIN; l <= (o + n - 1) / W580_CA_LIN; l++) {
        r->lineas++;
        if (!test_bit(l, w580_ca.sucia)) { continue; }
        if (!w580_ca_diferida(s, l)) { w580_ca_baja(l); r->resueltas++; continue; }     /* la página no tiene caché de escritura diferida: no hay línea sucia */
        uint32_t ini = MAX(l * W580_CA_LIN, o), fin = MIN((l + 1) * W580_CA_LIN, o + n);
        if (!r->sucias++) { r->primera = W580_CA_BASE + l * W580_CA_LIN; }
        if (memcmp(w580_ca.fisica + ini, w580_ca.vista + ini, fin - ini)) { r->distintas++; }
        if (w580_es.on) { memcpy(buf + (ini - o), w580_ca.fisica + ini, fin - ini); }
    }
}
static const char *w580_ca_mapa_txt(unsigned lin)
{
    static char b[120];
    W580CaZona *z = &w580_ca.zona[0]; unsigned i = (lin * W580_CA_LIN) >> 12; uint8_t f = z->f[i];
    snprintf(b, sizeof(b), "virtual 0x%08x, %s%s", z->va[i] + ((lin * W580_CA_LIN) & 0xfff), (f & CA_DIF) ? "C=1 B=1" : (f & CA_C) ? "C=1 B=0" : "sin caché",
             (f & CA_ALIAS) ? ((f & CA_NOC) ? "; hay además un alias SIN caché" : "; con alias") : "");
    return b;
}
static void w580_ca_pdicon_fin(W580State *s, W580CaLectura *r, uint32_t org, unsigned x0, unsigned y0, unsigned w, unsigned h)
{
    if (!w580_ca.hay) { return; }
    w580_ca.pd_n++; w580_ca.pd_lineas += r->lineas; w580_ca.pd_lin_sucias += r->sucias; w580_ca.pd_lin_resueltas += r->resueltas;
    if (r->sucias) { w580_ca.pd_sucias++; }
    if (r->distintas) { w580_ca.pd_distintas++; }
    if (r->fuera) { w580_ca.pd_fuera++; }
    bool sin_dren = w580_ca.sin_drenar && w580_ca.limp_desde;
    if (sin_dren) { w580_ca.pd_sin_drenar++; }
    unsigned l0 = (r->fuera || !r->hay_pa0) ? 0 : (r->pa0 - W580_CA_BASE) / W580_CA_LIN;
    bool dif = !r->fuera && r->hay_pa0 && w580_ca_diferida(s, l0);
    if (!dif && !r->fuera) { w580_ca.pd_no_diferida++; }
    if (w580_es.aprender && (w580_ca.n_log < 80 || ((r->sucias || r->fuera || sin_dren) && w580_ca.n_log < 600))) {
        w580_ca.n_log++;
        w580_es_suceso("cache", "pdicon n=%" PRIu64 " origen=0x%08x rect=%u,%u %ux%u bytes=%u fuera_de_la_ventana=%u lineas=%u sucias=%u distintas=%u no_diferida=%u "
                       "limpiezas_utiles_desde_la_anterior=%" PRIu64 " escrituras_con_cache_desde=%" PRIu64 " sin_drenar=%d cache=%d %s pc=0x%08x",
                       w580_ca.pd_n, 0x20000000 + org, x0, y0, w, h, r->bytes, r->fuera, r->lineas, r->sucias, r->distintas, r->resueltas,
                       w580_ca.limp_desde, w580_ca.esc_desde, sin_dren, w580_ca_sctlr_cache(s), r->fuera ? "-" : w580_ca_mapa_txt(l0), w580_es_pc(s));
    }
    w580_ca.limp_desde = w580_ca.esc_desde = 0;
    if (r->fuera) {
        w580_es_aviso(s, "cache", 0x100, "el PDICON manda el cuadro desde 0x%08x (%u bytes fuera de 0x%08x..0x%08x): el original solo manda cuadros desde su "
                      "memoria gráfica EXT_GRAM, y el modelo de caché no vigila otra zona (ahí no puede decir si las líneas están limpias)",
                      0x20000000 + org, r->fuera, W580_CA_BASE, W580_CA_BASE + W580_CA_TAM - 1);
    }
    if (r->distintas) {
        w580_es_aviso(s, "cache", 0x101, "PDICON+04 lanza el cuadro (%u,%u %ux%u desde 0x%08x) con %u de %u líneas de caché escritas por la CPU y SIN LIMPIAR "
                      "(%u con contenido distinto del de la SDRAM; la primera 0x%08x, %s): el PDICON lee la SDRAM física y el panel recibe lo VIEJO de esas "
                      "líneas. El original limpia antes el rectángulo que manda (cp15 c7,c10,1 línea a línea de 32 bytes, FUN_44a2c0f4) y NO vacía el búfer de escritura",
                      x0, y0, w, h, 0x20000000 + org, r->sucias, r->lineas, r->distintas, r->primera, w580_ca_mapa_txt((r->primera - W580_CA_BASE) / W580_CA_LIN));
    }
}
/* MMIACC: copia de n bytes dentro de la SDRAM (desplazamientos). true = hecha aquí. */
static bool w580_ca_mmiacc(W580State *s, uint32_t dst, uint32_t src, uint64_t n)
{
    if (!w580_ca.hay) { return false; }
    const uint32_t b = W580_CA_BASE - 0x20000000u;
    w580_ca.mm_n++;
    if (src < b || dst < b || src + n > b + W580_CA_TAM || dst + n > b + W580_CA_TAM) {
        w580_ca.mm_fuera++;
        w580_es_aviso(s, "cache", 0x200, "el MMIACC copia 0x%" PRIx64 " bytes de 0x%08x a 0x%08x, fuera de la memoria gráfica 0x%08x..0x%08x: el original solo "
                      "copia dentro de EXT_GRAM, y el modelo de caché no vigila otra zona", n, 0x20000000 + src, 0x20000000 + dst, W580_CA_BASE, W580_CA_BASE + W580_CA_TAM - 1);
        return false;
    }
    uint32_t so = src - b, dn = dst - b;
    W580CaLectura r = { 0 };
    g_autofree uint8_t *tmp = g_malloc(n);
    memcpy(tmp, w580_ca.vista + so, n);
    w580_ca_hw_lee(s, 0x20000000 + src, tmp, n, &r);
    unsigned d_sucias = 0, d_cache = 0, d_lin = 0;
    for (unsigned l = dn / W580_CA_LIN; l <= (dn + n - 1) / W580_CA_LIN; l++) {
        d_lin++;
        if (test_bit(l, w580_ca.sucia) && !w580_ca_diferida(s, l)) { w580_ca_baja(l); }
        if (test_bit(l, w580_ca.sucia)) { d_sucias++; }
        if (test_bit(l, w580_ca.en_cache)) { d_cache++; }
    }
    bool sin_dren = w580_ca.sin_drenar && w580_ca.limp_desde;
    w580_ca.mm_lin_dst += d_lin; w580_ca.mm_lin_dst_en_cache += d_cache;
    if (r.sucias) { w580_ca.mm_org_sucias++; }
    if (r.distintas) { w580_ca.mm_org_distintas++; }
    if (d_sucias) { w580_ca.mm_dst_sucias++; }
    if (d_cache) { w580_ca.mm_dst_en_cache++; }
    if (sin_dren) { w580_ca.mm_sin_drenar++; }
    if (w580_es.aprender && (w580_ca.n_log < 80 || ((r.sucias || d_sucias || sin_dren) && w580_ca.n_log < 600))) {
        w580_ca.n_log++;
        w580_es_suceso("cache", "mmiacc n=%" PRIu64 " destino=0x%08x origen=0x%08x bytes=%" PRIu64 " origen: lineas=%u sucias=%u distintas=%u | destino: lineas=%u "
                       "sucias=%u en_cache=%u | limpiezas_utiles_desde=%" PRIu64 " sin_drenar=%d pc=0x%08x", w580_ca.mm_n, 0x20000000 + dst, 0x20000000 + src, n,
                       r.lineas, r.sucias, r.distintas, d_lin, d_sucias, d_cache, w580_ca.limp_desde, sin_dren, w580_es_pc(s));
    }
    /* el destino: la SDRAM recibe la copia. Una línea que la CPU tiene SUCIA en caché conserva (para la CPU) lo suyo, y
     * al limpiarla machacará la copia: en estricto se deja así; aprendiendo, la copia llega a las dos vistas. */
    for (uint64_t i = 0; i < n; ) {
        unsigned l = (dn + i) / W580_CA_LIN; uint64_t fin = MIN(n, (uint64_t)(l + 1) * W580_CA_LIN - dn);
        memcpy(w580_ca.fisica + dn + i, tmp + i, fin - i);
        if (!(w580_es.on && test_bit(l, w580_ca.sucia))) {
            memcpy(w580_ca.vista + dn + i, tmp + i, fin - i);
            if (test_bit(l, w580_ca.sucia) && i == (uint64_t)l * W580_CA_LIN - dn && fin - i == W580_CA_LIN) { clear_bit(l, w580_ca.sucia); }
        }
        i = fin;
    }
    if (r.distintas) {
        w580_es_aviso(s, "cache", 0x201, "el MMIACC copia desde 0x%08x con %u de %u líneas de caché escritas por la CPU y sin limpiar (%u distintas de la SDRAM; "
                      "la primera 0x%08x): copia lo VIEJO. El original limpia el búfer antes de mandarlo y de copiarlo", 0x20000000 + src, r.sucias, r.lineas,
                      r.distintas, r.primera);
    }
    if (d_cache && !d_sucias) {
        w580_es_aviso(s, "cache", 0x203, "el MMIACC escribe en 0x%08x sobre %u de %u líneas que la CPU ha escrito (o leído, con estricto-cache-lecturas) desde la última vez que se invalidaron: la caché "
                      "puede conservarlas y la CPU seguiría leyendo lo de antes de la copia (y, al escribir encima, bajaría lo viejo). El original limpia E INVALIDA "
                      "el búfer de destino entero antes de cada copia (cp15 c7,c14,1 línea a línea, FUN_44a2c1a8)", 0x20000000 + dst, d_cache, d_lin);
    }
    if (d_sucias) {
        w580_es_aviso(s, "cache", 0x202, "el MMIACC escribe en 0x%08x sobre %u de %u líneas que la CPU tiene escritas y sin limpiar en la caché: cuando la caché "
                      "las baje machacarán la copia (y mientras, la CPU no ve lo copiado)", 0x20000000 + dst, d_sucias, d_lin);
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ NOR
 * El controlador del original (0x4414393C, 0x441439B8, en RAM) está escrito para una NOR mapeada CON caché: tras cada
 * orden «c7,c14,1 de su línea + c7,c10,4», y tras cada lectura de estado «c7,c6,1». Aquí: una orden escrita por una
 * página con alias de escritura diferida y la caché encendida tiene que limpiarse antes de la orden siguiente; y lo
 * programado en una página con alias con caché tiene que invalidarse antes de volver a leer (orden FF). [inferido
 * de ese código y de la arquitectura; el original NO lo ejercita: manda sus órdenes por páginas sin caché] */
static uint8_t w580_ca_nor_f(W580State *s, uint32_t abs)
{
    if (!w580_ca.hay || !w580_ca_sctlr_cache(s)) { return 0; }
    w580_ca_mapa(s);
    return w580_ca.zona[1].f[(abs & 0x03ffffff) >> 12];
}
static void w580_ca_nor_orden(W580State *s, uint32_t abs, uint8_t cmd, bool via04, bool es_orden)
{
    if (!w580_ca.hay) { return; }
    if (w580_ca.nor_pend) {
        w580_ca.nor_pend = false; w580_ca.nor_ord_sin_limpiar++;
        w580_es_aviso(s, "cache", 0x300, "la orden anterior a la NOR (0x%02x en 0x%08x) se escribió por una página con caché de escritura diferida (C=1, B=1) y "
                      "no se ha limpiado su línea (cp15 c7,c10,1 o c7,c14,1) antes de esta: en el aparato puede no haber llegado al chip. El original hace "
                      "c7,c14,1 + c7,c10,4 tras cada orden (0x441439F8)", w580_ca.nor_pend_cmd, 0x44000000 + w580_ca.nor_pend_dir);
    }
    if (w580_ca.nor_inv_pend && es_orden && cmd == 0xff) {
        w580_ca.nor_inv_pend = false; w580_ca.nor_prog_sin_invalidar++;
        w580_es_aviso(s, "cache", 0x301, "se vuelve a leer la NOR (orden FF) después de programar 0x%08x..0x%08x, que está mapeada CON caché, sin invalidar esas "
                      "líneas (cp15 c7,c6,1 o c7,c14,1; c7,c5,1 si es código): la CPU puede seguir viendo lo de antes", 0x44000000 + w580_ca.nor_inv_ini,
                      0x44000000 + w580_ca.nor_inv_fin - 1);
    }
    if (!via04 && (w580_ca_nor_f(s, abs) & CA_DIF)) {
        w580_ca.nor_ord_diferida++; w580_ca.nor_pend = true; w580_ca.nor_pend_dir = abs; w580_ca.nor_pend_cmd = cmd;
    }
}
static void w580_ca_nor_programa(W580State *s, uint32_t ini, uint32_t fin)
{
    if (!w580_ca.hay) { return; }
    if (w580_ca_nor_f(s, ini) & CA_C) {
        w580_ca.nor_prog_con_cache++; w580_ca.nor_inv_pend = true; w580_ca.nor_inv_ini = ini; w580_ca.nor_inv_fin = fin;
    }
}

/* ------------------------------------------------------------------------------------------------ volcado, reset, inicio */
static void w580_ca_volcar(GString *g)
{
    if (!w580_ca.hay) { return; }
    for (unsigned m = 0; m < 16; m++) { for (unsigned o = 0; o < 8; o++) {
        if (!w580_ca.c7[m][o]) { continue; }
        g_string_append_printf(g, "C7\tc7,c%u,%u\tn=%" PRIu64 "\tpc1=%08" PRIx64, m, o, w580_ca.c7[m][o], w580_ca.c7_pc[m][o]);
        for (unsigned z = 0; z < CA_NZ; z++) { if (w580_ca.c7_zona[m][o][z]) { g_string_append_printf(g, "\t%s=%" PRIu64, w580_ca_zona_nombre[z], w580_ca.c7_zona[m][o][z]); } }
        g_string_append_c(g, '\n');
    } }
    for (unsigned k = 0; k < G_N_ELEMENTS(w580_ca.pcs) && w580_ca.pcs[k].n; k++) {
        g_string_append_printf(g, "C7_VENTANA\tpc=%08x\tc7,c%u,1\tn=%" PRIu64 "\tsobre_linea_sucia=%" PRIu64 "\n", w580_ca.pcs[k].pc, w580_ca.pcs[k].crm,
                               w580_ca.pcs[k].n, w580_ca.pcs[k].utiles);
    }
    g_string_append_printf(g, "CACHE\tescrituras_con_cache=%" PRIu64 "\tescrituras_sin_cache=%" PRIu64 "\tlimpiezas_utiles=%" PRIu64 "\tlimpiezas_de_linea_limpia=%" PRIu64
                           "\tinvalidadas_sucias=%" PRIu64 "\tpor_indice=%" PRIu64 "\tpasadas_enteras=%" PRIu64 "\ttest_and_clean=%" PRIu64 "\tdrenados=%" PRIu64
                           "\tlineas_sucias_ahora=%lu\n", w580_ca.esc_cache, w580_ca.esc_sin, w580_ca.limpias_utiles, w580_ca.limpias_vacias, w580_ca.perdidas,
                           w580_ca.n_indice, w580_ca.n_pasadas, w580_ca.n_tc, w580_ca.n_dren, bitmap_count_one(w580_ca.sucia, W580_CA_NLIN));
    g_string_append_printf(g, "CACHE_PDICON\ttransferencias=%" PRIu64 "\tcon_lineas_sucias=%" PRIu64 "\tcon_lineas_distintas=%" PRIu64 "\tfuera_de_la_ventana=%" PRIu64
                           "\tsin_drenar_tras_limpiar=%" PRIu64 "\torigen_sin_escritura_diferida=%" PRIu64 "\tlineas_leidas=%" PRIu64 "\tlineas_sucias=%" PRIu64
                           "\tlineas_sucias_en_pagina_sin_cache=%" PRIu64 "\n", w580_ca.pd_n, w580_ca.pd_sucias, w580_ca.pd_distintas, w580_ca.pd_fuera,
                           w580_ca.pd_sin_drenar, w580_ca.pd_no_diferida, w580_ca.pd_lineas, w580_ca.pd_lin_sucias, w580_ca.pd_lin_resueltas);
    g_string_append_printf(g, "CACHE_MMIACC\tcopias=%" PRIu64 "\torigen_con_sucias=%" PRIu64 "\torigen_con_distintas=%" PRIu64 "\tdestino_con_sucias=%" PRIu64
                           "\tdestino_con_lineas_en_cache=%" PRIu64 "\tlineas_destino=%" PRIu64 "\tlineas_destino_en_cache=%" PRIu64 "\tfuera=%" PRIu64
                           "\tsin_drenar_tras_limpiar=%" PRIu64 "\n", w580_ca.mm_n, w580_ca.mm_org_sucias, w580_ca.mm_org_distintas, w580_ca.mm_dst_sucias,
                           w580_ca.mm_dst_en_cache, w580_ca.mm_lin_dst, w580_ca.mm_lin_dst_en_cache, w580_ca.mm_fuera, w580_ca.mm_sin_drenar);
    if (w580_ca.lecturas) { g_string_append_printf(g, "CACHE_LECTURAS\tlecturas_de_la_cpu_con_cache=%" PRIu64 "\n", w580_ca.lect_cache); }
    g_string_append_printf(g, "CACHE_NOR\tordenes_por_pagina_diferida=%" PRIu64 "\tsin_limpiar=%" PRIu64 "\tprogramas_en_pagina_con_cache=%" PRIu64
                           "\tsin_invalidar=%" PRIu64 "\n", w580_ca.nor_ord_diferida, w580_ca.nor_ord_sin_limpiar, w580_ca.nor_prog_con_cache, w580_ca.nor_prog_sin_invalidar);
}
static char *w580_get_es_cache(Object *obj, Error **errp)
{
    GString *g = g_string_new(NULL);
    if (!w580_ca.hay) { g_string_append(g, "cache: sin vigilar (hace falta estricto=on o estricto-aprender)\n"); }
    else {
        g_string_append_printf(g, "cache: SCTLR=0x%08x cache_de_datos=%d TTBR0=0x%08x\n", (unsigned)W580_MACHINE(obj)->cpu->env.cp15.sctlr_ns,
                               w580_ca_sctlr_cache(W580_MACHINE(obj)), (unsigned)W580_MACHINE(obj)->cpu->env.cp15.ttbr0_ns);
        w580_ca_volcar(g);
    }
    return g_string_free(g, false);
}
static void w580_ca_sdram(bool lista) { if (w580_ca.hay) { memory_region_set_enabled(&w580_ca.mr, lista); } }
static bool w580_get_ca_lecturas(Object *obj, Error **errp) { return w580_ca.lecturas; }
static void w580_set_ca_lecturas(Object *obj, bool value, Error **errp) { w580_ca.lecturas = value; }
static bool w580_get_ca_sordo(Object *obj, Error **errp) { return w580_ca.sordo; }
static void w580_set_ca_sordo(Object *obj, bool value, Error **errp) { w580_ca.sordo = value; }
static void w580_ca_reset(W580State *s)
{
    if (!w580_ca.hay) { return; }
    memcpy(w580_ca.fisica, w580_ca.vista, W580_CA_TAM);
    bitmap_zero(w580_ca.sucia, W580_CA_NLIN); bitmap_zero(w580_ca.en_cache, W580_CA_NLIN);
    w580_ca.mapa_ok = false; w580_ca.nor_pend = w580_ca.nor_inv_pend = false; w580_ca.sin_drenar = false; w580_ca.indice_seguidas = 0;
}
static void w580_ca_init(W580State *s)
{
    if (!w580_es_activo() || s->sin_ram || s->ram_base != 0x20000000) { return; }
    memory_region_init_rom_device(&w580_ca.mr, NULL, &w580_ca_ops, s, "w580.gram-vigilada", W580_CA_TAM, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), W580_CA_BASE, &w580_ca.mr, 5);
    w580_ca.vista = memory_region_get_ram_ptr(&w580_ca.mr);
    w580_ca.fisica = g_malloc0(W580_CA_TAM);
    w580_ca.sucia = bitmap_new(W580_CA_NLIN); w580_ca.en_cache = bitmap_new(W580_CA_NLIN);
    w580_ca.zona[0] = (W580CaZona){ W580_CA_BASE, W580_CA_TAM >> 12, g_malloc0(W580_CA_TAM >> 12), g_new0(uint32_t, W580_CA_TAM >> 12) };
    w580_ca.zona[1] = (W580CaZona){ 0x44000000, 0x04000000 >> 12, g_malloc0(0x04000000 >> 12), g_new0(uint32_t, 0x04000000 >> 12) };
    define_arm_cp_regs(s->cpu, w580_ca_c7_regs);
    if (w580_ca.lecturas) { memory_region_rom_device_set_romd(&w580_ca.mr, false); }       /* pista E6: también las lecturas de la CPU */
    w580_ca.hay = true;
}
