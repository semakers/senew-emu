#ifndef W580_ES_PARTE_NOR
/* W580 (pista E4): MODO ESTRICTO. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * La máquina w580 es permisiva justo donde el aparato no perdona: la SDRAM está desde el reset, SYSCON, el EMC y el
 * perro son cerrojos, la NOR acepta cualquier cosa y el ADC del PMU contesta sin su pulso. Con «estricto=on»
 * (apagada por omisión: sin ella NADA de este fichero cambia el comportamiento) cada regla de aquí AVISA
 * («W580_ESTRICTO [regla] …», registro de QEMU con -d guest_errors y propiedades «estricto-avisos» /
 * «estricto-resumen») cuando el firmware hace algo que el main ORIGINAL (R8BE001) no hace nunca, o lo hace en un
 * orden que el hardware no permite.
 *
 * Criterio (el del W600, docs/emulador_estricto.md): una regla entra si el original la cumple —arranca igual y con
 * 0 avisos— y hay de dónde sacarla (su código o la hoja del chip). El castigo (SDRAM que no contesta, bloque de la
 * NOR que no se programa, dato viejo del ADC, reset) es el modelo MÁS PROBABLE, no una medida: nada de esto se ha
 * visto en un aparato.
 *
 * «estricto-aprender=RUTA» no cambia nada: cuenta lo que hace el firmware (cada registro tocado con el estado de las
 * puertas de SYSCON en ese momento, EMC, PLL, perro, NOR, I2C, ADC) y lo vuelca en RUTA al salir o al escribir la
 * propiedad «estricto-volcar». De ahí salen las tablas w580-estricto-registros.inc (tools/e4_tablas.py).
 */
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "accel/tcg/cpu-ops.h"

static uint64_t w580_latch_get(W580State *s, hwaddr addr, unsigned size);

#define W580_ES_NCERR 5         /* palabras con puertas: SYSCON +16, +18, +1A, +1C (bit 10), +3A (bit 8) */
static const uint32_t w580_es_cerr_dir[W580_ES_NCERR] = { 0xf9090016, 0xf9090018, 0xf909001a, 0xf909001c, 0xf909003a };
typedef struct {
    uint32_t n, n_tras, pc1, lr1; int64_t t1; uint8_t tam;
    uint16_t y_cerr[W580_ES_NCERR], o_cerr[W580_ES_NCERR], y_hab[3], o_hab[3];
    uint32_t val[6]; uint8_t nval;
} W580EsReg;
typedef struct { char *regla, *texto; uint32_t pc, lr, n; double t1; unsigned orden; } W580EsAviso;
typedef struct { uint32_t n, pc1; uint8_t val[8], nval; } W580EsI2C;

/* tablas generadas del original (tools/e4_tablas.py): registros y megabytes que el original toca */
typedef struct { uint32_t dir; uint8_t f; } W580EsLista;     /* f: 1 lee, 2 escribe (ejecutado); 4 lee, 8 escribe (solo en su código) */
#include "w580-estricto-registros.inc"
/* tabla de puertas de reloj: bloque -> (palabra de puerta, bit; palabra de reset, bit); -1 = sin esa mitad */
typedef struct { uint32_t ini, fin; int8_t gr, gb, hr, hb; const char *nombre, *quien; } W580EsPuerta;
static const W580EsPuerta w580_es_puertas[] = {
#include "w580-estricto-puertas.inc"
};

#define W580_ES_SD_ESPERA_NS 40000      /* pista E6, regla [espera]: mitad de lo que tarda el original con reloj-cpu (84 µs) */
/* hitos del encendido de la SDRAM (rutina de la ITCM del original, docs/hal.md §1.3) */
enum { SD_HAB = 1, SD_CFG = 2, SD_PRE = 4, SD_REF = 8, SD_RFR = 16, SD_TIE = 32, SD_FIN = 64, SD_TODO = 127 };

static struct {
    bool on, aprender;
    char *ruta;
    uint32_t semilla;
    GString *avisos, *sucesos;
    GHashTable *vistos, *regs, *i2c;
    unsigned n_avisos, n_callados, n_orden, resets, n_sucesos, n_puerta, n_dsp;
    GHashTable *nack;
    int64_t t_reset;
    bool syscon_visto, basura_hecha;
    /* a. memoria */
    uint8_t sd_hitos; unsigned sd_ref; bool sd_lista; uint64_t sd_antes, sd_accesos_cerrada; int64_t sd_t_cfg, sd_t_tie;
    /* b. PLL */
    int64_t pll_t; bool pll_visto, pll_en_uso; unsigned mhz;
    /* c. perro */
    uint8_t wd_paso; bool wd_armado, wd_llave, wd_30_cero; int64_t wd_vence; QEMUTimer *wd_timer; unsigned wd_resets;
    /* alimentación */
    QEMUTimer *pw_timer;
    /* e. ADC */
    int adc_pedido; bool adc_p11, adc_hecho; int64_t adc_t11, adc_t12, adc_t_sel;
    uint64_t adc_sel, adc_pulsos, adc_lect, adc_lect_sin, adc_6a, adc_pulso_sin_puerta;
    int64_t adc_min_ancho, adc_max_ancho, adc_min_lee, adc_max_lee;
    /* f. registros */
    uint64_t n_solo_estatico;
    uint32_t pc_forzado;        /* pista E7: un aviso que salta por temporizador lleva el PC de la escritura que lo armó */
} w580_es;

static bool w580_es_activo(void) { return w580_es.on || w580_es.aprender; }
static uint32_t w580_es_pc(W580State *s)
{
    CPUState *cs = CPU(s->cpu);
    uint64_t data[8];
    if (w580_es.pc_forzado) { return w580_es.pc_forzado; }
    /* con CF_PCREL (ARM) el dato de la instrucción es el desplazamiento dentro de la página; la página la lleva regs[15] */
    if (current_cpu && cs->mem_io_pc && cpu_unwind_state_data(cs, cs->mem_io_pc, data)) {
        return (s->cpu->env.regs[15] & TARGET_PAGE_MASK) | (data[0] & ~TARGET_PAGE_MASK);
    }
    return s->cpu->env.regs[15];
}
static double w580_es_t(void) { return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_es.t_reset) / 1e9; }
static unsigned w580_es_l16(W580State *s, uint32_t addr) { return w580_latch_get(s, addr, 2); }
static uint32_t w580_es_l32(W580State *s, uint32_t addr) { return w580_latch_get(s, addr, 4); }

/* Un aviso. Cada (regla, clave, pc) sale 2 veces en el registro y se calla; el resumen los cuenta todos. */
static void G_GNUC_PRINTF(4, 5) w580_es_aviso(W580State *s, const char *regla, uint32_t clave, const char *fmt, ...)
{
    if (!w580_es.on) { return; }
    uint32_t pc = w580_es_pc(s);
    g_autofree char *k = g_strdup_printf("%s/%08x/%08x", regla, clave, pc);
    if (!w580_es.vistos) { w580_es.vistos = g_hash_table_new(g_str_hash, g_str_equal); }
    W580EsAviso *a = g_hash_table_lookup(w580_es.vistos, k);
    w580_es.n_avisos++;
    if (a && a->n >= 2) { a->n++; w580_es.n_callados++; return; }
    va_list ap; va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (!a) {
        if (g_hash_table_size(w580_es.vistos) >= 4000) { w580_es.n_callados++; return; }
        a = g_new0(W580EsAviso, 1);
        a->regla = g_strdup(regla); a->texto = g_strdup(msg); a->pc = pc; a->lr = s->cpu->env.regs[14];
        a->t1 = w580_es_t(); a->orden = w580_es.n_orden++;
        g_hash_table_insert(w580_es.vistos, g_strdup(k), a);
    }
    a->n++;
    g_autofree char *linea = g_strdup_printf("W580_ESTRICTO [%s] %s (pc=0x%08x lr=0x%08x t=%.4f s)%s\n", regla, msg,
        pc, s->cpu->env.regs[14], w580_es_t(), a->n == 2 ? " [ya no se repite]" : "");
    qemu_log_mask(LOG_GUEST_ERROR, "%s", linea);
    if (!w580_es.avisos) { w580_es.avisos = g_string_new(NULL); }
    if (w580_es.avisos->len < 256 * 1024) { g_string_append(w580_es.avisos, linea); }
}
/* Un suceso del modo «aprender» (no es un aviso). */
static void G_GNUC_PRINTF(2, 3) w580_es_suceso(const char *clase, const char *fmt, ...)
{
    if (!w580_es.aprender) { return; }
    if (!w580_es.sucesos) { w580_es.sucesos = g_string_new(NULL); }
    if (w580_es.sucesos->len > 8 * 1024 * 1024) { return; }
    va_list ap; va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    w580_es.n_sucesos++;
    g_string_append_printf(w580_es.sucesos, "SUCESO\t%.9f\t%s\t%s\n", w580_es_t(), clase, msg);     /* ns: con -icount los plazos son de decenas de instrucciones */
}
static uint32_t w580_es_basura(uint32_t x)
{
    x ^= w580_es.semilla; x ^= x << 13; x ^= x >> 17; x ^= x << 5; x *= 0x9e3779b1u; return x ^ (x >> 15);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Estadística por registro (aprender) */
static void w580_es_cuenta(W580State *s, uint32_t addr, unsigned size, bool write, uint64_t value)
{
    uint64_t key = ((uint64_t)write << 32) | addr;
    if (!w580_es.regs) { w580_es.regs = g_hash_table_new(g_int64_hash, g_int64_equal); }
    W580EsReg *e = g_hash_table_lookup(w580_es.regs, &key);
    if (!e) {
        if (g_hash_table_size(w580_es.regs) >= 60000) { return; }
        e = g_new0(W580EsReg, 1);
        memset(e->y_cerr, 255, sizeof(e->y_cerr)); memset(e->y_hab, 255, sizeof(e->y_hab));
        e->pc1 = w580_es_pc(s); e->lr1 = s->cpu->env.regs[14]; e->t1 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_es.t_reset; e->tam = size;
        uint64_t *k = g_new(uint64_t, 1); *k = key;
        g_hash_table_insert(w580_es.regs, k, e);
    }
    e->n++;
    if (e->nval < 6) {
        unsigned i; for (i = 0; i < e->nval && e->val[i] != (uint32_t)value; i++) { }
        if (i == e->nval) { e->val[e->nval++] = value; }
    }
    if (!w580_es.syscon_visto) { return; }
    e->n_tras++;
    for (unsigned i = 0; i < W580_ES_NCERR; i++) { uint16_t c = w580_es_l16(s, w580_es_cerr_dir[i]); e->y_cerr[i] &= c; e->o_cerr[i] |= c; }
    for (unsigned i = 0; i < 3; i++) { uint16_t h = w580_es_l16(s, 0xf9090010 + 2 * i); e->y_hab[i] &= h; e->o_hab[i] |= h; }
}

/* ---------------------------------------------------------------------------------------------------------------
 * a. Memoria: la SDRAM no existe hasta que el EMC la enciende (rutina de la ITCM 0xFBF8 del original, copia en flash
 * 0x442BBEA4; docs/hal.md §1.3):  EMC+00 bit 5 (habilitar) · +30 (configuración) · +40 <- 1 (precarga) · +40 <- 2 al
 * menos dos veces (refresco; el original manda 8) · +34 (refresco) · +3C (tiempos) · +00 |= 0x40 y |= 0x1F.
 * La precarga y los dos refrescos son lo que pide CUALQUIER SDRAM (JEDEC); el resto es «lo que hace el original». */
static const char *w580_es_sd_falta(void)
{
    static char b[200];
    unsigned f = SD_TODO & ~w580_es.sd_hitos;
    snprintf(b, sizeof(b), "%s%s%s%s%s%s%s",
             f & SD_HAB ? " EMC+00 bit 5 (habilitar);" : "", f & SD_CFG ? " EMC+30 (configuración);" : "",
             f & SD_PRE ? " EMC+40 <- 1 (precarga);" : "", f & SD_REF ? " dos EMC+40 <- 2 (refresco);" : "",
             f & SD_RFR ? " EMC+34;" : "", f & SD_TIE ? " EMC+3C (tiempos);" : "", f & SD_FIN ? " EMC+00 |= 0x40 y |= 0x1F;" : "");
    return b;
}
static void w580_ca_sdram(bool lista);
static void w580_es_sd_mira(W580State *s)
{
    bool lista = w580_es.sd_hitos == SD_TODO;
    if (lista == w580_es.sd_lista) { return; }
    w580_es.sd_lista = lista;
    w580_es_suceso("sdram", "%s pc=0x%08x accesos_antes=%" PRIu64, lista ? "ENCENDIDA" : "apagada", w580_es_pc(s), w580_es.sd_antes);
    if (w580_es.on && !s->sin_ram) { memory_region_set_enabled(MACHINE(s)->ram, lista); w580_ca_sdram(lista); }
}
/* los tres registros de la SDRAM solo llevan dos juegos de valores en el original: 13 MHz y 52 MHz de bus (FUN_441405e4
 * los calcula del descriptor 0x44140FB8). Su significado bit a bit es [sin saber]: otro valor no se puede dar por bueno. */
static void w580_es_emc_valor(W580State *s, uint32_t addr, uint32_t value, uint32_t v13, uint32_t v52)
{
    if (value != v13 && value != v52) {
        w580_es_aviso(s, "sdram", addr & 0xff, "EMC+%02X = 0x%08x: el original solo escribe ahí 0x%08x (bus a 13 MHz) o 0x%08x (a 52 MHz)", addr & 0xff, value, v13, v52);
    }
}
static void w580_es_emc_escribe(W580State *s, uint32_t addr, uint32_t value)
{
    uint32_t antes = w580_es_l32(s, addr);
    w580_es_suceso("emc", "W +%02x = 0x%08x (antes 0x%08x) pc=0x%08x", addr - 0x14000000, value, antes, w580_es_pc(s));
    switch (addr) {
    case 0x14000000:
        if (value & 0x20) { w580_es.sd_hitos |= SD_HAB; }
        else {
            if (w580_es.sd_lista) { w580_es_aviso(s, "sdram", 3, "EMC+00 = 0x%x quita el bit 5 con la SDRAM encendida: se pierde la RAM", value); }
            w580_es.sd_hitos = 0; w580_es.sd_ref = 0;
        }
        if ((value & 0x5f) == 0x5f) {
            int64_t d = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_es.sd_t_tie;       /* pista E6, [espera]: de los tiempos (EMC+3C) a darla por encendida */
            if (!(w580_es.sd_hitos & SD_FIN)) { w580_es_suceso("espera", "EMC+3C -> SDRAM en uso: %" PRId64 " ns", d); }
            if (w580_reloj_activo(s) && !(w580_es.sd_hitos & SD_FIN) && (w580_es.sd_hitos & SD_TIE) && d < W580_ES_SD_ESPERA_NS) {
                w580_es_aviso(s, "espera", 2, "SDRAM: se da por encendida (EMC+00 con 0x5F) a %" PRId64 " us de escribir sus tiempos (EMC+3C); el original espera ~84 us de este reloj y aqui se exigen %d", d / 1000, W580_ES_SD_ESPERA_NS / 1000);
            }
            w580_es.sd_hitos |= SD_FIN;
        } else { w580_es.sd_hitos &= ~SD_FIN; }
        break;
    case 0x14000030: w580_es.sd_hitos |= SD_CFG; w580_es_emc_valor(s, addr, value, 0x123d00c0, 0x12310303); w580_es.sd_t_cfg = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); break;
    case 0x14000034: w580_es.sd_hitos |= SD_RFR; w580_es_emc_valor(s, addr, value, 0x27, 0x37); break;
    case 0x1400003c: w580_es.sd_hitos |= SD_TIE; w580_es_emc_valor(s, addr, value, 0x20408108, 0x83060c1a); w580_es.sd_t_tie = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); break;
    case 0x14000040:
        if (!(w580_es.sd_hitos & SD_HAB)) { break; }
        if (value == 1) {
            /* Pista E6, regla [espera] (solo con el reloj de CPU según SYSCON, `reloj-cpu=on` + -icount; sin él el plazo es tiempo
             * del anfitrión y no dice nada): de configurar la SDRAM (EMC+30) a la precarga el original da 326 vueltas de 4
             * instrucciones a 13 MHz (0x442BBEEC; ~1956 ciclos = 150 µs; 84 µs en este reloj, que cuenta instrucciones a 64 ns).
             * Se exige la mitad de lo que tarda el original aquí [inferido el umbral]: JEDEC pide >= 100 µs de pausa. */
            int64_t d = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_es.sd_t_cfg;
            if (!(w580_es.sd_hitos & SD_PRE)) { w580_es_suceso("espera", "EMC+30 -> precarga: %" PRId64 " ns", d); }
            if (w580_reloj_activo(s) && !(w580_es.sd_hitos & SD_PRE) && w580_es.sd_t_cfg && d < W580_ES_SD_ESPERA_NS) {
                w580_es_aviso(s, "espera", 1, "SDRAM: precarga (EMC+40 <- 1) a %" PRId64 " us de configurarla (EMC+30); el original espera ~84 us de este reloj (326 vueltas a 13 MHz) y aqui se exigen %d", d / 1000, W580_ES_SD_ESPERA_NS / 1000);
            }
            w580_es.sd_hitos |= SD_PRE;
        }
        else if (value == 2 && (w580_es.sd_hitos & SD_PRE) && ++w580_es.sd_ref >= 2) { w580_es.sd_hitos |= SD_REF; }
        break;
    }
    w580_es_sd_mira(s);
}
/* acceso a 0x20000000..0x20FFFFFF con la RAM deshabilitada (solo llega aquí en estricto) */
static bool w580_es_sdram_cerrada(W580State *s, hwaddr addr, unsigned size, bool write, uint64_t *value)
{
    w580_es.sd_accesos_cerrada++;
    w580_es_aviso(s, "sdram", 1, "%s de 0x%08" HWADDR_PRIx " con la SDRAM sin encender; falta:%s (el original la enciende desde la ITCM antes de tocarla). "
                  "%s", write ? "escritura" : "lectura", addr, w580_es_sd_falta(), write ? "Se pierde" : "Lee basura");
    if (!write) { *value = w580_es_basura(addr) & (size >= 4 ? UINT32_MAX : (1u << (size * 8)) - 1); }
    return true;
}

/* ---------------------------------------------------------------------------------------------------------------
 * b. PLL (docs/hal.md §2.2; FUN_44236530). El original, para subir: EMC a 52 MHz de bus ANTES (EMC+30 = 0x12310303,
 * +34 = 0x37, +3C = 0x83060C1A, desde la ITCM), F9090054 <- 3 (208) o +1C bit 4 (104), F9090080 <- 0x2810, espera
 * ACTIVA a F9090080 bit 0 y solo entonces F909001C |= 1. El bit 0 tarda W580_ES_PLL_NS en subir [inferido el tiempo]. */
#define W580_ES_PLL_NS 150000
static bool w580_es_pll_enganchado(W580State *s)
{
    bool ok = w580_es.pll_t && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_es.pll_t >= W580_ES_PLL_NS;
    if (ok) { w580_es.pll_visto = true; }
    return ok;
}
static void w580_es_syscon_escribe(W580State *s, uint32_t addr, unsigned size, uint32_t value)
{
    unsigned antes = w580_es_l16(s, addr);
    w580_ms_es_syscon(s, addr, value);          /* pista E9: reglas [ms] */
    if (addr == 0xf9090016 || addr == 0xf9090010) { w580_es.syscon_visto = true; }
    if (addr == 0xf9090080 || addr == 0xf909001c || addr == 0xf9090054 || addr == 0xf9090038 || addr == 0xf9090050 || addr == 0xf9090052) {
        w580_es_suceso("pll", "W F90900%02x = 0x%04x (antes 0x%04x) EMC+30=0x%08x +3C=0x%08x +1C=0x%04x +54=0x%x pc=0x%08x", addr & 255, value, antes,
                       w580_es_l32(s, 0x14000030), w580_es_l32(s, 0x1400003c), w580_es_l16(s, 0xf909001c), w580_es_l16(s, 0xf9090054), w580_es_pc(s));
    } else if (addr >= 0xf9090010 && addr <= 0xf909001a && w580_es.n_puerta++ < 1500) {
        w580_es_suceso("puerta", "W F90900%02x = 0x%04x (antes 0x%04x) pc=0x%08x lr=0x%08x", addr & 255, value, antes, w580_es_pc(s), s->cpu->env.regs[14]);
    }
    if (addr == 0xf9090080) {
        if (w580_es_l16(s, 0xf909001c) & 1) {
            w580_es_aviso(s, "pll", 4, "F9090080 = 0x%x con el PLL en uso (F909001C bit 0 = 1): el original solo lo toca con la CPU en 13 MHz", value);
        }
        if ((value & 0x10) && !(antes & 0x10)) { w580_es.pll_t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); w580_es.pll_visto = false; }
        if (!(value & 0x10)) { w580_es.pll_t = 0; w580_es.pll_visto = false; }
    }
    if (addr == 0xf909001c && (value & 1) && !(antes & 1)) {
        uint32_t e30 = w580_es_l32(s, 0x14000030), e3c = w580_es_l32(s, 0x1400003c);
        unsigned d54 = w580_es_l16(s, 0xf9090054);
        if (!w580_es.pll_t) {
            w580_es_aviso(s, "pll", 1, "F909001C |= 1 (usar el PLL) sin haberlo arrancado: el original escribe antes F9090080 = 0x2810 (0x44236580)");
        } else if (!w580_es.pll_visto) {
            w580_es_aviso(s, "pll", 2, "F909001C |= 1 (usar el PLL) sin haber leído F9090080 bit 0 = 1 (enganchado): el original espera en 0x44236582");
        }
        if (e30 == 0x123d00c0 || e3c == 0x20408108) {
            w580_es_aviso(s, "pll", 3, "se sube el reloj con la SDRAM en los tiempos de 13 MHz (EMC+30 = 0x%08x, +3C = 0x%08x): el original pone ANTES los "
                          "de 52 MHz de bus (0x12310303, 0x83060C1A), desde la ITCM (0xFCB0)", e30, e3c);
        }
        if (!(value & 0x10) && d54 != 3) {
            w580_es_aviso(s, "pll", 5, "PLL sin dividir (F909001C bit 4 = 0: 208 MHz) con F9090054 = %u: el original pone antes F9090054 <- 3 "
                          "(divisor del bus, 208/4 = 52 MHz) [inferido el significado]", d54);
        }
    }
    if (addr == 0xf909001c) {
        unsigned mhz = !(value & 1) ? 13 : (value & 0x10) ? 104 : 208;
        if (mhz != w580_es.mhz) { w580_es_suceso("pll", "CPU a %u MHz (antes %u)", mhz, w580_es.mhz); w580_es.mhz = mhz; }
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * c. Perro guardián (docs/hal.md §3). El original escribe la llave F9010010 = AA, F9010024 = 5A, F901002C = A5 UNA
 * vez (FUN_441ad0cc, desde el crt0) y no vuelve a tocar el bloque en activo; el reset forzado (FUN_441a7164) es
 * F9010030 <- 0, F901002C <- 0 y bucle. => la llave DESARMA y «+2C <- 0» ARMA [inferido, igual que en el DB2010].
 * Estricto: del reset a la llave no pueden pasar más de W580_ES_WD_MS; armado, muerde a los W580_ES_WD_MS. */
#define W580_ES_WD_MS 2000      /* [inferido el valor: el periodo real no se puede medir, el original nunca lo alimenta] */
static void w580_es_wd_arma(W580State *s)
{
    w580_es.wd_armado = true;
    w580_es.wd_vence = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + W580_ES_WD_MS * INT64_C(1000000);
    if (w580_es.on) { timer_mod(w580_es.wd_timer, w580_es.wd_vence); }
}
static void w580_es_wd_muerde(void *opaque)
{
    W580State *s = opaque;
    if (!w580_es.on || !w580_es.wd_armado) { return; }
    w580_es.wd_resets++;
    if (w580_es.wd_llave) {
        w580_es_aviso(s, "perro", 1, "RESET por perro guardián: armado con F901002C <- 0 hace %d ms (reset forzado del original, FUN_441a7164). Reset n.º %u",
                      W580_ES_WD_MS, w580_es.wd_resets);
    } else {
        w580_es_aviso(s, "perro", 2, "RESET por perro guardián: %d ms desde el reset sin la llave F9010010 = AA, F9010024 = 5A, F901002C = A5 "
                      "(el original la escribe una vez, FUN_441ad0cc, nada más encender la SDRAM). Reset n.º %u", W580_ES_WD_MS, w580_es.wd_resets);
    }
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}
/* Retención de la alimentación (docs/hal.md §3): el crt0 del original hace F9050058 |= 1 antes de tener RAM, y solo
 * lo quita (FUN_442342b4) con el PMU ya programado (registro 0x41 = 0x26). Si a los W580_ES_PW_MS del reset no hay ni
 * lo uno ni lo otro, en el aparato se soltaría la alimentación al soltar la tecla [inferido, como en el DB2010]. */
#define W580_ES_PW_MS 2000
static void w580_es_pw_mira(void *opaque)
{
    W580State *s = opaque;
    if (!w580_es.on) { return; }
    if (!(s->rtc_regs[0x58] & 1) && !s->i2c[0].regs[0x4a][0x41]) {
        w580_es_aviso(s, "alimentacion", 1, "a los %d ms del reset nada sujeta la alimentación: F9050058 bit 0 = 0 y PMU 0x41 = 0 (el original pone "
                      "F9050058 |= 1 en el crt0 y no lo quita hasta programar el PMU: 0x41 = 0x26)", W580_ES_PW_MS);
    }
}
static void w580_es_perro(W580State *s, uint32_t addr, bool write, uint32_t value)
{
    w580_es_suceso("perro", "%s F90100%02x = 0x%x pc=0x%08x lr=0x%08x", write ? "W" : "R", addr & 255, value, w580_es_pc(s), s->cpu->env.regs[14]);
    if (!write) { return; }
    if (addr == 0xf9010030) { w580_es.wd_30_cero = value == 0; }      /* (AUD4) primer paso del reset forzado del original */
    if (addr == 0xf9010010) { w580_es.wd_paso = value == 0xaa ? 1 : 0; }
    else if (addr == 0xf9010024) { w580_es.wd_paso = (w580_es.wd_paso == 1 && value == 0x5a) ? 2 : 0; }
    else if (addr == 0xf901002c) {
        if (value == 0xa5 && w580_es.wd_paso == 2) {
            w580_es.wd_paso = 3; w580_es.wd_armado = false; w580_es.wd_llave = true;
            if (w580_es.on) { timer_del(w580_es.wd_timer); }
        } else if (value == 0xa5) {
            w580_es_aviso(s, "perro", 3, "F901002C = A5 sin las dos llaves anteriores (F9010010 = AA, F9010024 = 5A): no cuenta");
        } else if (value == 0) {
            w580_es.wd_paso = 0;
            if (!w580_es.wd_30_cero) {       /* (AUD4) antes armaba (y reiniciaba) igual: un «reiniciar» incompleto pasaba */
                w580_es_aviso(s, "perro", 5, "F901002C <- 0 sin F9010030 <- 0 antes: el reset forzado del original (FUN_441a7164) son F9010030 <- 0, F901002C <- 0 "
                              "y F9010014 <- 1 en bucle; si con solo esto el aparato se reinicia: [sin saber]");
            }
            if (!w580_es.wd_armado) { w580_es_wd_arma(s); }
        } else {
            w580_es_aviso(s, "perro", 4, "F901002C = 0x%x: el original solo escribe ahí A5 (llave) o 0 (armar)", value);
        }
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * e. ADC del PMU (docs/hal.md §12; vincenne_adc_request 0x44232F70 + FUN_44233734). PMU 0x70 <- 0xA0|canal elige el
 * canal y arma; la conversión la dispara el pulso F5000874 <- 0x11 … <- 0x12 (con la puerta 40 de SYSCON abierta:
 * F9090016 bit 11 a 0); el resultado está en 0x71. PMU 0x70 <- 0x6A (la batería en el arranque temprano,
 * FUN_449cdb04) convierte sin pulso [observado: el original lee 0x71 sin darlo]. Estricto: sin pulso completo entre
 * el 0x70 <- 0xAx y la lectura de 0x71, la lectura devuelve el dato VIEJO y avisa. */
#define W580_ES_ADC_NS 20000    /* de la bajada del pulso (0x12) al dato listo [inferido el valor; el original tarda más] */
static uint8_t w580_pmu_adc(W580State *s, unsigned canal);
static void w580_es_adc_pulso(W580State *s, uint32_t value)
{
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool puerta = !(w580_es_l16(s, 0xf9090016) & 0x0800);
    if (value == 0x11) {
        w580_es.adc_p11 = true; w580_es.adc_t11 = t;
        if (!puerta) {
            w580_es.adc_pulso_sin_puerta++;
            w580_es_aviso(s, "adc", 4, "pulso de conversión (F5000874 <- 0x11) con la puerta 40 de SYSCON cerrada (F9090016 bit 11 = 1): "
                          "el original la abre antes (FUN_44233734)");
        }
    } else if (value == 0x12) {
        if (!w580_es.adc_p11) {
            w580_es_aviso(s, "adc", 3, "F5000874 <- 0x12 sin el 0x11 anterior: no es un pulso de conversión");
            return;
        }
        int64_t ancho = t - w580_es.adc_t11;
        w580_es.adc_p11 = false; w580_es.adc_pulsos++; w580_es.adc_t12 = t;
        if (!w580_es.adc_min_ancho || ancho < w580_es.adc_min_ancho) { w580_es.adc_min_ancho = ancho; }
        if (ancho > w580_es.adc_max_ancho) { w580_es.adc_max_ancho = ancho; }
        if (w580_es.on && w580_es.adc_pedido >= 0) {
            s->i2c[0].regs[0x4a][0x71] = w580_pmu_adc(s, w580_es.adc_pedido);
            w580_es.adc_hecho = true;
        }
    }
}
/* escritura del registro 0x70 del PMU. Devuelve true si el modelo NO debe convertir ya (estricto: espera al pulso). */
static bool w580_es_adc_elige(W580State *s, uint8_t value)
{
    if (!w580_es_activo()) { return false; }
    if (value == 0x6a) { w580_es.adc_6a++; w580_es.adc_pedido = -1; w580_es.adc_hecho = true; return false; }
    if ((value & 0xe0) != 0xa0) { return false; }
    w580_es.adc_sel++; w580_es.adc_pedido = value & 15; w580_es.adc_hecho = false;
    w580_es.adc_t_sel = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return w580_es.on;
}
static void w580_es_adc_lee(W580State *s)
{
    if (!w580_es_activo() || w580_es.adc_pedido < 0) { return; }
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    w580_es.adc_lect++;
    if (!w580_es.adc_hecho && !(w580_es.adc_t12 > w580_es.adc_t_sel)) {
        w580_es.adc_lect_sin++;
        w580_es_suceso("adc", "lectura de 0x71 SIN pulso tras elegir el canal %d pc=0x%08x lr=0x%08x", w580_es.adc_pedido, w580_es_pc(s), s->cpu->env.regs[14]);
        w580_es_aviso(s, "adc", 1, "se lee el resultado del ADC (PMU 0x71) del canal %d sin el pulso de conversión F5000874 <- 0x11 … <- 0x12 "
                      "después de PMU 0x70 <- 0x%02x: devuelve el dato VIEJO (el original: FUN_44233734)", w580_es.adc_pedido, 0xa0 | w580_es.adc_pedido);
        return;
    }
    int64_t d = t - w580_es.adc_t12;
    if (!w580_es.adc_min_lee || d < w580_es.adc_min_lee) { w580_es.adc_min_lee = d; }
    if (d > w580_es.adc_max_lee) { w580_es.adc_max_lee = d; }
    if (d < W580_ES_ADC_NS) {
        w580_es_aviso(s, "adc", 2, "se lee PMU 0x71 a %" PRId64 " ns del pulso de conversión: antes de %d ns el dato no está [inferido el tiempo]", d, W580_ES_ADC_NS);
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * I2C: cada (bus, esclavo, registro, sentido) que usa el firmware. En estricto, lo que el original no toca: aviso. */
#include "w580-estricto-i2c.inc"
static void w580_es_i2c(W580State *s, unsigned bus, uint8_t slave, uint8_t reg, bool write, uint8_t value)
{
    if (!w580_es_activo() || !current_cpu) { return; }
    uint32_t key = (bus << 24) | (slave << 16) | (reg << 8) | write;
    if (w580_es.aprender) {
        if (!w580_es.i2c) { w580_es.i2c = g_hash_table_new(g_direct_hash, g_direct_equal); }
        W580EsI2C *e = g_hash_table_lookup(w580_es.i2c, GUINT_TO_POINTER(key));
        if (!e) { e = g_new0(W580EsI2C, 1); e->pc1 = w580_es_pc(s); g_hash_table_insert(w580_es.i2c, GUINT_TO_POINTER(key), e); }
        e->n++;
        if (e->nval < 8) { unsigned i; for (i = 0; i < e->nval && e->val[i] != value; i++) { } if (i == e->nval) { e->val[e->nval++] = value; } }
        if (write && !(slave == 0x4a && reg == 0x70)) {       /* traza de sonido y de energía: todo lo que se escribe por I2C salvo elegir canal del ADC */
            w580_es_suceso("i2cw", "bus=%u esclavo=0x%02x reg=0x%02x valor=0x%02x pc=0x%08x lr=0x%08x", bus, slave, reg, value, w580_es_pc(s), s->cpu->env.regs[14]);
        }
    }
    if (!w580_es.on || G_N_ELEMENTS(w580_es_i2c_lista) < 2) { return; }
    if (write && bus == 0 && slave == 0x4a) {
        if (reg == 0x70 && (value & 0xe0) == 0xa0 && !(w580_es_adc_canales >> (value & 15) & 1)) {
            w580_es_aviso(s, "adc", 0x500 | (value & 15), "PMU 0x70 <- 0x%02x: el canal %u del ADC no lo convierte nunca el original (usa los de la máscara 0x%04x)",
                          value, value & 15, w580_es_adc_canales);
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(w580_es_pmu_val); i++) {
            if (w580_es_pmu_val[i].reg != reg || !w580_es_pmu_val[i].n) { continue; }
            unsigned j; for (j = 0; j < w580_es_pmu_val[i].n && w580_es_pmu_val[i].val[j] != value; j++) { }
            if (j == w580_es_pmu_val[i].n) {
                w580_es_aviso(s, "pmu", (reg << 8) | value, "PMU registro 0x%02x <- 0x%02x: es un regulador o el cargador, y el original nunca escribe ese valor ahí "
                              "(sus valores están en w580-estricto-i2c.inc)", reg, value);
            }
        }
    }
    /* E5: niveles del chip de LED del I2C 1 (0x40). [observado, e5_led_* y e5_c1led_*] el original escribe en 0x09 y 0x0A de 0 a 5,
     * y en 0x2F de 0 a 0x84 (0x84 con el logo, 0x2F en standby, 0x06 atenuada). Qué corriente es cada valor: [sin saber]; por eso
     * un nivel por encima del máximo del original es un aviso (como los reguladores del PMU).
     * (S5, con la pista L) El tope de 0x09 / 0x0A pasa de 0x05 a 0x10: 0x05 es solo el 36 % del standby. El máximo del CÓDIGO del
     * original es 0x10 [observado: su nivel es % x máximo / 100 (FUN_4529cffc) y lo ejecutado solo cuadra con máximo 0x10 —36 % -> 5,
     * 30 % -> 4, y la rampa 2, 4, 5 es el paso de 400 ms sobre un rango de 0x10—; es el valor por omisión de su GDFS (unidad 2018)],
     * y lo escribe cualquier efecto de luz a intensidad 100 (una llamada entrante: mt_call.ble). No se le ha visto EJECUTAR más de 5. */
    if (write && bus == 1 && slave == 0x40 && (((reg == 0x09 || reg == 0x0a) && value > 0x10) || (reg == 0x2f && value > 0x84))) {
        w580_es_aviso(s, "luz", (reg << 8) | value, "chip de LED (I2C 1, 0x40) registro 0x%02x <- 0x%02x: es un NIVEL de luz y el original nunca pasa de 0x%02x ahí "
                      "(0x09 y 0x0A: 0..0x10 en su código, 0..5 en lo ejecutado; 0x2F: 0x84 con el logo, 0x2F en standby, 0x06 atenuada). Qué corriente es cada valor: [sin saber]", reg, value,
                      reg == 0x2f ? 0x84 : 0x10);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(w580_es_i2c_lista); i++) { if (w580_es_i2c_lista[i] == key) { return; } }
    w580_es_aviso(s, "i2c", key, "%s del registro 0x%02x del esclavo 0x%02x del bus %u (valor 0x%02x): el original no lo hace nunca en lo ejecutado",
                  write ? "escritura" : "lectura", reg, slave, bus, value);
}
static void w580_es_i2c_nadie(W580State *s, unsigned bus, uint8_t slave)
{
    if (w580_es.aprender && current_cpu) {
        if (!w580_es.nack) { w580_es.nack = g_hash_table_new(g_direct_hash, g_direct_equal); }
        uint32_t k = (bus << 8) | slave;
        g_hash_table_insert(w580_es.nack, GUINT_TO_POINTER(k), GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(w580_es.nack, GUINT_TO_POINTER(k))) + 1));
    }
    if (!w580_es.on || !current_cpu || G_N_ELEMENTS(w580_es_i2c_nack) < 2) { return; }
    for (unsigned i = 0; i < G_N_ELEMENTS(w580_es_i2c_nack); i++) { if (w580_es_i2c_nack[i] == ((bus << 8) | slave)) { return; } }
    w580_es_aviso(s, "i2c", 0x80000000 | (bus << 8) | slave, "se llama al esclavo 0x%02x del bus %u: no contesta (NACK) y el original no lo llama nunca", slave, bus);
}

/* ---------------------------------------------------------------------------------------------------------------
 * b. Puertas de reloj y f. registros que el original no toca: el gancho común de toda la E/S.
 * Devuelve true si el acceso queda resuelto aquí (solo la SDRAM cerrada): la lectura da *value, la escritura se pierde. */
static const W580EsLista *w580_es_busca(uint32_t addr)
{
    unsigned a = 0, b = G_N_ELEMENTS(w580_es_lista);
    while (a < b) {
        unsigned m = (a + b) / 2;
        if (w580_es_lista[m].dir == addr) { return &w580_es_lista[m]; }
        if (w580_es_lista[m].dir < addr) { a = m + 1; } else { b = m; }
    }
    return NULL;
}
static bool w580_es_pagina(uint32_t addr)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(w580_es_pags); i++) { if (w580_es_pags[i] == addr >> 20) { return true; } }
    return false;
}
static bool w580_es_acceso(W580State *s, hwaddr addr, unsigned size, bool write, uint64_t *value)
{
    if (!w580_es_activo() || !current_cpu) { return false; }
    if (addr >= 0x20000000 && addr < 0x21000000) {
        if (!w580_es.sd_lista) { w580_es.sd_antes++; }
        if (w580_es.on && !s->sin_ram && !w580_es.sd_lista) { return w580_es_sdram_cerrada(s, addr, size, write, value); }
    }
    if (w580_es.aprender) { w580_es_cuenta(s, addr, size, write, *value); }
    if (write) {
        if (addr >= 0x14000000 && addr < 0x14000100) { w580_es_emc_escribe(s, addr, *value); }
        else if (addr >= 0xf9090000 && addr < 0xf9090100) { w580_es_syscon_escribe(s, addr, size, *value); }
        else if (addr == 0xf5000874) { w580_es_adc_pulso(s, *value); }
        else if (((addr >= 0xfa040000 && addr < 0xfa040100 && (addr & 0xff) != 0x24 && (addr & 0xff) != 0x28 && (addr & 0xff) != 0x2c) ||
                  (addr >= 0xfa020000 && addr < 0xfa020100)) && w580_es.n_dsp++ < 4000) {       /* traza de sonido: buzón del DSP y PCM */
            w580_es_suceso("dsp", "W 0x%08x = 0x%x pc=0x%08x lr=0x%08x", (unsigned)addr, (unsigned)*value, w580_es_pc(s), s->cpu->env.regs[14]);
        } else if (addr >= 0xf9020000 && addr < 0xf9020008) {
            w580_es_suceso("tono", "W F902000%x = 0x%x pc=0x%08x", (unsigned)addr & 15, (unsigned)*value, w580_es_pc(s));
        }
    }
    if (addr >= 0xf9010010 && addr < 0xf9010038 && addr != 0xf901001c && addr != 0xf9010020) { w580_es_perro(s, addr, write, *value); }
    if (!w580_es.on) { return false; }
    /* b. puertas */
    if (w580_es.syscon_visto) {
        for (unsigned i = 0; i < G_N_ELEMENTS(w580_es_puertas); i++) {
            const W580EsPuerta *p = &w580_es_puertas[i];
            if (addr < p->ini || addr > p->fin) { continue; }
            bool parado = p->gr >= 0 && (w580_es_l16(s, w580_es_cerr_dir[p->gr]) & (1u << p->gb));
            bool en_reset = p->hr >= 0 && !(w580_es_l16(s, 0xf9090010 + 2 * p->hr) & (1u << p->hb));
            if (parado || en_reset) {
                char q1[80] = "", q2[80] = "";
                if (parado) { snprintf(q1, sizeof(q1), "su reloj parado (F90900%02X bit %d a 1)", w580_es_cerr_dir[p->gr] & 255, p->gb); }
                if (en_reset) { snprintf(q2, sizeof(q2), "el bloque en reset (F90900%02X bit %d a 0)", 0x10 + 2 * p->hr, p->hb); }
                w580_es_aviso(s, "reloj", (p->ini >> 4) ^ (parado ? 2 : 0) ^ (en_reset ? 4 : 0) ^ write,
                              "%s de 0x%08" HWADDR_PRIx " (%s) con %s%s%s: el original nunca toca este bloque así (lo abre %s)",
                              write ? "escritura" : "lectura", addr, p->nombre, q1, parado && en_reset ? " y " : "", q2, p->quien);
            }
            break;
        }
    }
    /* a/f. mapa y registros */
    if (G_N_ELEMENTS(w580_es_lista) > 1) {
        bool memoria = addr < 0xf0000000 && !(addr >= 0x14000000 && addr < 0x14001000);
        if (!w580_es_pagina(addr)) {
            w580_es_aviso(s, "mapa", addr >> 20, "%s de 0x%08" HWADDR_PRIx ": ahí no hay nada que el original use (no toca ese megabyte en nada de lo ejecutado)",
                          write ? "escritura" : "lectura", addr);
        } else if (!memoria) {
            const W580EsLista *e = w580_es_busca(addr);
            unsigned din = write ? 2 : 1, est = write ? 8 : 4;
            if (!e) {
                w580_es_aviso(s, "registro", addr, "%s del registro 0x%08" HWADDR_PRIx " (%u bytes, valor 0x%" PRIx64 "): el original no lo toca nunca",
                              write ? "escritura" : "lectura", addr, size, *value);
            } else if (!(e->f & din)) {
                if (e->f & (est | (write ? 0 : 2))) { w580_es.n_solo_estatico++; }     /* está en su código (o lo escribe y aquí se relee): no se avisa */
                else {
                    w580_es_aviso(s, "registro", addr ^ 0x80000000, "%s del registro 0x%08" HWADDR_PRIx ": el original solo lo %s",
                                  write ? "escritura" : "lectura", addr, write ? "lee" : "escribe");
                }
            }
        }
    }
    return false;
}
/* lectura de F9090080 (PLL): en estricto el bit 0 tarda */
static void w580_es_pll_lee(W580State *s, uint64_t *out)
{
    if (!w580_es.on) { return; }
    if (w580_es_pll_enganchado(s)) { *out |= 1; } else { *out &= ~UINT64_C(1); }
}

#include "w580-estricto-cache.inc"

/* ---------------------------------------------------------------------------------------------------------------
 * Volcado de lo aprendido */
static gint w580_es_cmp64(gconstpointer a, gconstpointer b, gpointer d) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static gint w580_es_cmp32(gconstpointer a, gconstpointer b, gpointer d) { uint32_t x = GPOINTER_TO_UINT(a), y = GPOINTER_TO_UINT(b); return x < y ? -1 : x > y; }
static void w580_es_nor_volcar(GString *g);
static bool w580_es_volcar(W580State *s)
{
    if (!w580_es.aprender || !w580_es.ruta || !*w580_es.ruta) { return false; }
    GString *g = g_string_new("# aprendido por estricto-aprender (pista E4). REG: dir sentido tam n n_tras_syscon pc1 lr1 t1_ns y_cerr[16,18,1A,1C,3A] o_cerr[...] y_hab[10,12,14] o_hab[...] valores\n");
    if (w580_es.regs) {
        GList *ks = g_list_sort_with_data(g_hash_table_get_keys(w580_es.regs), w580_es_cmp64, NULL);
        for (GList *l = ks; l; l = l->next) {
            uint64_t k = *(uint64_t *)l->data; W580EsReg *e = g_hash_table_lookup(w580_es.regs, &k);
            g_string_append_printf(g, "REG\t%08x\t%c\t%u\t%u\t%u\t%08x\t%08x\t%" PRId64, (uint32_t)k, (k >> 32) ? 'W' : 'R', e->tam, e->n, e->n_tras, e->pc1, e->lr1, e->t1);
            for (unsigned i = 0; i < W580_ES_NCERR; i++) { g_string_append_printf(g, "%c%04x", i ? ',' : '\t', e->y_cerr[i]); }
            for (unsigned i = 0; i < W580_ES_NCERR; i++) { g_string_append_printf(g, "%c%04x", i ? ',' : '\t', e->o_cerr[i]); }
            for (unsigned i = 0; i < 3; i++) { g_string_append_printf(g, "%c%04x", i ? ',' : '\t', e->y_hab[i]); }
            for (unsigned i = 0; i < 3; i++) { g_string_append_printf(g, "%c%04x", i ? ',' : '\t', e->o_hab[i]); }
            for (unsigned i = 0; i < e->nval; i++) { g_string_append_printf(g, "%c%x", i ? ',' : '\t', e->val[i]); }
            g_string_append_c(g, '\n');
        }
        g_list_free(ks);
    }
    if (w580_es.i2c) {
        GList *ks = g_list_sort_with_data(g_hash_table_get_keys(w580_es.i2c), w580_es_cmp32, NULL);
        for (GList *l = ks; l; l = l->next) {
            uint32_t k = GPOINTER_TO_UINT(l->data); W580EsI2C *e = g_hash_table_lookup(w580_es.i2c, l->data);
            g_string_append_printf(g, "I2C\t%u\t%02x\t%02x\t%c\t%u\t%08x", k >> 24, (k >> 16) & 255, (k >> 8) & 255, (k & 1) ? 'W' : 'R', e->n, e->pc1);
            for (unsigned i = 0; i < e->nval; i++) { g_string_append_printf(g, "%c%02x", i ? ',' : '\t', e->val[i]); }
            g_string_append_c(g, '\n');
        }
        g_list_free(ks);
    }
    if (w580_es.nack) {
        GList *ks = g_list_sort_with_data(g_hash_table_get_keys(w580_es.nack), w580_es_cmp32, NULL);
        for (GList *l = ks; l; l = l->next) {
            uint32_t k = GPOINTER_TO_UINT(l->data);
            g_string_append_printf(g, "NACK\t%u\t%02x\t%u\n", k >> 8, k & 255, GPOINTER_TO_UINT(g_hash_table_lookup(w580_es.nack, l->data)));
        }
        g_list_free(ks);
    }
    g_string_append_printf(g, "ADC\telige=%" PRIu64 "\tpulsos=%" PRIu64 "\tlecturas=%" PRIu64 "\tlecturas_sin_pulso=%" PRIu64 "\tbateria_6a=%" PRIu64
                           "\tpulso_sin_puerta=%" PRIu64 "\tancho_ns=%" PRId64 "..%" PRId64 "\tpulso_a_lectura_ns=%" PRId64 "..%" PRId64 "\n",
                           w580_es.adc_sel, w580_es.adc_pulsos, w580_es.adc_lect, w580_es.adc_lect_sin, w580_es.adc_6a, w580_es.adc_pulso_sin_puerta,
                           w580_es.adc_min_ancho, w580_es.adc_max_ancho, w580_es.adc_min_lee, w580_es.adc_max_lee);
    g_string_append_printf(g, "SDRAM\thitos=0x%02x\tlista=%d\taccesos_antes=%" PRIu64 "\n", w580_es.sd_hitos, w580_es.sd_lista, w580_es.sd_antes);
    w580_es_nor_volcar(g);
    w580_ca_volcar(g);
    if (w580_es.sucesos) { g_string_append(g, w580_es.sucesos->str); }
    g_autofree char *tmp = g_strdup_printf("%s.tmp", w580_es.ruta);
    bool ok = g_file_set_contents(tmp, g->str, g->len, NULL) && rename(tmp, w580_es.ruta) == 0;
    g_string_free(g, true);
    return ok;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Reset e inicio */
static void w580_es_reset(W580State *s)
{
    if (!w580_es_activo()) { return; }
    w580_es.t_reset = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    w580_es.resets++;
    w580_es_suceso("reset", "n=%u", w580_es.resets);
    w580_es.syscon_visto = false;
    w580_es.sd_hitos = 0; w580_es.sd_ref = 0; w580_es.sd_lista = false; w580_es.sd_antes = 0;
    w580_es.pll_t = 0; w580_es.pll_visto = false; w580_es.mhz = 13;
    w580_es.wd_paso = 0; w580_es.wd_llave = false; w580_es.wd_30_cero = false;
    w580_es.adc_pedido = -1; w580_es.adc_p11 = false; w580_es.adc_hecho = false;
    if (w580_es.on) {
        if (!s->sin_ram) { memory_region_set_enabled(MACHINE(s)->ram, false); w580_ca_sdram(false); }
        if (!w580_es.basura_hecha) {
            /* la RAM no arranca a ceros: basura con semilla fija (no se repone en un reset en caliente) */
            struct { MemoryRegion *mr; unsigned n; } r[] = { { MACHINE(s)->ram, 16 * 1024 * 1024 }, { &s->itcm, 0x10000 }, { &s->dtcm, 0x10000 },
                                                           { &s->bram, 0x4000 }, { &s->iram, 0x3000 } };
            for (unsigned k = 0; k < G_N_ELEMENTS(r); k++) {
                if (k == 0 && s->sin_ram) { continue; }
                uint32_t *p = memory_region_get_ram_ptr(r[k].mr);
                for (unsigned i = 0; i < r[k].n / 4; i++) { p[i] = w580_es_basura(i * 4 + k * 0x1000003); }
                memory_region_set_dirty(r[k].mr, 0, r[k].n);
            }
            if (w580_ca.hay) { for (unsigned i = 0; i < W580_CA_TAM / 4; i++) { ((uint32_t *)w580_ca.vista)[i] = w580_es_basura(W580_CA_BASE - 0x20000000u + i * 4); } }
            w580_es.basura_hecha = true;
        }
        w580_es_wd_arma(s);       /* del reset a la llave */
        timer_mod(w580_es.pw_timer, w580_es.t_reset + W580_ES_PW_MS * INT64_C(1000000));
    }
    w580_ca_reset(s);
}
static void w580_es_init(W580State *s)
{
    w580_es.wd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_es_wd_muerde, s);
    w580_es.adc_pedido = -1;
    w580_es.pw_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_es_pw_mira, s);
    w580_ca_init(s);
}
static gint w580_es_cmp_aviso(gconstpointer a, gconstpointer b, gpointer d)
{
    const W580EsAviso *x = a, *y = b; return x->orden < y->orden ? -1 : x->orden > y->orden;
}
static bool w580_get_estricto(Object *obj, Error **errp) { return w580_es.on; }
static void w580_set_estricto(Object *obj, bool value, Error **errp) { w580_es.on = value; }
static char *w580_get_es_aprender(Object *obj, Error **errp) { return g_strdup(w580_es.ruta); }
static void w580_set_es_aprender(Object *obj, const char *value, Error **errp)
{
    g_free(w580_es.ruta); w580_es.ruta = g_strdup(value); w580_es.aprender = value && *value;
}
static char *w580_get_es_volcar(Object *obj, Error **errp) { return g_strdup(""); }
static void w580_set_es_volcar(Object *obj, const char *value, Error **errp)
{
    if (!w580_es_volcar(W580_MACHINE(obj))) { error_setg(errp, "estricto-volcar: no hay estricto-aprender=RUTA o no se puede escribir"); }
}
static char *w580_get_es_avisos(Object *obj, Error **errp)
{
    char *r = g_strdup(w580_es.avisos ? w580_es.avisos->str : "");
    if (w580_es.avisos) { g_string_truncate(w580_es.avisos, 0); }
    return r;
}
static char *w580_get_es_resumen(Object *obj, Error **errp)
{
    GString *g = g_string_new(NULL);
    g_string_append_printf(g, "estricto=%s avisos=%u distintos=%u resets_perro=%u sdram=%s cpu=%u MHz registros_solo_en_codigo=%" PRIu64 "\n",
                           w580_es.on ? "on" : "off", w580_es.n_avisos, w580_es.vistos ? g_hash_table_size(w580_es.vistos) : 0, w580_es.wd_resets,
                           w580_es.sd_lista ? "encendida" : "apagada", w580_es.mhz, w580_es.n_solo_estatico);
    if (w580_es.vistos) {
        GList *vs = g_list_sort_with_data(g_hash_table_get_values(w580_es.vistos), w580_es_cmp_aviso, NULL);
        for (GList *l = vs; l; l = l->next) {
            W580EsAviso *a = l->data;
            g_string_append_printf(g, "%s\t%08x\t%08x\t%u\t%.4f\t%s\n", a->regla, a->pc, a->lr, a->n, a->t1, a->texto);
        }
        g_list_free(vs);
    }
    return g_string_free(g, false);
}
static void w580_get_es_n(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint32_t n = w580_es.n_avisos; visit_type_uint32(v, name, &n, errp);
}
static void w580_es_semilla(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &w580_es.semilla, errp);
}
static void w580_es_class_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "estricto", w580_get_estricto, w580_set_estricto);
    object_class_property_set_description(oc, "estricto", "on: modo estricto (docs/emulador_estricto.md): avisa de lo que el main original no hace nunca. Solo al lanzar");
    object_class_property_add_str(oc, "estricto-aprender", w580_get_es_aprender, w580_set_es_aprender);
    object_class_property_set_description(oc, "estricto-aprender", "RUTA: no cambia nada; cuenta lo que hace el firmware y lo vuelca ahi al salir (o con estricto-volcar)");
    object_class_property_add_str(oc, "estricto-volcar", w580_get_es_volcar, w580_set_es_volcar);
    object_class_property_add_str(oc, "estricto-avisos", w580_get_es_avisos, NULL);
    object_class_property_set_description(oc, "estricto-avisos", "Avisos del modo estricto desde la ultima lectura (leerla los vacia)");
    object_class_property_add_str(oc, "estricto-resumen", w580_get_es_resumen, NULL);
    object_class_property_set_description(oc, "estricto-resumen", "Una linea de estado y una por aviso distinto: regla, pc, lr, veces, t del primero, texto");
    object_class_property_add(oc, "estricto-n", "uint32", w580_get_es_n, NULL, NULL, NULL);
    object_class_property_add_bool(oc, "estricto-cache-lecturas", w580_get_ca_lecturas, w580_set_ca_lecturas);
    object_class_property_set_description(oc, "estricto-cache-lecturas", "Pista E6: la regla del destino del MMIACC ve tambien las lineas que la CPU solo ha LEIDO (lento: la memoria grafica deja de leerse directa). Solo al lanzar");
    object_class_property_add_bool(oc, "estricto-cache-sordo", w580_get_ca_sordo, w580_set_ca_sordo);
    object_class_property_set_description(oc, "estricto-cache-sordo", "Pista E5, PRUEBA NEGATIVA: el modelo ignora las limpiezas de linea (c7,c10,1 y c7,c14,1) en la memoria grafica, como si el firmware no las hiciera");
    object_class_property_add_str(oc, "estricto-cache", w580_get_es_cache, NULL);
    object_class_property_set_description(oc, "estricto-cache", "Pista E5: contadores del modelo de cache de datos (operaciones de cp15 c7, lineas sucias al mandar el cuadro)");
    object_class_property_add(oc, "semilla", "uint32", w580_es_semilla, w580_es_semilla, NULL, NULL);
    object_class_property_set_description(oc, "semilla", "Semilla de la basura inicial de la RAM en estricto");
}

#else  /* ============================================================================================================
 * d. NOR M18 («Sibley»), segunda parte del fichero: se incluye desde w580-nor.inc, detrás de W580Nor.
 *
 * Reglas y de dónde salen:
 *  - bloques BLOQUEADOS tras el reset; programar o borrar uno bloqueado no hace nada y da SR.1 + SR.4/SR.5
 *    (familia Intel StrataFlash; el original desbloquea —60 D0— antes de cada operación y vuelve a bloquear —60 01—);
 *  - el programa es con búfer (E9 + cuenta + datos + D0) de hasta 512 palabras DENTRO de una región de programación
 *    de 1 KiB (CFI 0x2A = 2^10; el original manda siempre 0x1FF en direcciones múltiplo de 0x400);
 *  - regiones de programación de 1 KiB (M18): «modo objeto» = se programa UNA vez y no se vuelve a tocar hasta
 *    borrar el bloque; «modo control» = solo los 16 primeros bytes de cada segmento de 32 (A4 = 0), que sí se pueden
 *    volver a programar (bajando bits). Programar la mitad alta de una región en modo control, o reprogramar una
 *    región en modo objeto, es un aviso. [la regla del chip, de memoria de la hoja del M18 y de cómo lo trata Linux
 *    —writesize = región, sin MTD_BIT_WRITEABLE—; calibrada contra lo que hace el original: ver el documento];
 *  - el borrado y el programa TARDAN (W580_ES_T_*): mientras, el estado da SR.7 = 0 y el chip solo atiende 70
 *    (estado) y B0 (suspender; D0 reanuda). Cualquier otra orden durante un borrado es un aviso y se ignora.
 */
#define W580_ES_T_BORRA  INT64_C(200000000)   /* bloque de 256 KiB [inferido: la hoja del M18 da ~0,9 s típico (de memoria); aquí 0,2 s para
                                               * que el primer arranque del original, que borra ~80 bloques libres del FS, no tarde un minuto más] */
#define W580_ES_T_BUFER  INT64_C(2000000)     /* 1 KiB con búfer [ídem] */
#define W580_ES_T_PAL    INT64_C(150000)      /* una palabra [ídem] */
#define W580_ES_REG      0x400u
static struct {
    uint8_t *modo;              /* por región de 1 KiB: 0 blanca, 1 control, 2 objeto */
    uint8_t antes[2 * W580_ES_REG], dato[2 * W580_ES_REG], puesto[2 * W580_ES_REG];
    uint32_t base;              /* región donde empieza el búfer en curso */
    bool via04, buf_via04, buf_bloqueado;
    int64_t hasta, resto; uint8_t que; uint32_t que_dir; bool suspendida;
    uint64_t n_ord[2], n_bufer, n_palabra, n_borra, n_desbloq, n_bloq, n_susp, n_reanuda, n_durante, n_bloqueado;
    uint64_t clase[3][4];       /* [modo antes][0 solo FF, 1 solo mitad baja, 2 con mitad alta, 3 reprograma bytes ya escritos] */
    uint64_t n_no_alineado, n_cruza, n_cuenta_rara, n_sube, n_log, n_cmd[256], n_abortos;
} w580_esn;

static uint8_t w580_es_nor_modo_de(const uint8_t *p)
{
    bool bajo = false;
    for (unsigned i = 0; i < W580_ES_REG; i++) {
        if (p[i] != 0xff) { if (i & 0x10) { return 2; } bajo = true; }
    }
    return bajo ? 1 : 0;
}
static void w580_es_nor_reset(W580Nor *n)
{
    if (!w580_es_activo()) { return; }
    if (!w580_esn.modo) {
        w580_esn.modo = g_malloc(W580_NOR_TAM / W580_ES_REG);
        for (uint32_t r = 0; r < W580_NOR_TAM / W580_ES_REG; r++) { w580_esn.modo[r] = w580_es_nor_modo_de(w580_nor_ptr(n, r * W580_ES_REG)); }
    }
    w580_esn.hasta = 0; w580_esn.que = 0; w580_esn.suspendida = false;
    if (w580_es.on) { memset(n->bloqueo, 1, sizeof(n->bloqueo)); }      /* tras el reset, todo bloqueado */
}
static bool w580_es_nor_ocupada(void)
{
    if (!w580_esn.que || w580_esn.suspendida) { return false; }
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= w580_esn.hasta) { w580_esn.que = 0; return false; }
    return true;
}
static uint8_t w580_es_nor_estado(W580Nor *n)
{
    if (!w580_es.on) { return n->status; }
    if (w580_es_nor_ocupada()) { return n->status & ~0x80; }
    return n->status | 0x80 | (w580_esn.suspendida ? (w580_esn.que == 'B' ? 0x40 : 0x04) : 0);
}
static void w580_es_nor_tarda(uint8_t que, uint32_t abs, int64_t ns)
{
    if (!w580_es.on) { return; }
    if (w580_esn.suspendida) { return; }       /* un programa dentro de un borrado suspendido: no se modela su tiempo */
    w580_esn.que = que; w580_esn.que_dir = abs; w580_esn.hasta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns;
}
/* Toda escritura a la NOR pasa por aquí primero. true = el chip la ignora. */
static bool w580_es_nor_orden(W580Nor *n, W580NorPart *pt, uint32_t abs, uint64_t value)
{
    W580State *s = n->s;
    uint8_t cmd = value & 0xff;
    w580_esn.n_ord[w580_esn.via04]++;
    w580_ca_nor_orden(s, abs, cmd, w580_esn.via04, n->pend == 0);
    if (n->pend == 0) { w580_esn.n_cmd[cmd]++; }
    if (w580_esn.via04) {
        w580_es_aviso(s, "nor", 0x700, "orden 0x%02x a la NOR por la ventana 0x04000000: el original manda TODAS sus órdenes por 0x44000000 (1,5 millones "
                      "en la calibración, ninguna por 0x04000000); que el chip conteste ahí es [inferido] de su tabla de páginas", cmd);
    }
    if (w580_es.aprender && w580_esn.n_log < 600 && n->pend != 0xe8 && n->pend != 0x40) {
        w580_esn.n_log++;
        w580_es_suceso("nor", "orden 0x%02x dir=0x%08x via=%s pend=0x%02x pc=0x%08x lr=0x%08x", cmd, W580_NOR_BASE + abs, w580_esn.via04 ? "04" : "44",
                       n->pend, w580_es_pc(s), s->cpu->env.regs[14]);
    }
    if (!w580_es.on) { return false; }
    if (w580_es_nor_ocupada()) {
        if (cmd == 0x70) { return false; }
        if (cmd == 0xb0) {
            w580_esn.suspendida = true; w580_esn.resto = w580_esn.hasta - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); w580_esn.n_susp++;
            w580_nor_modo(pt, 0x70);
            return true;
        }
        w580_esn.n_durante++;
        w580_es_aviso(s, "nor", 0x100 | cmd, "orden 0x%02x (dir 0x%08x) durante un %s en 0x%08x (faltan %.1f ms): el chip solo atiende 70 (estado) y B0 "
                      "(suspender); se ignora. El original sondea SR.7 antes de mandar nada", cmd, W580_NOR_BASE + abs,
                      w580_esn.que == 'B' ? "BORRADO" : "programa", W580_NOR_BASE + w580_esn.que_dir,
                      (w580_esn.hasta - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) / 1e6);
        return true;
    }
    if (n->pend == 0 && w580_esn.suspendida) {
        if (cmd == 0xd0) {
            w580_esn.suspendida = false; w580_esn.hasta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + w580_esn.resto; w580_esn.n_reanuda++;
            w580_nor_modo(pt, 0x70);
            return true;
        }
        if (cmd == 0x20) {
            w580_es_aviso(s, "nor", 0x220, "orden de borrado con otro borrado suspendido (0x%08x): hay que reanudarlo (D0) antes", W580_NOR_BASE + w580_esn.que_dir);
            return true;
        }
    }
    if (n->pend == 0 && (cmd == 0x10 || cmd == 0x40)) {
        w580_es_aviso(s, "nor", 0x300 | cmd, "orden 0x%02x (programa de palabra de los Intel clásicos): el M18 usa 0x41, y el original solo programa con búfer (E9)", cmd);
    }
    if (n->pend == 0 && cmd == 0xe8) {
        w580_es_aviso(s, "nor", 0x3e8, "orden 0xE8 (búfer de los Intel clásicos): el M18 y el original usan 0xE9");
    }
    return false;
}
static bool w580_es_nor_bloqueado(W580Nor *n, uint32_t abs, const char *que)
{
    if (!w580_es.on || !(n->bloqueo[abs / W580_NOR_BLOQUE] & 1)) { return false; }
    w580_esn.n_bloqueado++;
    w580_es_aviso(n->s, "nor", 0x400 | (abs / W580_NOR_BLOQUE), "%s en 0x%08x con el bloque BLOQUEADO: no se hace y el estado da SR.1 (el original desbloquea "
                  "antes con 60 D0; tras un reset todos los bloques están bloqueados)", que, W580_NOR_BASE + abs);
    return true;
}
/* evalúa un programa (ya con el dato en w580_esn.dato/puesto relativos a base) contra las regiones */
static void w580_es_nor_evalua(W580Nor *n, uint32_t ini, uint32_t fin, const char *como)
{
    W580State *s = n->s;
    for (uint32_t r = ini / W580_ES_REG; r <= (fin - 1) / W580_ES_REG; r++) {
        uint32_t o = r * W580_ES_REG - w580_esn.base;
        unsigned bajo = 0, alto = 0, sobre = 0, rebaja = 0, sube = 0;
        uint8_t antes = w580_esn.modo[r];
        for (unsigned i = 0; i < W580_ES_REG; i++) {
            if (!w580_esn.puesto[o + i] || w580_esn.dato[o + i] == 0xff) { continue; }
            uint8_t d = w580_esn.dato[o + i], v = w580_esn.antes[o + i];
            if (i & 0x10) { alto++; } else { bajo++; }
            if (v != 0xff) { sobre++; if ((v & d) != v) { rebaja++; } if (d & ~v) { sube++; } }
        }
        unsigned cl = alto ? 2 : bajo ? 1 : 0;
        w580_esn.clase[antes][sobre ? 3 : cl]++;
        if (sube) { w580_esn.n_sube++; }
        if (w580_es.aprender && (w580_esn.n_log < 4000 || antes || sobre)) {
            w580_esn.n_log++;
            w580_es_suceso("norp", "%s region=0x%08x antes=%s bajo=%u alto=%u sobre_escrito=%u rebaja=%u sube=%u via=%s pc=0x%08x", como,
                           W580_NOR_BASE + r * W580_ES_REG, antes == 0 ? "blanca" : antes == 1 ? "control" : "objeto", bajo, alto, sobre, rebaja, sube,
                           w580_esn.buf_via04 ? "04" : "44", w580_es_pc(s));
        }
        if (antes == 2 && (bajo || alto)) {
            w580_es_aviso(s, "nor", 0x500, "%s en la región 0x%08x, que ya está programada en modo OBJETO (%u bytes nuevos, %u sobre bytes ya escritos): "
                          "una región en modo objeto se programa una vez y no se toca hasta borrar el bloque", como, W580_NOR_BASE + r * W580_ES_REG, bajo + alto, sobre);
        } else if (antes == 1 && alto) {
            w580_es_aviso(s, "nor", 0x501, "%s en la región 0x%08x, en modo CONTROL, con %u bytes en la mitad alta de un segmento de 32 (A4 = 1): "
                          "en modo control solo se usan los 16 primeros de cada 32", como, W580_NOR_BASE + r * W580_ES_REG, alto);
        }
        if (antes == 1 && !alto && bajo && como[9] == 'c') {     /* «programa con búfer» sobre una región ya en modo control */
            w580_es_aviso(s, "nor", 0x503, "programa con BÚFER (E9) en la región 0x%08x, que ya tiene datos en modo control (%u bytes nuevos en las mitades bajas): "
                          "el original no lo hace nunca; en modo control (su GDFS) programa palabra a palabra con la orden 0x41, y con búfer solo "
                          "regiones EN BLANCO enteras (su FS, modo objeto). Si el M18 acepta un búfer con 0xFFFF en las mitades altas: [sin saber]",
                          W580_NOR_BASE + r * W580_ES_REG, bajo);
        }
        if (antes != 2 && (alto || como[9] == 'c') && bajo + alto && antes == 0 && ((fin - ini) != W580_ES_REG || (ini & (W580_ES_REG - 1)))) {
            w580_es_aviso(s, "nor", 0x504, "programa con búfer de %u bytes en 0x%08x: el original programa con búfer SOLO regiones enteras de 1 KiB "
                          "(512 palabras en una dirección múltiplo de 0x400)", fin - ini, W580_NOR_BASE + ini);
        }
        if (antes != 2 && alto && como[9] == 'd') {      /* «programa de palabra» en una mitad alta */
            w580_es_aviso(s, "nor", 0x505, "programa de palabra (0x41) en la mitad alta de un segmento de 32 bytes (0x%08x): el original solo usa 0x41 "
                          "en las mitades bajas (modo control)", W580_NOR_BASE + r * W580_ES_REG);
        }
        if (sube && !(antes == 2)) {
            w580_es_aviso(s, "nor", 0x502, "%s en la región 0x%08x: %u bytes piden subir bits que ya están a 0 (la NOR solo baja bits; lo leído no será lo escrito)",
                          como, W580_NOR_BASE + r * W580_ES_REG, sube);
        }
        if (antes == 0) { w580_esn.modo[r] = cl; } else if (antes == 1 && alto) { w580_esn.modo[r] = 2; }
    }
}
static void w580_es_nor_buf_ini(W580Nor *n, uint32_t abs)
{
    w580_esn.base = abs & ~(W580_ES_REG - 1);
    if (w580_esn.base + 2 * W580_ES_REG > W580_NOR_TAM) { w580_esn.base = W580_NOR_TAM - 2 * W580_ES_REG; }
    memcpy(w580_esn.antes, w580_nor_ptr(n, w580_esn.base), W580_ES_REG);
    memcpy(w580_esn.antes + W580_ES_REG, w580_nor_ptr(n, w580_esn.base + W580_ES_REG), W580_ES_REG);
    memset(w580_esn.puesto, 0, sizeof(w580_esn.puesto));
    w580_esn.buf_via04 = w580_esn.via04;
    w580_esn.buf_bloqueado = w580_es_nor_bloqueado(n, abs, "programa con búfer");
}
/* carga del búfer abortada (dirección fuera del tramo): en el chip no se ha programado NADA todavía (los datos
 * estaban en el búfer); el modelo de E los iba poniendo en la matriz al llegar. Con estricto/aprender se deshace. */
static void w580_es_nor_buf_aborta(W580Nor *n)
{
    w580_esn.n_abortos++;
    if (n->buf_ini == UINT32_MAX) { return; }
    memcpy(w580_nor_ptr(n, w580_esn.base), w580_esn.antes, W580_ES_REG);
    memcpy(w580_nor_ptr(n, w580_esn.base + W580_ES_REG), w580_esn.antes + W580_ES_REG, W580_ES_REG);
}
static void w580_es_nor_buf_dato(W580Nor *n, uint32_t abs, unsigned size, uint64_t value)
{
    for (unsigned i = 0; i < size; i++) {
        uint32_t o = abs + i - w580_esn.base;
        if (o < 2 * W580_ES_REG) { w580_esn.dato[o] = value >> (8 * i); w580_esn.puesto[o] = 1; }
    }
}
/* confirmación (D0) del búfer. true = no se ha programado (bloque bloqueado): el llamante deja SR con error */
static bool w580_es_nor_buf_fin(W580Nor *n)
{
    W580State *s = n->s;
    uint32_t ini = n->buf_ini, fin = n->buf_fin, pal = (fin - ini) / 2;
    w580_esn.n_bufer++;
    if (w580_esn.buf_bloqueado) {
        memcpy(w580_nor_ptr(n, w580_esn.base), w580_esn.antes, W580_ES_REG);
        memcpy(w580_nor_ptr(n, w580_esn.base + W580_ES_REG), w580_esn.antes + W580_ES_REG, W580_ES_REG);
        return true;
    }
    if (ini / W580_ES_REG != (fin - 1) / W580_ES_REG) {
        w580_esn.n_cruza++;
        w580_es_aviso(s, "nor", 0x600, "programa con búfer de 0x%08x a 0x%08x: cruza una frontera de región de 1 KiB (el original manda siempre 512 palabras "
                      "en una dirección múltiplo de 0x400)", W580_NOR_BASE + ini, W580_NOR_BASE + fin);
    } else if ((ini & (W580_ES_REG - 1)) || pal != 512) { w580_esn.n_no_alineado++; }
    if (pal > 512) {
        w580_esn.n_cuenta_rara++;
        w580_es_aviso(s, "nor", 0x601, "programa con búfer de %u palabras: el búfer del M18 es de 512 (CFI 0x2A = 2^10 bytes)", pal);
    }
    w580_es_nor_evalua(n, ini, fin, "programa con búfer");
    w580_ca_nor_programa(s, ini, fin);
    w580_es_nor_tarda('P', ini, W580_ES_T_BUFER);
    return false;
}
static bool w580_es_nor_palabra(W580Nor *n, uint32_t abs, unsigned size, uint64_t value)
{
    w580_esn.n_palabra++;
    if (w580_es_nor_bloqueado(n, abs, "programa de palabra")) { return true; }
    w580_esn.base = abs & ~(W580_ES_REG - 1);
    memcpy(w580_esn.antes, w580_nor_ptr(n, w580_esn.base), W580_ES_REG);
    memset(w580_esn.puesto, 0, sizeof(w580_esn.puesto));
    w580_esn.buf_via04 = w580_esn.via04;
    w580_es_nor_buf_dato(n, abs, size, value);
    w580_es_nor_evalua(n, abs, abs + size, "programa de palabra");
    w580_ca_nor_programa(n->s, abs, abs + size);
    w580_es_nor_tarda('P', abs, W580_ES_T_PAL);
    return false;
}
static bool w580_es_nor_borra(W580Nor *n, uint32_t b)
{
    w580_esn.n_borra++;
    w580_es_suceso("norb", "bloque=0x%08x via=%s bloqueado=%u pc=0x%08x", W580_NOR_BASE + b, w580_esn.via04 ? "04" : "44", n->bloqueo[b / W580_NOR_BLOQUE] & 1, w580_es_pc(n->s));
    if (w580_es_nor_bloqueado(n, b, "borrado")) { return true; }
    memset(w580_esn.modo + b / W580_ES_REG, 0, W580_NOR_BLOQUE / W580_ES_REG);
    w580_es_nor_tarda('B', b, W580_ES_T_BORRA);
    return false;
}
static void w580_es_nor_volcar(GString *g)
{
    g_string_append_printf(g, "NOR\tordenes_via44=%" PRIu64 "\tordenes_via04=%" PRIu64 "\tbufer=%" PRIu64 "\tpalabra=%" PRIu64 "\tborrados=%" PRIu64
                           "\tdesbloqueos=%" PRIu64 "\tbloqueos=%" PRIu64 "\tsuspensiones=%" PRIu64 "\treanudaciones=%" PRIu64 "\tdurante_ocupada=%" PRIu64
                           "\ten_bloque_bloqueado=%" PRIu64 "\tno_alineado=%" PRIu64 "\tcruza=%" PRIu64 "\tsube_bits=%" PRIu64 "\n",
                           w580_esn.n_ord[0], w580_esn.n_ord[1], w580_esn.n_bufer, w580_esn.n_palabra, w580_esn.n_borra, w580_esn.n_desbloq, w580_esn.n_bloq,
                           w580_esn.n_susp, w580_esn.n_reanuda, w580_esn.n_durante, w580_esn.n_bloqueado, w580_esn.n_no_alineado, w580_esn.n_cruza, w580_esn.n_sube);
    g_string_append_printf(g, "NORCMD\tabortos_de_bufer=%" PRIu64, w580_esn.n_abortos);
    for (unsigned i = 0; i < 256; i++) { if (w580_esn.n_cmd[i]) { g_string_append_printf(g, "\t%02x=%" PRIu64, i, w580_esn.n_cmd[i]); } }
    g_string_append_c(g, '\n');
    static const char *const m[3] = { "blanca", "control", "objeto" }, *const c[4] = { "solo_FF", "solo_mitad_baja", "con_mitad_alta", "sobre_bytes_ya_escritos" };
    for (unsigned i = 0; i < 3; i++) { for (unsigned j = 0; j < 4; j++) {
        if (w580_esn.clase[i][j]) { g_string_append_printf(g, "NORCLASE\tregion_antes=%s\tprograma=%s\t%" PRIu64 "\n", m[i], c[j], w580_esn.clase[i][j]); }
    } }
}
#endif
