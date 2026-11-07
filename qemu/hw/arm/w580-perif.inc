/* Periféricos del DB2020 que este firmware ha pedido (pista E). SPDX-License-Identifier: GPL-2.0-or-later
 * Cada bloque lleva la evidencia que lo justifica. Lo que no está aquí cae en el cerrojo exploratorio. */

static bool w580_kp_read(W580State *s, hwaddr addr, unsigned size, uint64_t *out);
static bool w580_kp_write(W580State *s, hwaddr addr, unsigned size, uint64_t value);
static bool w580_uart_read(W580State *s, hwaddr addr, uint64_t *out);
static bool w580_uart_write(W580State *s, hwaddr addr, uint64_t value);
static uint16_t w580_uart_status(W580State *s, unsigned u);
static uint64_t w580_latch_get(W580State *s, hwaddr addr, unsigned size);
static void w580_latch_set(W580State *s, hwaddr addr, unsigned size, uint64_t value);
static void w580_apagado_mirar(W580State *s);
static uint8_t w580_gpio_pend(W580State *s, unsigned p);
static void w580_gpio_read_fix(W580State *s, hwaddr addr, uint64_t *out);
static void w580_reset_mirar(W580State *s, hwaddr addr, uint64_t value);
static void w580_tono_mirar(W580State *s, hwaddr addr, uint64_t value);

/* ---------------------------------------------------------------- controlador de interrupciones
 * 0xFB020100/200/300: tres bancos de 32 fuentes. El main lo llama «ti_intcon intcon_hw_layout:1to2
 * level:9,12,57,75,76,77» (registro de arranque): es el «interrupt handler» de dos niveles de TI
 * (el de los OMAP1; mismo mapa de registros que hw/intc/omap_intc.c de QEMU):
 *   +0x00 ITR  pendientes (escribir 0 en un bit borra una fuente de flanco: FB020200 = 0xF7FFFFFF)
 *   +0x04 MIR  máscara (1 = enmascarada)
 *   +0x10 SIR_IRQ / +0x14 SIR_FIQ  número de la fuente activa de más prioridad; LEERLO borra el
 *         pendiente si la fuente es de flanco [observado: sin eso el tic de OSE es una tormenta]
 *   +0x18 CONTROL  bit0 = «new IRQ agreement», bit1 = «new FIQ agreement»
 *   +0x1C.. ILRn   bit0 FIQ, bit1 sensibilidad (1 = nivel, 0 = flanco), bits 6:2 prioridad
 * Cascada [observado en e03: SIR raíz = 28 y luego se lee FB020210]: banco 1 -> fuente 28 de la raíz;
 * banco 2 -> fuente 30 [inferido del DB2010]. Numeración global del main: banco 0 = IRQ 0..27,
 * banco 1 = 28..59 (IRQ 53 = OSTICK = bit 25), banco 2 = 60.. (IRQ 77 = DMA = bit 17). */
static bool w580_rtc_irq_pending(W580State *s);
static void w580_intc_elige(W580State *s);
static uint32_t w580_intc_nivel_cfg(W580State *s, unsigned bank)
{
    uint32_t m = 0;
    for (unsigned i = 0; i < 32; i++) { if (s->irq_config[bank][i] & 2) { m |= 1u << i; } }
    return m;
}
static uint32_t w580_intc_pending(W580State *s, unsigned bank)
{
    /* (agente F, 1-nov) Una línea por NIVEL (ILR bit 1) es solo el nivel: el flanco que guarda w580_irq_linea no se borra nunca en ella (SIR solo borra los de flanco) y dejaba la línea
     * pendiente para siempre. La UART 3 por interrupción (línea 49, nivel, medido en el aparato) daba una tormenta de IRQ y colgaba el sistema al encender el Bluetooth. */
    uint32_t nivel = w580_intc_nivel_cfg(s, bank);
    uint32_t p = (s->irq_latch[bank] & ~nivel) | (s->irq_level[bank] & nivel);
    if (bank == 0) {
        if (w580_intc_pending(s, 1) & ~s->irq_mask[1]) { p |= 1u << 28; }
        if (w580_intc_pending(s, 2) & ~s->irq_mask[2]) { p |= 1u << 30; }
    }
    return p;
}
static void w580_irq_linea(W580State *s, unsigned bank, unsigned bit, bool on)
{
    uint32_t m = 1u << bit;
    if (on && !(s->irq_level[bank] & m)) { s->irq_latch[bank] |= m; }     /* flanco de subida */
    if (on) { s->irq_level[bank] |= m; } else { s->irq_level[bank] &= ~m; }
}
static void w580_irq_update(W580State *s)
{
    w580_irq_linea(s, 1, 8, w580_rtc_irq_pending(s));            /* IRQ 36: dda/RTC */
    w580_perif_lineas(s);
    w580_intc_elige(s);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ), (w580_intc_pending(s, 0) & ~s->irq_mask[0]) != 0);
}
static void w580_intc_log(W580State *s, const char *que, hwaddr addr, uint32_t value)
{
    /* las 3 primeras veces de cada (registro, sentido), como la E/S no modelada */
    static uint8_t visto[2][0x300 / 4];
    unsigned i = (addr - 0xfb020100) / 4, w = que[0] == 'e';
    if (i < 0x300 / 4 && visto[w][i] < 3) { visto[w][i]++;
        qemu_log_mask(LOG_UNIMP, "W580_INTC %s addr=0x%08x valor=0x%08x pc=0x%08x lr=0x%08x\n", que, (unsigned)addr, value,
                      s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
}
/* El despachador (ITCM 0xFA30..) lee el SIR de la raíz, ENMASCARA los tres bancos y solo entonces lee el SIR
 * del banco hijo: la fuente elegida se fija cuando hay algo activo y sobrevive al enmascarado. */
static void w580_intc_elige(W580State *s)
{
    for (int bank = 2; bank >= 0; bank--) {
        uint32_t active = w580_intc_pending(s, bank) & ~s->irq_mask[bank];
        if (!active) { continue; }
        unsigned mejor = 0, prio = 999;
        for (unsigned i = 0; i < 32; i++) {
            unsigned p = (s->irq_config[bank][i] >> 2) & 31;
            if ((active >> i & 1) && p < prio) { prio = p; mejor = i; }
        }
        s->irq_vector[bank] = mejor;
    }
}
static uint32_t w580_intc_sir(W580State *s, unsigned bank)
{
    w580_intc_elige(s);
    unsigned v = s->irq_vector[bank];
    if (!(s->irq_config[bank][v] & 2)) { s->irq_latch[bank] &= ~(1u << v); }   /* flanco: leer SIR lo borra */
    s->irq_cuenta[bank][v]++;
    return v;
}
static uint32_t w580_intc_read(W580State *s, hwaddr addr)
{
    unsigned bank = ((addr - 0xfb020000) >> 8) - 1, reg = addr & 255;
    uint32_t v = 0;
    if (reg == 0) { v = w580_intc_pending(s, bank); }
    else if (reg == 4) { v = s->irq_mask[bank]; }
    else if (reg == 0x10 || reg == 0x14) { v = w580_intc_sir(s, bank); w580_irq_update(s); }
    else if (reg >= 0x1c && reg < 0x9c) { v = s->irq_config[bank][(reg - 0x1c) / 4]; }
    w580_intc_log(s, "lee", addr, v);
    return v;
}
static void w580_intc_write(W580State *s, hwaddr addr, uint32_t value)
{
    unsigned bank = ((addr - 0xfb020000) >> 8) - 1, reg = addr & 255;
    w580_intc_log(s, "escribe", addr, value);
    if (reg == 0) { s->irq_latch[bank] &= value; }
    else if (reg == 4) { s->irq_mask[bank] = value; }
    else if (reg >= 0x1c && reg < 0x9c) { s->irq_config[bank][(reg - 0x1c) / 4] = value; }
    else if (reg == 0x9c) { s->irq_latch[bank] |= value; }      /* ISR: interrupción por software (como en el OMAP) */
    w580_irq_update(s);
}
static void w580_irq_pulso(W580State *s, unsigned bank, unsigned bit) { s->irq_latch[bank] |= 1u << bit; w580_irq_update(s); }
static char *w580_get_irqs(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    GString *g = g_string_new("");
    static const unsigned base[3] = { 0, 28, 60 };
    for (unsigned b = 0; b < 3; b++) {
        g_string_append_printf(g, "banco %u: pendiente=%08x mascara=%08x linea=%08x\n", b, w580_intc_pending(s, b), s->irq_mask[b], s->irq_level[b]);
        for (unsigned i = 0; i < 32; i++) {
            if (s->irq_cuenta[b][i]) { g_string_append_printf(g, "  IRQ %u (banco %u bit %u, ILR=%02x): %" PRIu64 " servidas\n", base[b] + i, b, i, s->irq_config[b][i], s->irq_cuenta[b][i]); }
        }
    }
    return g_string_free(g, false);
}

/* ---------------------------------------------------------------- temporizadores de 32 768 Hz
 * 0xF9010000 control (bit0 = TIMER1, bit1 = TIMER2; FUN_442368f8), +4/+8 comparadores,
 * +0x1C/+0x20 contadores (FUN_442369d0). Interrupción de TIMER1 = banco 1 bit 25, como en el
 * DB2010 (FUN_442328cc la desenmascara con «FB020204 &= 0xF7FFFFFF | 0x2000000»). */
/* Los dos son contadores de recarga automática: cuentan 0..comparador y vuelven a 0 generando la
 * interrupción. Evidencia: el tic de OSE (FUN_4423275c) NO reescribe el comparador en cada tic (solo
 * cada 124, para corregir la deriva: lee el contador recién entrada la ISR y espera un valor pequeño,
 * «0x85 - contador - …» ~ 0x82). Periodo = comparador + 1 = 131 tics de 32 768 Hz = 3,998 ms. */
/* Pista E6 (hallazgo 2 de la pista W): la época de cada temporizador es «instante base + cuentas de 32 768 Hz», y en cada
 * vencimiento avanza UN PERIODO EXACTO (comparador + 1 cuentas). Antes w580_tick hacía época = ahora, es decir, anclaba
 * el periodo siguiente al instante en que el hilo principal de QEMU llegaba a ejecutar la retrollamada: cada retraso
 * (despertar del hilo, una captura de pantalla por QMP…) se sumaba al tic de 4 ms y el sistema emulado iba un 1–7 % lento.
 * Si la retrollamada llega con más de un periodo de retraso se recuperan los tics perdidos de uno en uno, separados
 * W580_TIC_ALCANCE_NS (el vector de IRQ es de flanco: dos pulsos seguidos sin servir el primero se funden en uno); con
 * más de W580_TIC_PERDON_NS de atraso (el proceso estuvo detenido) se da por perdido y se vuelve a anclar.
 * El contador (+1C/+20) da lo que daría el hardware: cuentas desde el último vencimiento REAL, nunca más que el periodo. */
#define W580_TIC_ALCANCE_NS 250000
#define W580_TIC_PERDON_NS  INT64_C(2000000000)
#define W580_TIC_PACIENCIA  400          /* x W580_TIC_ALCANCE_NS = 100 ms esperando a que el hilo de la CPU sirva el tic anterior */
static int64_t w580_timer_epoca(W580State *s, unsigned t)
{
    return s->timer_epoch[t] + (int64_t)muldiv64(s->timer_cuentas[t], 1000000000, 32768);
}
static uint32_t w580_counter(W580State *s, unsigned t)
{
    int64_t d = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_timer_epoca(s, t);
    uint64_t c = d > 0 ? muldiv64(d, 32768, 1000000000) : 0;
    uint64_t periodo = (uint64_t)s->timer_compare[t] + 1;
    if ((s->timer_control & (1u << t)) && c >= periodo) { c %= periodo; }     /* vencido y aún sin atender: el contador ya dio la vuelta */
    return (uint32_t)c;
}
static void w580_timer_ancla(W580State *s, unsigned t)
{
    s->timer_reancla[t] = false; s->timer_aplaza[t] = 0;
    s->timer_epoch[t] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); s->timer_cuentas[t] = 0;
}
static void w580_timer_arm(W580State *s, unsigned t)
{
    QEMUTimer *tm = t ? s->timer2 : s->timer1;
    timer_del(tm);
    if (s->timer_control & (1u << t)) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t deadline = s->timer_epoch[t] + (int64_t)muldiv64(s->timer_cuentas[t] + s->timer_compare[t] + 1, 1000000000, 32768);
        /* comparador escrito por debajo de lo ya contado: vence enseguida y ESE vencimiento vuelve a anclar la época */
        s->timer_reancla[t] = deadline <= now;
        if (deadline <= now) { deadline = now + 30518; }
        timer_mod(tm, deadline);
    }
}
static void w580_tick(W580State *s, unsigned t)
{
    QEMUTimer *tm = t ? s->timer2 : s->timer1;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool sin_servir = s->irq_latch[1] & (1u << (25 + t));
    /* ¿Puede el invitado tomar ya la interrupción? Si la tiene tapada (CPSR.I, o la máscara del banco o de la cascada),
     * que se le junte con la siguiente es lo que pasaría en el aparato: se funden y pierde un tic. Si la tiene abierta y
     * aún no la ha servido, el retraso es de QEMU (el hilo de la CPU sin turno): se espera, sin dar el periodo por pasado. */
    bool abierta = !(s->cpu->env.daif & PSTATE_I) && !(s->irq_mask[1] & (1u << (25 + t))) && !(s->irq_mask[0] & (1u << 28));
    if (sin_servir && abierta && s->timer_aplaza[t] < W580_TIC_PACIENCIA) {
        s->timer_aplaza[t]++; s->timer_aplazados[t]++; timer_mod(tm, now + W580_TIC_ALCANCE_NS); return;
    }
    s->timer_aplaza[t] = 0;
    if (sin_servir) {
        s->timer_fundidos[t]++;
        if (s->timer_fundidos[t] <= 20) {
            qemu_log_mask(LOG_UNIMP, "W580_TIMER%u tic FUNDIDO con el anterior (sin servir en un periodo; %s) pc=0x%08x t=%" PRId64 " ms\n", t + 1,
                          abierta ? "abierta: se agoto la paciencia del modelo" : "IRQ tapada por el invitado", s->cpu->env.regs[15], now / 1000000);
        }
    }
    if (s->timer_reancla[t]) { w580_timer_ancla(s, t); s->timer_reancla[t] = false; }
    else { s->timer_cuentas[t] += (uint64_t)s->timer_compare[t] + 1; }       /* la época avanza un periodo justo */
    s->timer_tics[t]++;
    w580_irq_pulso(s, 1, 25 + t);          /* IRQ 53 (OSTICK) y 54 (TIMER02): «dda/TIMER … intline:53/54» */
    if (!(s->timer_control & (1u << t))) { return; }
    int64_t deadline = s->timer_epoch[t] + (int64_t)muldiv64(s->timer_cuentas[t] + s->timer_compare[t] + 1, 1000000000, 32768);
    if (deadline <= now) {                 /* llegamos tarde por más de un periodo */
        s->timer_atrasos[t]++;
        if (now - deadline > W580_TIC_PERDON_NS) {
            qemu_log_mask(LOG_UNIMP, "W580_TIMER%u atraso de %" PRId64 " ms: se da por perdido y se vuelve a anclar\n", t + 1, (now - deadline) / 1000000);
            w580_timer_ancla(s, t); s->timer_perdones[t]++;
            deadline = now + (int64_t)muldiv64((uint64_t)s->timer_compare[t] + 1, 1000000000, 32768);
        } else {
            deadline = now + W580_TIC_ALCANCE_NS;
        }
    }
    timer_mod(tm, deadline);
}
static char *w580_get_reloj_tic(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    GString *g = g_string_new("");
    for (unsigned t = 0; t < 2; t++) {
        g_string_append_printf(g, "temporizador %u: %s comparador=0x%x vencimientos=%" PRIu64 " con_atraso_de_mas_de_un_periodo=%" PRIu64 " perdidos_y_reanclado=%" PRIu64 " fundidos_con_el_anterior_sin_servir=%" PRIu64 " esperas_al_hilo_de_la_cpu=%" PRIu64 "\n",
                               t + 1, s->timer_control & (1u << t) ? "en marcha" : "parado", s->timer_compare[t], s->timer_tics[t], s->timer_atrasos[t], s->timer_perdones[t], s->timer_fundidos[t], s->timer_aplazados[t]);
    }
    return g_string_free(g, false);
}
static void w580_tick1(void *opaque) { w580_tick(opaque, 0); }
static void w580_tick2(void *opaque) { w580_tick(opaque, 1); }
static bool w580_timer_read(W580State *s, hwaddr addr, uint64_t *out)
{
    switch (addr) {
    case 0xf9010000: *out = s->timer_control; return true;
    case 0xf9010004: *out = s->timer_compare[0]; return true;
    case 0xf9010008: *out = s->timer_compare[1]; return true;
    case 0xf901001c: *out = w580_counter(s, 0); return true;
    case 0xf9010020: *out = w580_counter(s, 1); return true;
    }
    return false;
}
static bool w580_timer_write(W580State *s, hwaddr addr, uint64_t value)
{
    static unsigned n;
    if (addr >= 0xf9010000 && addr <= 0xf9010008 && n < 40) { n++;
        qemu_log_mask(LOG_UNIMP, "W580_TIMER escribe addr=0x%08x valor=0x%08x pc=0x%08x lr=0x%08x\n", (unsigned)addr,
                      (unsigned)value, s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    switch (addr) {
    case 0xf9010000:
        for (unsigned t = 0; t < 2; t++) {
            if (!(s->timer_control & (1u << t)) && (value & (1u << t))) {
                w580_timer_ancla(s, t); s->timer_expired[t] = false;
            }
        }
        s->timer_control = value;
        w580_timer_arm(s, 0); w580_timer_arm(s, 1);
        return true;
    case 0xf9010004:
        s->timer_compare[0] = value;
        w580_timer_arm(s, 0);
        return true;
    case 0xf9010008:
        s->timer_compare[1] = value;
        w580_timer_arm(s, 1);
        return true;
    }
    return false;
}

#include "w580-rtc.inc"
#include "w580-lcd.inc"
typedef struct W580I2C W580I2C;
#include "w580-acel.inc"
static void w580_ms_pmu(W580State *s, uint8_t reg, uint8_t antes, uint8_t value);     /* pista E9 */
#include "w580-i2c.inc"
#include "w580-ms.inc"             /* pista E9: Memory Stick; sin sus propiedades, lo de siempre */
#include "w580-usb.inc"            /* pista UEMU: USB de dispositivo; sin udc/usb-host, lo de siempre */
#include "w580-bt.inc"             /* pista BT: la UART 3 con el STLC2500 falso; sin bt-uart, lo de siempre */

/* ---------------------------------------------------------------- huecos del bus
 * El sondeo de memoria del arranque (FUN_44140240) prueba CS1 (0x48000000) y CS2 (0x4C000000)
 * como flash CFI y como RAM. Con cerrojos se «fabricaría» RAM ahí. [sin saber] qué hay de verdad:
 * provisionalmente flotan a unos y las escrituras se pierden. Lo mismo por encima de los 16 MiB
 * de SDRAM (MEMMAP_RAM_SDRAM_SIZE = 0x01000000, tabla de 0x45A8EC4C). */
static bool w580_hueco(hwaddr addr)
{
    return (addr >= 0x21000000 && addr < 0x40000000) || (addr >= 0x48000000 && addr < 0x58000000);
}

static bool w580_perif_read(W580State *s, hwaddr addr, unsigned size, uint64_t *out)
{
    if (addr >= 0xfb020100 && addr < 0xfb020400 && size == 4) { *out = w580_intc_read(s, addr); return true; }
    if (addr >= 0xf9010000 && addr < 0xf9010024 && w580_timer_read(s, addr, out)) { return true; }
    if (addr >= 0xfb080000 && addr < 0xfb080040) { w580_bt_es(s, addr, size, false, 0); }      /* pista BT: reglas [uart3] */
    if (addr >= 0xfb060000 && addr < 0xfb090000 && w580_uart_read(s, addr, out)) { return true; }
    if (w580_kp_read(s, addr, size, out)) { return true; }
    if (s->ms.modelo && (addr & ~(hwaddr)0xf) == 0xfc010000) { *out = w580_ms_lee(s, addr, size); return true; }      /* pista E9 */
    if (s->usb.modelo && (addr & ~(hwaddr)0xffff) == 0xfc030000) { *out = w580_usb_lee(s, addr, size); return true; }  /* pista UEMU */
    if (w580_hueco(addr)) {
        *out = size == 4 ? UINT32_MAX : ((UINT64_C(1) << (size * 8)) - 1);
        s->pag_r[addr >> 20]++;
        if (s->cs_log < 3000) { s->cs_log++;
            qemu_log_mask(LOG_UNIMP, "W580_HUECO read addr=0x%08x size=%u pc=0x%08x lr=0x%08x\n", (unsigned)addr, size,
                          s->cpu->env.regs[15], s->cpu->env.regs[14]);
        }
        return true;
    }
    return false;
}
static void w580_perif_read_fix(W580State *s, hwaddr addr, unsigned size, uint64_t *out)
{
    /* PLL: 0x44236580 escribe 0x2810 en F9090080 y espera activamente el bit 0 (enganchado). */
    if (addr == 0xf9090080 && (*out & 0x10)) { *out |= 1; }
    /* ID de chip: GetChipID (0x44235F30) lee 16 bits de F9090000. 0x9900 = «Marika P2A» en la tabla de BuildInfo
     * (FUN_4421e810) [dato de la pista H; el valor del aparato real, sin saber]. */
    if (addr == 0xf9090000 && size == 2) { *out = s->chip_id; }
    /* GAMCon: FUN_441ae818 exige (F7000000 & 0xFF00) == 0x4100 («Wrong GAMCon version read»). Solo el identificador. */
    if (addr == 0xf7000000 && size == 4) { *out = 0x4100; }
    if (addr >= 0xf9000000 && addr < 0xf9000032) { w580_gpio_read_fix(s, addr, out); }      /* entradas forzadas (gpio-in) */
}
/* ---------------------------------------------------------------- UART (sin nadie al otro lado)
 * Tabla de bases del main en 0x4427AEEC: unidad 0 = FB060000, 1 = FB070000, 2 = ausente, 3 = FB080000 (Bluetooth).
 * Líneas: «dda/UART0 … intline:46», UART1 47, UART3 49. Registros como en el DB2010 (w800-uart.inc): +2 dato TX,
 * +0x0E bit 4 = transmisor vacío (0x4422041C lo espera), +0x12 máscara, +0x14 estado (bit 1 = hay sitio para
 * transmitir), +0x2C sitio en la FIFO de TX, +0x2E bytes en la de RX. Lo transmitido se registra (W580_UARTn tx). */
static int w580_uart_idx(hwaddr addr)
{
    hwaddr b = addr & 0xffff0000;
    return (addr & 0xffff) >= 0x40 ? -1 : b == 0xfb060000 ? 0 : b == 0xfb070000 ? 1 : b == 0xfb080000 ? 2 : -1;
}
static uint16_t w580_uart_status(W580State *s, unsigned u)
{
    static const hwaddr base[3] = { 0xfb060000, 0xfb070000, 0xfb080000 };
    if (u == 2 && s->bt.modelo) {           /* pista BT: hay sitio = la FIFO de TX no está llena; hay datos = la de RX no está vacía */
        return ((s->bt.tx_n < s->bt.fifo ? 2 : 0) | (s->bt.rx_n >= w580_bt_umbral(s) ? 1 : 0) | s->uart_pending[u]) & w580_latch_get(s, base[u] + 0x12, 2);
    }
    return (2 | (u == 0 && s->uart0_rx_n ? 0x1041 : 0) | s->uart_pending[u]) & w580_latch_get(s, base[u] + 0x12, 2);
}
static void w580_uart_tx(W580State *s, unsigned u, uint8_t c)
{
    static const unsigned unidad[3] = { 0, 1, 3 };
    s->uart_tx_total[u]++;
    if (u == 0 && s->uart0_hay) { qemu_chr_fe_write_all(&s->uart0_chr, &c, 1); }
    s->uart_linea[u][s->uart_n[u]++] = c;
    if (s->uart_n[u] == 32 || c == '\n') {
        if (s->uart_log < 4000) { s->uart_log++;
            char hex[100], txt[40]; unsigned n = s->uart_n[u];
            for (unsigned i = 0; i < n; i++) {
                snprintf(hex + i * 2, 3, "%02x", s->uart_linea[u][i]);
                txt[i] = s->uart_linea[u][i] >= 32 && s->uart_linea[u][i] < 127 ? s->uart_linea[u][i] : '.';
            }
            txt[n] = 0;
            qemu_log_mask(LOG_UNIMP, "W580_UART%u tx %s |%s|\n", unidad[u], hex, txt);
        }
        s->uart_n[u] = 0;
    }
}
static bool w580_uart_read(W580State *s, hwaddr addr, uint64_t *out)
{
    int u = w580_uart_idx(addr);
    if (u < 0) { return false; }
    if (u == 2 && s->bt.modelo && w580_bt_lee(s, addr & 0xffff, out)) { return true; }      /* pista BT */
    switch (addr & 0xffff) {
    case 0x00:
        *out = 0;
        if (u == 0 && s->uart0_rx_n) {
            *out = s->uart0_rx[s->uart0_rx_head]; s->uart0_rx_head = (s->uart0_rx_head + 1) % sizeof(s->uart0_rx); s->uart0_rx_n--;
            w580_irq_update(s); qemu_chr_fe_accept_input(&s->uart0_chr);
        }
        return true;
    case 0x0e: *out = 0x10; return true;
    case 0x14: *out = w580_uart_status(s, u); return true;
    case 0x2c: *out = 256; return true;
    case 0x2e: *out = u == 0 ? s->uart0_rx_n : 0; return true;
    }
    return false;
}
static bool w580_uart_write(W580State *s, hwaddr addr, uint64_t value)
{
    int u = w580_uart_idx(addr);
    if (u < 0) { return false; }
    if (u == 2 && s->bt.modelo && w580_bt_escribe(s, addr & 0xffff, value)) { return true; }  /* pista BT */
    switch (addr & 0xffff) {
    case 0x02: w580_uart_tx(s, u, value); return true;
    case 0x14: s->uart_pending[u] &= ~value; w580_irq_update(s); return true;
    }
    return false;
}

/* ---------------------------------------------------------------- teclado (FB010000), matriz de 6 columnas x 5 filas
 * Registros de 32 bits [descripción de la pista H, docs/hardware.md, leída del pseudo-C; aquí se ejecuta]:
 *   +0  bits 0..15 control (bit 8 = rearme que escribe la ISR al salir, bit 9 = cliente registrado, bit 11);
 *       bits 16..21 = columnas excitadas a mano (a 0 la activa); bits 22..26 = filas leídas ahora (a 0 = tecla);
 *       bits 27..31 = filas con todas las columnas excitadas.
 *   +4  barrido automático: bit (columna*5 + fila) a 0 = tecla pulsada; en reposo 0x3FFFFFFF.
 * IRQ 60 (banco 2 bit 0) en cada cambio. [observado en e10] con el cerrojo, el barrido manual del arranque
 * (0x3E0924, 0x3D0924) leía las filas a 0 = todas las teclas pulsadas.
 * Celda = columna*5 + fila (propiedad kp-cell: +0x100 pulsa, sin él suelta, 0xFF suelta todo); 30..34 = teclas a masa. */
static uint32_t w580_kp_ctl(W580State *s)
{
    /* bits 27..31: las 5 teclas «a masa» (grupo 6 de FUN_441acf3c: ON_OFF, REWIND, FORWARD, TOGGLE, PLAYSTOP según
     * work/h2/teclas.tsv), NO «filas con todas las columnas»: [observado en e24/e25] con esa lectura cualquier tecla
     * de la matriz se convertía en una tecla a masa de su misma fila y además tapaba la de la matriz. */
    unsigned drive = (s->kp_control >> 16) & 0x3f, ahora = 0x1f, masa = 0x1f & ~w580_fa_kp_foto(s);      /* E8: kp-foto (nivel = lo de siempre) */
    for (unsigned c = 0; c < 6; c++) {
        if (!(drive >> c & 1)) { ahora &= ~w580_fa_kp_col(s, c); }
    }
    ahora &= ~w580_fa_kp_masa(s);     /* [pista S, hal.md §5] una tecla a masa tira de su fila con CUALQUIER patrón de columnas */
    return (s->kp_control & (s->fa.kp_foto_modo == W580_KF_REARME ? 0x3ffeff : 0x3fffff)) | (ahora << 22) | (masa << 27);   /* E8: en kp-foto=rearme el bit 8 se lee a 0 (un |= de otro bit no rearma) */
}
static uint32_t w580_kp_scan(W580State *s)
{
    uint32_t v = 0x3fffffff;
    for (unsigned c = 0; c < 6; c++) { v &= ~((uint32_t)(w580_fa_kp_col(s, c) & 0x1f) << (c * 5)); }
    return v;
}
static void w580_kp_debounce(void *opaque)
{
    W580State *s = opaque;
    w580_fa_kp_suceso(s);                        /* E8: la foto de los bits 27..31 (kp-foto) */
    if (s->kp_control & 0x200) { w580_irq_pulso(s, 2, 0); }
}
static bool w580_kp_read(W580State *s, hwaddr addr, unsigned size, uint64_t *out)
{
    if (addr < 0xfb010000 || addr >= 0xfb010008) { return false; }
    uint32_t v = (addr & 4) ? w580_kp_scan(s) : w580_kp_ctl(s);
    *out = (v >> ((addr & 3) * 8)) & (size == 4 ? UINT32_MAX : ((1u << (size * 8)) - 1));
    if (s->kp_log < 200) { s->kp_log++;
        qemu_log_mask(LOG_UNIMP, "W580_KP lee +%u/%u = 0x%08x pc=0x%08x lr=0x%08x\n", (unsigned)(addr & 7), size, (unsigned)*out,
                      s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    return true;
}
static bool w580_kp_write(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    if (addr != 0xfb010000 || size != 4) { return false; }
    if (s->kp_log < 200) { s->kp_log++;
        qemu_log_mask(LOG_UNIMP, "W580_KP escribe +0 = 0x%08x pc=0x%08x lr=0x%08x\n", (unsigned)value, s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    s->kp_control = value & 0x3fffff;
    w580_fa_kp_escribe(s, value);                /* E8: el rearme (bit 8) */
    return true;
}
static void w580_get_kp_cell(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    uint32_t value = w580_kp_scan(s);
    visit_type_uint32(v, name, &value, errp);
}
static void w580_pmu_tecla(W580State *s, bool abajo);
static void w580_set_kp_cell(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) { return; }
    unsigned cell = value & 0xff, col = cell / 5, row = cell % 5;
    if (cell == 0xff) { memset(s->kp_cols, 0, sizeof(s->kp_cols)); s->kp_masa = 0; w580_pmu_tecla(s, false); }
    else if (cell == 35) { w580_pmu_tecla(s, value & 0x100); }                 /* línea ON del PMU (suceso 0x21 bit 0) */
    else if (col == 6) { if (value & 0x100) { s->kp_masa |= 1u << row; } else { s->kp_masa &= ~(1u << row); } }
    else if (col > 5) { error_setg(errp, "kp-cell: 0..29 matriz, 30..34 teclas a masa, 35 = ON del PMU, 0xFF suelta todo"); return; }
    else if (value & 0x100) { s->kp_cols[col] |= 1u << row; }
    else { s->kp_cols[col] &= ~(1u << row); }
    timer_mod(s->kp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 5000000);
    qemu_log_mask(LOG_UNIMP, "W580_KP celda=%u columna=%u fila=%u pulsada=%u\n", cell, col, row, !!(value & 0x100));
    w580_lcd_traza(s, "TECLA\t%u\t%u", cell, !!(value & 0x100));
    w580_es_suceso("tecla", "celda=%u pulsada=%u", cell, !!(value & 0x100));
}

/* Pista E7: arrancar con teclas YA pulsadas. teclas-arranque=CELDA[:CELDA…] (30 = encendido a masa, 35 = línea ON del PMU)
 * las pulsa en CADA reset, antes de la primera instrucción, y teclas-arranque-ms=N las suelta a los N ms VIRTUALES del
 * reset (0 = no las suelta nadie: hay que soltarlas con kp-cell). Sirve para el «modo seguro por tecla al arrancar» y
 * para sostener la tecla de encendido un tiempo exacto. Sin la propiedad, el reset deja el teclado suelto, como antes. */
static void w580_kp_arranque_pon(W580State *s, bool abajo)
{
    if (!s->kp_arranque || !*s->kp_arranque) { return; }
    g_auto(GStrv) v = g_strsplit(s->kp_arranque, ":", -1);
    for (unsigned i = 0; v[i]; i++) {
        if (!*v[i]) { continue; }
        unsigned cell = strtoul(v[i], NULL, 0), col = cell / 5, row = cell % 5;
        if (cell == 35) {
            uint8_t *r = s->i2c[0].regs[0x4a];
            if (abajo) { r[0x23] |= 1; } else { w580_pmu_tecla(s, false); }      /* al pulsar en el reset no hay suceso nuevo: 0x21 bit 0 ya es la causa del encendido */
        } else if (col == 6) { if (abajo) { s->kp_masa |= 1u << row; } else { s->kp_masa &= ~(1u << row); } }
        else if (col < 6) { if (abajo) { s->kp_cols[col] |= 1u << row; } else { s->kp_cols[col] &= ~(1u << row); } }
        qemu_log_mask(LOG_UNIMP, "W580_KP teclas-arranque celda=%u pulsada=%d\n", cell, abajo);
        w580_es_suceso("tecla", "celda=%u pulsada=%d (teclas-arranque)", cell, abajo);
    }
}
static void w580_kp_arranque_suelta(void *opaque)
{
    W580State *s = opaque;
    w580_kp_arranque_pon(s, false);
    timer_mod(s->kp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 5000000);
}
static void w580_kp_arranque_reset(W580State *s)
{
    if (!s->kp_arranque || !*s->kp_arranque) { return; }
    w580_kp_arranque_pon(s, true);
    if (s->kp_arranque_ms) { timer_mod(s->kp_arranque_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)s->kp_arranque_ms * 1000000); }
}
static char *w580_get_kp_arranque(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->kp_arranque); }
static void w580_set_kp_arranque(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_auto(GStrv) v = g_strsplit(value, ":", -1);
    for (unsigned i = 0; v[i]; i++) {
        char *fin = NULL; unsigned long c = strtoul(v[i], &fin, 0);
        if (*v[i] && (*fin || c > 35)) { error_setg(errp, "teclas-arranque: celdas 0..35 separadas por ':' (30 = encendido, 35 = ON del PMU)"); return; }
    }
    g_free(s->kp_arranque); s->kp_arranque = g_strdup(value);
}
static void w580_get_u32p(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);
static bool w580_get_pmu_on_baja(Object *obj, Error **errp) { return W580_MACHINE(obj)->pmu_on_baja; }
static void w580_set_pmu_on_baja(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->pmu_on_baja = value; }
static void w580_kp_arranque_class_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "pmu-on-activa-baja", w580_get_pmu_on_baja, w580_set_pmu_on_baja);
    object_class_property_set_description(oc, "pmu-on-activa-baja", "(sis28) PMU 0x23 bit 0 como lo lee el firmware: on (por omision) = 0 PULSADA, 1 suelta, como "
                                          "el W580 medido el 2026-10-20; off = la polaridad de la pista E (1 = pulsada)");
    object_class_property_add_str(oc, "teclas-arranque", w580_get_kp_arranque, w580_set_kp_arranque);
    object_class_property_set_description(oc, "teclas-arranque", "Pista E7: celdas (CELDA[:CELDA]; 30 = encendido, 35 = ON del PMU) pulsadas desde cada reset, antes de la primera instruccion");
    object_class_property_add(oc, "teclas-arranque-ms", "uint32", w580_get_u32p, w580_get_u32p, NULL, (void *)offsetof(W580State, kp_arranque_ms));
    object_class_property_set_description(oc, "teclas-arranque-ms", "Pista E7: ms VIRTUALES desde el reset a los que se sueltan las teclas-arranque (0 = no se sueltan solas)");
}

/* ---------------------------------------------------------------- DSP: solo el puerto de memoria
 * FA040028 dirección (23 bits, en medias palabras), FA04002C dato, FA040024 orden (1 escribe, 2 lee) que
 * vuelve a 0 al terminar: FUN_441fe4dc la sondea. Igual que en el DB2010 (w800-dsp.inc de SEW800iEmu).
 * NO hay DSP: la memoria es un almacén pasivo; el buzón (FA040000..) queda en el cerrojo exploratorio. */
static bool w580_dsp_write(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    if (addr != 0xfa040024 || size != 4) { return false; }
    uint32_t index = (w580_latch_get(s, 0xfa040028, 4) & 0x7fffff) >> 1;
    if (!s->dsp_memory) { s->dsp_memory = g_new0(uint32_t, 0x400000); }
    if (value == 1) { s->dsp_memory[index] = w580_latch_get(s, 0xfa04002c, 4); s->dsp_writes++; }
    else if (value == 2) { w580_latch_set(s, 0xfa04002c, 4, s->dsp_memory[index]); s->dsp_reads++; }
    else { return false; }
    w580_latch_set(s, 0xfa040024, 4, 0);
    return true;
}
#include "w580-extra.inc"
#include "w580-luz.inc"
static bool w580_perif_write(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    if (addr >= 0xfa040000 && addr < 0xfa040100 && w580_dsp_write(s, addr, size, value)) { return true; }
    if (addr >= 0xf7001000 && addr < 0xf7001c00 && w580_lcd_write(s, addr, size, value)) { return true; }      /* PDI, PDICON y MMIACC */
    if (addr >= 0xfb080000 && addr < 0xfb080040) { w580_bt_es(s, addr, size, true, value); }   /* pista BT: reglas [uart3] */
    if (addr >= 0xfb060000 && addr < 0xfb090000 && w580_uart_write(s, addr, value)) { return true; }
    if (w580_kp_write(s, addr, size, value)) { return true; }
    if (s->ms.modelo && (addr & ~(hwaddr)0xf) == 0xfc010000) { w580_ms_escribe(s, addr, size, value); return true; }   /* pista E9 */
    if (s->usb.modelo && (addr & ~(hwaddr)0xffff) == 0xfc030000) { w580_usb_escribe(s, addr, size, value); return true; }  /* pista UEMU */
    if (addr == 0xf900000a || addr == 0xf900000c) { w580_acel_gpio(s, addr, value); }   /* GPIO1: solo se mira; sigue al cerrojo */
    if (addr == 0xf9010030 || addr == 0xf901002c || addr == 0xf9010014) { w580_reset_mirar(s, addr, value); }   /* reset-real; sigue al cerrojo */
    if (addr >= 0xf9020000 && addr < 0xf9020008) { w580_tono_mirar(s, addr, value); }     /* wav-tono; sigue al cerrojo */
    if (s->tsuc_on && addr >= 0xf9020000 && addr < 0xf9020008) { w580_tsuc_mirar(s, addr, size, value); }   /* tono-sucesos (pista W); sigue al cerrojo */
    if (addr >= 0xfb020100 && addr < 0xfb020400 && size == 4) { w580_intc_write(s, addr, value); return true; }
    if (addr >= 0xf9010000 && addr < 0xf9010024 && w580_timer_write(s, addr, value)) { return true; }
    if (w580_hueco(addr)) {
        s->pag_w[addr >> 20]++;
        if (s->cs_log < 3000) { s->cs_log++;
            qemu_log_mask(LOG_UNIMP, "W580_HUECO write addr=0x%08x size=%u value=0x%" PRIx64 " pc=0x%08x lr=0x%08x\n", (unsigned)addr,
                          size, value, s->cpu->env.regs[15], s->cpu->env.regs[14]);
        }
        return true;
    }
    return false;
}
static void w580_perif_reset(W580State *s)
{
    memset(s->irq_mask, 255, sizeof(s->irq_mask));
    memset(s->irq_latch, 0, sizeof(s->irq_latch));
    memset(s->irq_level, 0, sizeof(s->irq_level));
    memset(s->irq_config, 0, sizeof(s->irq_config));
    for (unsigned i = 0; i < 3; i++) { s->irq_vector[i] = 0; }
    s->timer_control = s->timer_compare[0] = s->timer_compare[1] = 0;
    s->timer_expired[0] = s->timer_expired[1] = false;
    w580_timer_ancla(s, 0); w580_timer_ancla(s, 1);
    timer_del(s->timer1); timer_del(s->timer2);
    w580_i2c_reset(s);
    w580_lcd_reset(s);
    w580_acel_reset(s);
    w580_extra_reset(s);
    s->kp_control = 0; s->kp_masa = 0; memset(s->kp_cols, 0, sizeof(s->kp_cols)); timer_del(s->kp_timer);
    timer_del(s->kp_arranque_timer); w580_kp_arranque_reset(s);      /* pista E7 */
    w580_fa_kp_reset(s);                         /* pista E8: teclas atascadas y la foto del reset */
    w580_ms_reset(s);                            /* pista E9 */
    w580_usb_reset(s);                           /* pista UEMU */
    w580_bt_reset(s);                            /* pista BT */
    w580_irq_update(s);
}
static void w580_perif_init(W580State *s, MemoryRegion *sysmem)
{
    s->timer1 = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_tick1, s);
    s->timer2 = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_tick2, s);
    w580_rtc_init(s, sysmem);
    w580_i2c_init(s, sysmem);
    w580_lcd_init(s);
    s->kp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_kp_debounce, s);
    s->kp_arranque_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_kp_arranque_suelta, s);
    w580_extra_init(s);
    w580_ms_init(s);                 /* pista E9 */
    w580_usb_init(s);                /* pista UEMU */
    w580_bt_init(s);                 /* pista BT */
}
/* i2c=0x4a,0x30… : esclavos I2C que contestan (bancos de registros a cero salvo lo medido) */
static char *w580_get_i2c(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->i2c_lista); }
static void w580_set_i2c(Object *obj, const char *value, Error **errp)
{
    /* «bus.dir:bus.dir…», p. ej. 0.0x4a:1.0x30 */
    W580State *s = W580_MACHINE(obj);
    g_free(s->i2c_lista); s->i2c_lista = g_strdup(value);
    memset(s->i2c[0].devs, 0, 16); memset(s->i2c[1].devs, 0, 16);
    g_auto(GStrv) v = g_strsplit(value, ":", -1);
    for (unsigned i = 0; v[i]; i++) {
        if (v[i][0] && v[i][1] == '.') {
            unsigned bus = (v[i][0] - '0') & 1, a = strtoul(v[i] + 2, NULL, 0) & 127;
            s->i2c[bus].devs[a >> 3] |= 1u << (a & 7);
        }
    }
}
static void w580_get_u8(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint8(v, name, (uint8_t *)((char *)obj + (uintptr_t)opaque), errp);
}
static void w580_get_u32p(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint32(v, name, (uint32_t *)((char *)obj + (uintptr_t)opaque), errp);
}
static void w580_perif_instance_init(W580State *s)
{
    /* PMU presente; batería quieta. Valores del ADC [inferido del W800: 3,9 V = 108; termistor 25 C = 38]: a calibrar
     * con lo que imprime el main («BattVoltageSample»). */
    s->i2c[0].devs[W580_PMU >> 3] |= 1u << (W580_PMU & 7);
    s->pmu_vbat = 108; s->pmu_id = 0xe0; s->chip_id = 0x9900; s->panel_id = 0x1505; s->panel_r28 = UINT32_MAX;
    memset(s->pmu_adc, 0x80, sizeof(s->pmu_adc));
    s->pmu_adc[1] = 108; s->pmu_adc[10] = 108; s->pmu_adc[11] = 38;
    s->bat.cargador_mv = 5000;
    s->pmu_on_baja = true;           /* (sis28) la tecla de encendido del PMU, activa a 0 como en el aparato */
    w580_fa_instance_init(s);        /* pista E8 */
    w580_ms_instance_init(s);        /* pista E9 */
    w580_acel_instance_init(s);
}
static void w580_perif_class_init(ObjectClass *oc)
{
    object_class_property_add(oc, "chip-id", "uint32", w580_get_u32p, w580_get_u32p, NULL, (void *)offsetof(W580State, chip_id));
    object_class_property_add(oc, "kp-cell", "uint32", w580_get_kp_cell, w580_set_kp_cell, NULL, NULL);
    object_class_property_set_description(oc, "kp-cell", "Teclado: columna*5+fila, +0x100 pulsa, sin el suelta; 30 = encendido; 0xFF suelta todo. Al leer: el barrido (+4)");
    object_class_property_add(oc, "panel-id", "uint32", w580_get_u32p, w580_get_u32p, NULL, (void *)offsetof(W580State, panel_id));
    object_class_property_add(oc, "panel-r28", "uint32", w580_get_u32p, w580_get_u32p, NULL, (void *)offsetof(W580State, panel_r28));
    object_class_property_set_description(oc, "panel-r28", "Lo que contesta el registro 0x0028 del panel al leerlo (0xFFFFFFFF, por omision: lo ultimo escrito)");
    object_class_property_add_str(oc, "pantalla", w580_get_pantalla, NULL);
    object_class_property_set_description(oc, "pantalla", "Cuadros enviados por el PDICON y pixeles no negros del ultimo");
    w580_lcd_class_init(oc);
    object_class_property_add(oc, "pmu-vbat", "uint8", w580_get_u8, w580_get_u8, NULL, (void *)offsetof(W580State, pmu_vbat));
    object_class_property_add(oc, "pmu-id", "uint8", w580_get_u8, w580_get_u8, NULL, (void *)offsetof(W580State, pmu_id));
    for (unsigned i = 0; i < 16; i++) {
        g_autofree char *n = g_strdup_printf("pmu-adc%u", i);
        object_class_property_add(oc, n, "uint8", w580_get_u8, w580_get_u8, NULL, (void *)(offsetof(W580State, pmu_adc) + i));
    }
    w580_acel_class_init(oc);
    w580_extra_class_init(oc);
    w580_bat_class_init(oc);
    w580_fa_class_init(oc);          /* pista E8 */
    w580_ms_class_init(oc);          /* pista E9 */
    w580_usb_class_init(oc);         /* pista UEMU */
    w580_bt_class_init(oc);          /* pista BT */
    w580_kp_arranque_class_init(oc);
    w580_luz_class_init(oc);
    object_class_property_add_str(oc, "i2c", w580_get_i2c, w580_set_i2c);
    object_class_property_set_description(oc, "i2c", "Esclavos I2C presentes: bus.direccion separados por ':' (p. ej. 0.0x4a:1.0x30)");
    object_class_property_add_str(oc, "reloj-tic", w580_get_reloj_tic, NULL);
    object_class_property_set_description(oc, "reloj-tic", "Pista E6: temporizadores de 32 768 Hz: vencimientos, los que llegaron con mas de un periodo de atraso y los dados por perdidos");
    object_class_property_add_str(oc, "irqs", w580_get_irqs, NULL);
    object_class_property_set_description(oc, "irqs", "Estado del controlador de interrupciones y cuenta de fuentes servidas");
}
/* Líneas de interrupción de nivel de los periféricos modelados (se llama desde w580_irq_update). */
static void w580_perif_lineas(W580State *s)
{
    /* I2C0/I2C1: «OS: Vector 70/71 Pri 30 do not use DDA» [inferido: 70 era la del I2C en el DB2010] = banco 2 bits 10 y 11 */
    for (unsigned i = 0; i < 2; i++) {
        w580_irq_linea(s, 2, 10 + i, s->i2c[i].pending && (s->i2c[i].control & 0x80));
    }
    {   /* IRQ 37 «VINCENNE»: sucesos 0x21/0x22 no enmascarados por 0x26/0x27 (como en el DB2010) */
        uint8_t *r = s->i2c[0].regs[W580_PMU];
        /* Pista E7: los sucesos de cargador (0x21 bit 3) y de USB (bit 6) —que solo existen con las propiedades cargador / usb-vbus—
         * interrumpen según la máscara 0x24, no la 0x26: [observado por la pista B en el original, work/b/registros_pmu.tsv] el
         * manejador FUN_442342b4 tapa esos dos bits en 0x24 durante el antirrebote (0xA4C/0xA4D) y PD_MIXSIG deja 0x24 = EF y luego
         * 00; con 0x26 = 08 (lo que escribe la lista) el suceso de cargador no interrumpiría nunca. Qué son 0x26/0x27: [sin saber];
         * para la tecla (bit 0) se deja lo de siempre. */
        w580_irq_linea(s, 1, 37 - 28, (r[0x21] & ~r[0x26]) || (r[0x22] & ~r[0x27]) || (r[0x21] & 0x48 & ~r[0x24]));
    }
    for (unsigned p = 0; p < 5; p++) {      /* IRQ 65..69: solo los puertos con entradas forzadas (gpio-in) */
        if (s->gpio_in_mask[p]) { w580_irq_linea(s, 2, 5 + p, w580_gpio_pend(s, p) != 0); }
    }
    if (s->ms.modelo) { w580_ms_linea(s); }      /* pista E9: IRQ 76 «dda/MEMORY_STICK … intline:76» */
    if (s->usb.modelo) { w580_usb_lineas(s); }   /* pista UEMU: IRQ 62/63/64 «dda/USB … intline_geni:62 … non_iso:63 … iso:64» */
    static const unsigned bit[3] = { 46 - 28, 47 - 28, 49 - 28 };
    for (unsigned u = 0; u < 3; u++) { w580_irq_linea(s, 1, bit[u], w580_uart_status(s, u) != 0); }
}
