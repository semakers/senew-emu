/* Pista E6: EL PANEL DE VERDAD. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hasta aquí el modelo de pantalla (w580-lcd.inc) no ejecutaba nada: descodificaba el área de datos del PDI por la
 * forma de sus bits RS, guardaba los registros y pintaba lo que mandara el PDICON. Con `panel-real=on` (encendida sola
 * con `estricto=on`; apagada por omisión: sin ella NADA de este fichero actúa) la pantalla solo se ve si el firmware
 * hace lo que hay que hacer, en tres capas:
 *
 *  1. PDI: un INTÉRPRETE DEL MICROCÓDIGO. Al lanzar (+1DC <- 1 escribe, <- 3 lee; PDICON+04 <- 2 manda un cuadro) se
 *     ejecuta lo que haya EN LA MEMORIA DE PROGRAMA (+000..+0E0) desde su punto de entrada (0 para escribir; +1FC
 *     byte 2 para leer; +1FC byte 0 para el cuadro). Lo que sale por el bus del panel sale de ejecutar eso: si el
 *     programa no está cargado, está roto, o su cuenta no se ha parcheado, al panel no le llega lo que se quería.
 *     Semántica: DEDUCIDA de los cuatro programas de fábrica (dispdriver0.dat) y de cómo los usa el original
 *     (docs/emulador_estricto.md §10.1 dice, instrucción por instrucción, qué es [inferido] y qué [sin saber]).
 *  2. Panel: un R61505 por su bus de 8 bits (índice y dato de 16 bits, byte alto primero; sincronismo con cuatro
 *     ceros), con sus registros, su GRAM (que arranca con BASURA), ventana, cursor y modo de entrada, y un estado
 *     FÍSICO: solo se ve si está alimentado (R10, R12), fuera de reposo y con la visualización encendida (R07).
 *     Contesta a las lecturas de identificación (R0000, R0028 según `panel-variante`, y solo tras CALB).
 *  3. El GUION: con `estricto=on`, cada escritura de registro se coteja con los guiones de fábrica de la variante
 *     (init, init2, standby_from_normal…; tabla generada por tools/e6_panel_tabla.py): avisos `[panel]` por escritura
 *     fuera de guion o a destiempo, espera `%w` omitida, GRAM con el panel sin iniciar, ventana incoherente.
 *
 * De dónde sale cada cosa: el descriptor de fábrica es [observado en el fichero]; el comportamiento del R61505 es de
 * MEMORIA de su hoja de datos (no consultada aquí) = [inferido]; nada se ha visto en un panel.
 */
#include "w580-panel-tabla.inc"

#define W580_PR_W 240
#define W580_PR_H 320
enum { W580_PR_ESCRIBE, W580_PR_LEE, W580_PR_GC };
enum { PR_A_MICRO = 0x100, PR_A_BUS = 0x200, PR_A_GUION = 0x300, PR_A_ESPERA = 0x400, PR_A_GRAM = 0x500, PR_A_VENTANA = 0x600,
       PR_A_PDI = 0x700, PR_A_ID = 0x800, PR_A_FISICO = 0x900 };

static struct W580PanelReal {
    unsigned variante;
    /* bus de 8 bits */
    uint8_t o_alto, d_alto; bool o_medio, d_medio; unsigned ceros;
    uint16_t idx; unsigned lee_fase;
    /* GRAM y contador de dirección */
    uint16_t gram[W580_PR_W * W580_PR_H];
    unsigned ax, ay, hsa, hea, vsa, vea; bool gram_abierta, gram_ventana, fb_viejo;
    bool calb, perdido;
    /* guion */
    int g; unsigned pos; int64_t t_ult; unsigned espera_ms; char espera_de[48];
    bool init_hecho, init2_hecho, en_reposo, en_gc;
    /* PDI */
    bool leyo_pdi, leyo_pdicon; uint8_t leido;
    /* contadores */
    uint64_t n_escribe, n_lee, n_gc, n_bytes, n_pixeles, n_pix_perdidos, n_micro_mal, n_avisos, n_sin_leer, n_reg, n_fuera, n_esperas, n_sync, n_resinc;
    unsigned log;
    char ultimo_mal[160];
} w580_pr;

static void w580_luz_muestra(W580State *s);

static void G_GNUC_PRINTF(3, 4) w580_pr_aviso(W580State *s, uint32_t clave, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    w580_pr.n_avisos++;
    g_strlcpy(w580_pr.ultimo_mal, msg, sizeof(w580_pr.ultimo_mal));
    if (w580_es.on) { w580_es_aviso(s, "panel", clave, "%s", msg); }
    else if (w580_pr.log < 200) { w580_pr.log++;
        qemu_log_mask(LOG_UNIMP, "W580_PANEL_REAL %s (pc=0x%08x t=%" PRId64 " ms)\n", msg, s->cpu->env.regs[15], qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
    w580_lcd_traza(s, "AVISO\t%s", msg);
}

/* ------------------------------------------------------------------------------------------------ estado físico
 * [inferido, de memoria de la hoja del R61505 y de lo que dejan escritos los guiones de fábrica]:
 *   alimentado  R10 con los amplificadores en marcha (AP, bits 6:4 != 0) y sin STB/SLP/DSTB (bits 2:0);
 *               R12 con PON y PSON (0x0030: fábrica deja 0x01BE; el reposo, 0x0000)
 *   visible     además R07 con BASEE, GON, DTE y D1:D0 = 11 (0x0133; fábrica deja 0x0173; 0x0021/0x0061/0x0072/0x0001 no) */
static bool w580_pr_alimentado(W580LCD *l) { return (l->panel[0x10] & 0x0070) && !(l->panel[0x10] & 0x0007) && (l->panel[0x12] & 0x0030) == 0x0030; }
static bool w580_pr_visible(W580LCD *l) { return w580_pr_alimentado(l) && (l->panel[0x07] & 0x0133) == 0x0133; }
static const char *w580_pr_por_que_no(W580LCD *l)
{
    if (!(l->panel[0x10] & 0x0070)) { return "R10 sin amplificadores (AP = 0): sin alimentar"; }
    if (l->panel[0x10] & 0x0007) { return "R10 en reposo (STB/SLP/DSTB)"; }
    if ((l->panel[0x12] & 0x0030) != 0x0030) { return "R12 sin PON/PSON: sin alimentar"; }
    if ((l->panel[0x07] & 0x0003) != 3) { return "R07 con D1:D0 != 11: visualizacion apagada"; }
    if ((l->panel[0x07] & 0x0133) != 0x0133) { return "R07 sin BASEE/GON/DTE"; }
    return "";
}
static void w580_pr_basura(void)
{
    uint32_t x = 0x2545f491u ^ w580_es.semilla;
    for (unsigned i = 0; i < W580_PR_W * W580_PR_H; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w580_pr.gram[i] = x; }
    w580_pr.fb_viejo = true;
}
/* GRAM -> lo que se vería. Efectos de los registros que cambian la imagen [inferido de la hoja]: R01 SS (bit 8: sentido de las
 * fuentes; a 0 la imagen sale en espejo), R01 SM (bit 10: reparto par/impar de las puertas; si no es el del cristal de la
 * variante las filas salen repartidas en dos mitades), R60 GS (bit 15: sentido de las puertas, imagen boca abajo) y NL (líneas
 * excitadas), R61 REV (bit 0 a 1: negativo; fábrica lo deja a 0). BGR (R03 bit 12) se aplica al ESCRIBIR la GRAM. */
static void w580_pr_a_fb(W580LCD *l)
{
    struct W580PanelReal *p = &w580_pr;
    if (!p->fb_viejo) { return; }
    p->fb_viejo = false;
    bool ss = l->panel[0x01] & 0x0100, sm = l->panel[0x01] & 0x0400, gs = l->panel[0x60] & 0x8000, rev = l->panel[0x61] & 1;
    bool sm_cristal = w580_pr_variantes[p->variante].sm_cristal;     /* (árbol público) campo de la variante, no el guion init */
    unsigned lineas = MIN(320u, 8 * (((l->panel[0x60] >> 8) & 0x3f) + 1));
    for (unsigned y = 0; y < W580_PR_H; y++) {
        unsigned sy = gs ? W580_PR_H - 1 - y : y;
        if (sm != sm_cristal) { sy = sy < 160 ? 2 * sy : 2 * (sy - 160) + 1; }
        for (unsigned x = 0; x < W580_PR_W; x++) {
            unsigned sx = ss ? x : W580_PR_W - 1 - x;
            uint16_t v = p->gram[sy * W580_PR_W + sx];
            if (rev) { v = ~v; }
            unsigned r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
            l->fb[y * W580_PR_W + x] = y < lineas ? (r << 16) | (g << 8) | b : 0;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ el guion de fábrica */
static const W580PrGuion *w580_pr_guion(int g) { return &w580_pr_variantes[w580_pr.variante].g[g]; }
static bool w580_pr_casa(const W580PrPaso *q, unsigned reg, unsigned val) { return (q->reg == 0xFFFF || q->reg == reg) && q->val == val; }
/* ¿Se ha cumplido la espera que el guion pone detrás del último paso casado? Vale para cualquier acceso al bus del panel. */
static void w580_pr_mira_espera(W580State *s, const char *que)
{
    struct W580PanelReal *p = &w580_pr;
    if (!p->espera_ms) { return; }
    int64_t ms10 = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - p->t_ult) / 100000;
    if (ms10 < (int64_t)p->espera_ms * 10) {
        p->n_esperas++;
        w580_pr_aviso(s, PR_A_ESPERA | p->espera_ms, "espera omitida: %s a %d.%d ms de %s; el guion de fabrica espera %u ms", que, (int)(ms10 / 10), (int)(ms10 % 10), p->espera_de, p->espera_ms);
    }
    p->espera_ms = 0;
}
static void w580_pr_paso_hecho(W580State *s, int g, unsigned k, unsigned reg, unsigned val)
{
    struct W580PanelReal *p = &w580_pr;
    const W580PrGuion *gu = w580_pr_guion(g);
    p->g = g; p->pos = k + 1; p->t_ult = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); p->espera_ms = gu->p[k].espera;
    snprintf(p->espera_de, sizeof(p->espera_de), "R%02X <- %04X (%s, paso %u)", reg, val, gu->nombre, k + 1);
    if (g == W580_PR_G_INIT && k == 0) { p->init_hecho = p->init2_hecho = false; }
    if (p->pos == gu->n) {
        if (g == W580_PR_G_STANDBY_FROM_NORMAL || g == W580_PR_G_SHUTDOWN) { p->en_reposo = true; p->init_hecho = p->init2_hecho = false; }
        if (g == W580_PR_G_INIT) { p->init_hecho = true; p->en_reposo = false; }
        if (g == W580_PR_G_INIT2) { p->init2_hecho = true; }
    }
}
static void w580_pr_guion_ev(W580State *s, unsigned reg, unsigned val)
{
    struct W580PanelReal *p = &w580_pr;
    static const int empiezan[] = { W580_PR_G_INIT, W580_PR_G_INIT2, W580_PR_G_STANDBY_FROM_NORMAL, W580_PR_G_SHUTDOWN, W580_PR_G_SET_BPP_MODE, W580_PR_G_SET_BPP_MODE_HIGH };
    if (!w580_pr_variantes[p->variante].g[W580_PR_G_INIT].n) { return; }      /* (árbol público) sin guiones de fábrica: nada con qué cotejar */
    g_autofree char *que = g_strdup_printf("R%02X <- %04X", reg, val);
    w580_pr_mira_espera(s, que);
    if (p->en_gc && (reg == 0x20 || reg == 0x21)) { return; }        /* el cursor que pone el programa del cuadro: no es del guion */
    const W580PrGuion *gu = p->g >= 0 ? w580_pr_guion(p->g) : NULL;
    if (gu && p->pos < gu->n && w580_pr_casa(&gu->p[p->pos], reg, val)) { w580_pr_paso_hecho(s, p->g, p->pos, reg, val); return; }
    for (unsigned i = 0; i < ARRAY_SIZE(empiezan); i++) {
        const W580PrGuion *n = w580_pr_guion(empiezan[i]);
        if (n->n && w580_pr_casa(&n->p[0], reg, val)) {
            if (gu && p->pos > 1 && p->pos < gu->n && (p->g == W580_PR_G_INIT || p->g == W580_PR_G_INIT2)) {
                w580_pr_aviso(s, PR_A_GUION | 0x80 | p->g, "guion %s abandonado en el paso %u de %u (se esperaba R%02X <- %04X) al empezar %s", gu->nombre, p->pos, gu->n,
                              gu->p[p->pos].reg, gu->p[p->pos].val, n->nombre);
            }
            w580_pr_paso_hecho(s, empiezan[i], 0, reg, val); return;
        }
    }
    {   /* la salida del reposo son datos sin índice: solo casa si el panel estaba en reposo */
        const W580PrGuion *n = w580_pr_guion(W580_PR_G_NORMAL_FROM_STANDBY);
        if (p->en_reposo && p->g != W580_PR_G_NORMAL_FROM_STANDBY && n->n && w580_pr_casa(&n->p[0], reg, val)) { w580_pr_paso_hecho(s, W580_PR_G_NORMAL_FROM_STANDBY, 0, reg, val); return; }
    }
    if (gu) {
        for (unsigned k = p->pos + 1; k < gu->n; k++) {
            if (w580_pr_casa(&gu->p[k], reg, val)) {
                p->n_fuera++;
                w580_pr_aviso(s, PR_A_GUION | 0x40 | p->g, "orden a destiempo: R%02X <- %04X es el paso %u de %s y tocaba el %u (R%02X <- %04X): se salta %u", reg, val, k + 1, gu->nombre,
                              p->pos + 1, gu->p[p->pos].reg, gu->p[p->pos].val, k - p->pos);
                w580_pr_paso_hecho(s, p->g, k, reg, val); return;
            }
        }
    }
    p->n_fuera++;
    if (gu && p->pos < gu->n) {
        w580_pr_aviso(s, PR_A_GUION | (reg & 0xff) << 16, "fuera de guion: R%02X <- %04X; en %s (variante %s) tocaba el paso %u, R%02X <- %04X", reg, val, gu->nombre,
                      w580_pr_variantes[p->variante].corto, p->pos + 1, gu->p[p->pos].reg, gu->p[p->pos].val);
    } else {
        w580_pr_aviso(s, PR_A_GUION | (reg & 0xff) << 16, "fuera de guion: R%02X <- %04X no es el paso siguiente ni el comienzo de ningun guion de fabrica de la variante %s", reg, val,
                      w580_pr_variantes[p->variante].corto);
    }
}

/* ------------------------------------------------------------------------------------------------ R61505: registros y GRAM */
static void w580_pr_reg_w(W580State *s, unsigned reg, unsigned val)
{
    struct W580PanelReal *p = &w580_pr; W580LCD *l = s->lcd;
    p->n_reg++;
    bool antes = w580_pr_visible(l);
    bool cambia = reg < 256 && l->panel[reg] != val;
    w580_pr_guion_ev(s, reg, val);
    if (reg == 0xA4 && (val & 1)) { p->calb = true; }       /* CALB: la NVM pasa a los registros [inferido del comentario del descriptor] */
    if (reg == 0x10 && (val & 0x0004) && !p->perdido) {     /* reposo profundo: la GRAM no se conserva [inferido de la hoja] */
        w580_pr_basura(); p->perdido = true; p->calb = false;
    }
    if (reg == 0x10 && !(val & 0x0007)) { p->perdido = false; }
    if (reg < 256) { l->panel[reg] = val; }
    w580_lcd_traza(s, "PANEL\t0x%04x\t0x%04x", reg, val);
    if (reg == 0x01 || reg == 0x60 || reg == 0x61) { p->fb_viejo = true; l->dirty = true; }
    if (reg == 0x07 || reg == 0x10 || reg == 0x12) {
        l->dirty = true; w580_luz_muestra(s);
        if (cambia && l->log_panel < 400) { l->log_panel++;
            qemu_log_mask(LOG_UNIMP, "W580_PANEL reg=0x%02x <- 0x%04x t=%" PRId64 " ms (cuadro %" PRIu64 ")%s\n", reg, val, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), l->cuadros,
                          antes != w580_pr_visible(l) ? (antes ? " -> deja de verse" : " -> SE VE") : "");
        }
    }
}
/* Se elige R22: el contador de dirección toma el cursor (R20, R21) y se mira la ventana. */
static void w580_pr_gram_abre(W580State *s)
{
    struct W580PanelReal *p = &w580_pr; W580LCD *l = s->lcd;
    unsigned x = l->panel[0x20], y = l->panel[0x21];
    p->hsa = l->panel[0x50]; p->hea = l->panel[0x51]; p->vsa = l->panel[0x52]; p->vea = l->panel[0x53];
    p->gram_ventana = p->hsa <= p->hea && p->hea < W580_PR_W && p->vsa <= p->vea && p->vea < W580_PR_H && x >= p->hsa && x <= p->hea && y >= p->vsa && y <= p->vea;
    if (!p->gram_ventana) {
        w580_pr_aviso(s, PR_A_VENTANA, "ventana incoherente al abrir la GRAM: R50..R53 = [%u..%u]x[%u..%u], cursor R20,R21 = (%u,%u)", p->hsa, p->hea, p->vsa, p->vea, x, y);
        p->hsa = 0; p->hea = W580_PR_W - 1; p->vsa = 0; p->vea = W580_PR_H - 1;
    }
    p->ax = MIN(x, W580_PR_W - 1u); p->ay = MIN(y, W580_PR_H - 1u);
    p->gram_abierta = !(l->panel[0x10] & 0x0007);           /* en reposo el panel no admite datos de imagen [inferido de la hoja] */
    w580_pr_mira_espera(s, "abrir la GRAM (R22)");
    if (!p->init_hecho && w580_pr_variantes[p->variante].g[W580_PR_G_INIT].n) {      /* (árbol público) sin guion init no se puede saber */
        w580_pr_aviso(s, PR_A_GRAM | 1, "escritura de GRAM (R22) con el panel sin iniciar: el guion init de la variante %s no se ha completado%s", w580_pr_variantes[p->variante].corto,
                      p->gram_abierta ? "" : "; el panel esta en reposo y la pierde");
    } else if (!p->gram_abierta) {
        w580_pr_aviso(s, PR_A_GRAM | 2, "escritura de GRAM (R22) con el panel en reposo (R10 = %04X): se pierde", l->panel[0x10]);
    }
    if (l->panel[0x03] & 0xC000) { w580_pr_aviso(s, PR_A_GRAM | 3, "R03 = %04X: modo de 18 bpp (TRI/DFM), sin modelo: los pixeles se toman de 16 en 16 bits", l->panel[0x03]); }
}
static inline void w580_pr_px(W580State *s, uint16_t v)
{
    struct W580PanelReal *p = &w580_pr; unsigned r03 = s->lcd->panel[0x03];
    if (!p->gram_abierta) { p->n_pix_perdidos++; return; }
    if (!(r03 & 0x1000)) { v = (v >> 11) | (v & 0x07e0) | (v << 11); }      /* BGR = 0: el cristal está cableado al revés (fábrica: 0x1230) [inferido] */
    p->gram[p->ay * W580_PR_W + p->ax] = v; p->n_pixeles++;
    bool id0 = r03 & 0x10, id1 = r03 & 0x20;
    if (!(r03 & 0x08)) {                                                    /* AM = 0: avanza en horizontal */
        if (id0 ? p->ax >= p->hea : p->ax <= p->hsa) {
            p->ax = id0 ? p->hsa : p->hea;
            if (id1 ? p->ay >= p->vea : p->ay <= p->vsa) { p->ay = id1 ? p->vsa : p->vea; } else { p->ay += id1 ? 1 : -1; }
        } else { p->ax += id0 ? 1 : -1; }
    } else {
        if (id1 ? p->ay >= p->vea : p->ay <= p->vsa) {
            p->ay = id1 ? p->vsa : p->vea;
            if (id0 ? p->ax >= p->hea : p->ax <= p->hsa) { p->ax = id0 ? p->hsa : p->hea; } else { p->ax += id0 ? 1 : -1; }
        } else { p->ay += id1 ? 1 : -1; }
    }
}
/* Un byte ESCRITO al panel (flanco de /WR). dato = RS a 1. */
static void w580_pr_bus_w(W580State *s, uint8_t v, bool dato)
{
    struct W580PanelReal *p = &w580_pr;
    p->n_bytes++;
    if (!dato) {
        p->d_medio = false; p->lee_fase = 0;
        /* Sincronismo [inferido de la hoja: «transfer synchronization»]: al cuarto byte de orden a cero seguido el panel
         * vuelve a esperar el byte ALTO. Los ceros se procesan como cualquier índice (pares = elegir R0000). */
        bool cero = v == 0;
        if (!cero) { p->ceros = 0; }
        if (!p->o_medio) { p->o_alto = v; p->o_medio = true; }
        else { p->o_medio = false; p->idx = (p->o_alto << 8) | v; if (p->idx == 0x22) { w580_pr_gram_abre(s); } }
        if (cero && (++p->ceros & 3) == 0) { if (p->o_medio) { p->n_resinc++; } p->o_medio = false; p->idx = 0; p->n_sync += p->ceros == 4; }
        return;
    }
    p->ceros = 0;
    if (p->o_medio) {
        p->o_medio = false;
        w580_pr_aviso(s, PR_A_BUS | 1, "bus del panel: un dato tras MEDIO indice (un solo byte de orden): el panel pierde el paso alto/bajo hasta el proximo sincronismo");
    }
    if (!p->d_medio) { p->d_alto = v; p->d_medio = true; return; }
    p->d_medio = false;
    unsigned val = (p->d_alto << 8) | v;
    if (p->idx == 0x22) { w580_pr_px(s, val); s->lcd->dirty = true; p->fb_viejo = true; }
    else { w580_pr_reg_w(s, p->idx, val); }
}
/* Un byte LEÍDO del panel (flanco de /RD): primero el alto y luego el bajo del registro elegido [inferido de los programas
 * readhigh y read: el primero manda el índice y lee un byte; el segundo lee otro sin mandar nada]. */
static uint8_t w580_pr_bus_r(W580State *s)
{
    struct W580PanelReal *p = &w580_pr; W580LCD *l = s->lcd;
    unsigned reg = p->idx, val;
    w580_pr_mira_espera(s, "una lectura");
    if (reg == 0x00) { val = l->codigo; }
    else if (reg == W580_PR_REG_ID) {
        if (s->panel_r28 != UINT32_MAX) { val = s->panel_r28 & 0xffff; }
        else if (!p->calb) {
            val = 0;
            w580_pr_aviso(s, PR_A_ID | 1, "lectura de R%04X (el identificador de variante) sin CALB (R A4 <- 0001) desde el reset: la NVM no esta en los registros; se contesta 0", reg);
        } else { int id = w580_pr_variantes[p->variante].id; val = id < 0 ? 0x0005 : id; }
    }
    else { val = reg < 256 ? l->panel[reg] : 0; }
    uint8_t b = p->lee_fase ? val & 0xff : val >> 8;
    if (l->log_lee < 300) { l->log_lee++;
        qemu_log_mask(LOG_UNIMP, "W580_PANEL lee reg=0x%04x byte %s = 0x%02x (valor 0x%04x) t=%" PRId64 " ms pc=0x%08x lr=0x%08x\n", reg, p->lee_fase ? "bajo" : "alto", b, val,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    w580_lcd_traza(s, "LEE\t0x%04x\t%s\t0x%02x", reg, p->lee_fase ? "bajo" : "alto", b);
    p->lee_fase ^= 1;
    return b;
}

/* ------------------------------------------------------------------------------------------------ PDI: el intérprete
 * Instrucción = (pines, operación) [+ inmediato de 16 bits]. Ver docs/emulador_estricto.md §10.1.
 *   pines  bit 3 = /CS, bit 1 = /WR, bit 4 = /RD [inferido: 1F reposo, 17 elegido, 15 escribe, 07 lee]
 *   20/28 + inm   configuración del puerto (28: 999C = bus de salida, 9999 = bus de entrada) [inferido]
 *   18/1D + inm   comienzo [sin saber qué es el inmediato: 0000 al leer, 0009 al escribir]
 *   98 + inm      cuenta de bytes del área de datos: los ÚLTIMOS inm bytes de +0xx..+0FF, con su bit RS [observado: es lo
 *                 que el original parchea antes de cada orden]
 *   05            saca el byte en curso del área de datos [inferido]
 *   F8            siguiente byte; si quedan, vuelve a la instrucción que sigue a la cuenta [inferido]
 *   06 / 07       muestrea el bus / deja el byte leído en +1F4 bits 8..15 [inferido]
 *   08 / 0D       fin (0D: fin del programa de cuadro)
 *   00 01 60 A0 A8 C0 C8 D0 E0 E8   solo en el programa de cuadro [sin saber]: se aceptan y no hacen nada; lo que cuenta
 *                 son los flancos de /WR: los primeros «cuenta» sacan el área de datos y el siguiente abre los píxeles
 *   cualquier otra: aviso y no hace nada (permisivo) */
typedef struct { unsigned bytes, lecturas; bool pixeles, fin; } W580PrRes;
static void w580_pr_pixeles(W580State *s);
static bool w580_pr_op_inm(uint8_t op) { return op == 0x18 || op == 0x1d || op == 0x20 || op == 0x28 || op == 0x98; }
static bool w580_pr_op_sabida(uint8_t op)
{
    static const uint8_t t[] = { 0x00, 0x01, 0x05, 0x06, 0x07, 0x08, 0x0d, 0x18, 0x1d, 0x20, 0x28, 0x60, 0x98, 0xa0, 0xa8, 0xc0, 0xc8, 0xd0, 0xe0, 0xe8, 0xf8 };
    for (unsigned i = 0; i < sizeof(t); i++) { if (t[i] == op) { return true; } }
    return false;
}
static W580PrRes w580_pr_ejecuta(W580State *s, unsigned entrada, int modo)
{
    struct W580PanelReal *p = &w580_pr;
    W580PrRes r = { 0 };
    static const char *const nom[] = { "escritura", "lectura", "cuadro" };
    if (entrada > 0xe0) {
        p->n_micro_mal++;
        w580_pr_aviso(s, PR_A_MICRO | modo, "microcodigo: el programa de %s no esta cargado (punto de entrada 0x%02X): al panel no le llega nada", nom[modo], entrada);
        return r;
    }
    unsigned pc = entrada, pasos = 0, cnt = 0, n = 0, bucle = 0xffff, pines_ant = 0x1f, rs_xor = w580_latch_get(s, 0xf7001120, 1) & 1;
    uint32_t rs = w580_latch_get(s, 0xf700111c, 4);
    bool hay_cuenta = false;
    while (!r.fin) {
        if (++pasos > 4096 || pc + 1 > 0xe0) {
            p->n_micro_mal++;
            w580_pr_aviso(s, PR_A_MICRO | 0x10 | modo, "microcodigo: el programa de %s (entrada 0x%02X) %s", nom[modo], entrada,
                          pasos > 4096 ? "no termina (4096 instrucciones sin fin)" : "se sale de la memoria de programa sin una instruccion de fin");
            break;
        }
        unsigned pines = w580_latch_get(s, 0xf7001000 + pc, 1), op = w580_latch_get(s, 0xf7001001 + pc, 1), inm = 0;
        unsigned aqui = pc; pc += 2;
        if (w580_pr_op_inm(op)) { inm = w580_latch_get(s, 0xf7001000 + pc, 1) | (w580_latch_get(s, 0xf7001001 + pc, 1) << 8); pc += 2; }
        if (!w580_pr_op_sabida(op)) {
            p->n_micro_mal++;
            w580_pr_aviso(s, PR_A_MICRO | 0x20 | op << 8, "microcodigo: instruccion %02X %02X en +0x%02X (programa de %s) que los programas de fabrica no usan: sin modelo, se ignora", pines, op, aqui, nom[modo]);
        }
        bool cs = !(pines & 0x08), wr_baja = (pines_ant & 0x02) && !(pines & 0x02), rd_baja = (pines_ant & 0x10) && !(pines & 0x10);
        pines_ant = pines;
        if (op == 0x98) {
            cnt = n = inm; bucle = pc; hay_cuenta = true;
            if (n == 0 || n > 32) {
                p->n_micro_mal++;
                w580_pr_aviso(s, PR_A_MICRO | 0x30 | modo, "microcodigo: cuenta de bytes %u en el programa de %s (+0x%02X): el area de datos lleva de 1 a 32", n, nom[modo], aqui + 2);
                if (n > 32) { cnt = n = 32; }
            }
        }
        if (cs && wr_baja) {
            if (cnt) {
                unsigned k = n - cnt; cnt--;
                uint8_t v = w580_latch_get(s, 0xf7001100 - n + k, 1);
                bool dato = ((rs >> (32 - n + k)) & 1) ^ rs_xor;
                r.bytes++;
                w580_pr_bus_w(s, v, dato);
            } else if (modo == W580_PR_GC && hay_cuenta) {
                if (!r.pixeles) { r.pixeles = true; w580_pr_pixeles(s); }
            } else {
                p->n_micro_mal++;
                w580_pr_aviso(s, PR_A_MICRO | 0x40 | modo, "microcodigo: el programa de %s da un pulso de /WR sin bytes que mandar (cuenta %s)", nom[modo], hay_cuenta ? "agotada" : "sin cargar");
            }
        }
        if (cs && rd_baja) { p->leido = w580_pr_bus_r(s); r.lecturas++; }
        switch (op) {
        case 0xf8: if (cnt && bucle != 0xffff) { pc = bucle; } break;
        case 0x07: w580_latch_set(s, 0xf70011f4, 4, (uint32_t)p->leido << 8); break;
        case 0x08: case 0x0d: r.fin = true; break;
        default: break;
        }
    }
    if (r.fin && cnt) {
        p->n_micro_mal++;
        w580_pr_aviso(s, PR_A_MICRO | 0x50 | modo, "microcodigo: el programa de %s acaba con %u de %u bytes sin mandar", nom[modo], cnt, n);
    }
    return r;
}
/* Reglas del lado del PDI, comunes a los tres lanzamientos. Calibradas con el original (docs/emulador_estricto.md §10.3). */
static void w580_pr_mira_pdi(W580State *s, int modo)
{
    struct W580PanelReal *p = &w580_pr;
    uint32_t f8 = w580_latch_get(s, 0xf70011f8, 4);
    unsigned vel = (f8 >> 8) & 0xff, min = modo == W580_PR_LEE ? W580_PR_VEL_READ : modo == W580_PR_GC ? W580_PR_VEL_GC : W580_PR_VEL_WRITE;
    if (!(p->leyo_pdi && p->leyo_pdicon)) {
        p->n_sin_leer++;
        w580_pr_aviso(s, PR_A_PDI | 1, "lanza %s sin haber mirado antes los dos bits de ocupado (PDI+1F4%s, PDICON+08%s) desde el lanzamiento anterior",
                      modo == W580_PR_GC ? "un cuadro" : modo == W580_PR_LEE ? "una lectura" : "una orden", p->leyo_pdi ? " si" : " NO", p->leyo_pdicon ? " si" : " NO");
    }
    p->leyo_pdi = p->leyo_pdicon = false;
    if (vel < min) {
        w580_pr_aviso(s, PR_A_PDI | 0x10 | modo, "velocidad del PDI (+1F8 bits 8..15) = 0x%02X al lanzar %s: mas rapido que el 0x%02X del descriptor de fabrica", vel,
                      modo == W580_PR_GC ? "un cuadro" : modo == W580_PR_LEE ? "una lectura" : "una orden", min);
    }
    if (modo == W580_PR_GC ? !(f8 & 0x00100000) || (f8 & 0x00800000) : (f8 & 0x00900000) != 0) {
        w580_pr_aviso(s, PR_A_PDI | 0x20 | modo, "PDI+1F8 = %08X al lanzar %s: el original %s", f8, modo == W580_PR_GC ? "un cuadro" : "una orden o lectura",
                      modo == W580_PR_GC ? "cede antes el PDI al PDICON (bit 20 a 1, bit 23 a 0)" : "lo toma antes (bits 20 y 23 a 0)");
    }
}
/* +1DC <- 1 (escribe), <- 3 (lee). Las demás (2 = calibrar [sin saber], 5 = reconocer la interrupción) no hacen nada. */
static void w580_pr_pdi(W580State *s, uint32_t orden)
{
    struct W580PanelReal *p = &w580_pr; W580LCD *l = s->lcd;
    if (orden != 1 && orden != 3) {
        if (orden != 5) { w580_pr_aviso(s, PR_A_PDI | 0x40 | (orden & 15), "PDI+1DC <- %u: sin modelo (el original solo lanza 1 y 3)", orden); }
        return;
    }
    int modo = orden == 1 ? W580_PR_ESCRIBE : W580_PR_LEE;
    uint32_t fc = w580_latch_get(s, 0xf70011fc, 4);
    w580_pr_mira_pdi(s, modo);
    if (modo == W580_PR_ESCRIBE) { p->n_escribe++; } else { p->n_lee++; }
    W580PrRes r = w580_pr_ejecuta(s, modo == W580_PR_ESCRIBE ? 0 : (fc >> 16) & 0xff, modo);
    if (modo == W580_PR_LEE && !r.lecturas) {
        w580_pr_aviso(s, PR_A_PDI | 0x50, "el programa de lectura (entrada 0x%02X) no ha leido nada del panel: +1F4 no cambia", (fc >> 16) & 0xff);
    }
    if (l->log_pdi < 600) { l->log_pdi++;
        char hex[80]; unsigned n = 0;
        for (hwaddr a = 0xf70010e8; a < 0xf7001100; a++) { n += snprintf(hex + n, 4, "%02x", (unsigned)w580_latch_get(s, a, 1)); }
        qemu_log_mask(LOG_UNIMP, "W580_PDI orden=%u datos[E8..FF]=%s rs=0x%08x 1FC=0x%08x -> %u bytes al panel, %u leidos%s pc=0x%08x lr=0x%08x\n", orden, hex,
                      (unsigned)w580_latch_get(s, 0xf700111c, 4), fc, r.bytes, r.lecturas, r.fin ? "" : " (SIN FIN)", s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    w580_lcd_traza(s, "PDI\t%s\t0x%02x\t%u\t%u", modo == W580_PR_ESCRIBE ? "escribe" : "lee", modo == W580_PR_ESCRIBE ? 0 : (fc >> 16) & 0xff, r.bytes, r.lecturas);
}
/* El cuadro: parámetros del PDICON que deja preparados w580_pdicon_transfer antes de ejecutar el programa «gc». */
static struct { uint32_t org, fila; unsigned x0, y0, w, h; W580CaLectura cl; bool hecho; } w580_pr_cuadro;
static void w580_pr_pixeles(W580State *s)
{
    struct W580PanelReal *p = &w580_pr;
    uint32_t tab[4];
    for (unsigned i = 0; i < 4; i++) { tab[i] = w580_latch_get(s, 0xf70014e4 + 4 * i, 4); }
    bool de_fabrica = !memcmp(tab, w580_pr_colores16, sizeof(tab));
    if (!de_fabrica) {
        w580_pr_aviso(s, PR_A_PDI | 0x60, "tabla de encaminamiento de bits del PDICON (+E4..+F0) = %08X %08X %08X %08X, no la de 16 bpp del descriptor: los colores salen como los encamine", tab[0], tab[1], tab[2], tab[3]);
    }
    unsigned w = w580_pr_cuadro.w, h = w580_pr_cuadro.h;
    uint16_t *linea = g_new(uint16_t, w);
    for (unsigned y = 0; y < h; y++) {
        hwaddr dir = 0x20000000 + w580_pr_cuadro.org + ((hwaddr)(w580_pr_cuadro.y0 + y) * w580_pr_cuadro.fila + w580_pr_cuadro.x0) * 2;
        w580_ca.hw = true;
        bool leido = address_space_read(&address_space_memory, dir, MEMTXATTRS_UNSPECIFIED, linea, w * 2) == MEMTX_OK;
        w580_ca.hw = false;
        if (!leido) { break; }
        w580_ca_hw_lee(s, dir, (uint8_t *)linea, w * 2, &w580_pr_cuadro.cl);
        for (unsigned x = 0; x < w; x++) {
            uint16_t v = le16_to_cpu(linea[x]);
            if (!de_fabrica) {      /* RGB565 -> los 24 bits internos (B 0..7, G 8..15, R 16..23) -> 16 bits de salida según la tabla [inferido de la numeración de la tabla] */
                uint32_t rgb = ((v >> 11) << 19) | (((v >> 5) & 63) << 10) | ((v & 31) << 3);
                unsigned b0 = 0, b1 = 0;
                for (unsigned k = 0; k < 8; k++) {
                    b0 |= ((rgb >> ((tab[k / 4] >> (8 * (k % 4))) & 31)) & 1) << k;
                    b1 |= ((rgb >> ((tab[2 + k / 4] >> (8 * (k % 4))) & 31)) & 1) << k;
                }
                v = (b0 << 8) | b1;
            }
            if (p->idx == 0x22 && !p->d_medio && !p->o_medio) { w580_pr_px(s, v); }       /* camino rápido: el bus está alineado */
            else { w580_pr_bus_w(s, v >> 8, true); w580_pr_bus_w(s, v & 0xff, true); }
        }
    }
    g_free(linea);
    p->fb_viejo = true; s->lcd->dirty = true;
    w580_pr_cuadro.hecho = true;
}
/* PDICON+04 <- 2 con panel-real. Lo llama w580_pdicon_transfer con el rectángulo ya validado. */
static void w580_pr_pdicon(W580State *s, uint32_t org, uint32_t fila, unsigned x0, unsigned y0, unsigned w, unsigned h)
{
    struct W580PanelReal *p = &w580_pr; W580LCD *l = s->lcd;
    uint32_t fc = w580_latch_get(s, 0xf70011fc, 4);
    w580_pr_mira_pdi(s, W580_PR_GC);
    if (w580_latch_get(s, 0xf7001440, 4) != 0x014000f0 || w580_latch_get(s, 0xf7001444, 4) != 16) {
        w580_pr_aviso(s, PR_A_PDI | 0x70, "PDICON+40/+44 = %08X / %u: el original declara un panel de 240x320 y 16 bpp (0x014000F0 / 16)", (unsigned)w580_latch_get(s, 0xf7001440, 4), (unsigned)w580_latch_get(s, 0xf7001444, 4));
    }
    /* +5C: el original pone AHÍ TAMBIÉN la esquina del rectángulo (x | y << 16), igual que en +48 [observado: 0x00FA0000 con el
     * rectángulo en y = 250; `pdicon_set_offset`, 0x441ADD40, desde gld_display_refresh]; +60 (giro/espejo) siempre 0.
     * [sin saber] cuál de los dos usa el PDICON para situarse DENTRO del búfer de origen: con el original no se distinguen.
     * El modelo permisivo lee desde +48 (pista E3). Este, PESIMISTA, lee desde +5C: quien no lo ponga como el original ve
     * en la banda las filas de arriba del búfer. Si no coinciden, aviso. */
    uint32_t c5 = w580_latch_get(s, 0xf700145c, 4);
    unsigned sx = c5 & 0x7ff, sy = (c5 >> 16) & 0x7ff;
    if (c5 != w580_latch_get(s, 0xf7001448, 4) || w580_latch_get(s, 0xf7001460, 4)) {
        w580_pr_aviso(s, PR_A_PDI | 0x71, "PDICON+5C = %08X con el rectangulo +48 = %08X (+60 = %08X): el original pone en +5C la misma esquina que en +48 y deja +60 a 0; no se sabe cual usa el PDICON para leer del bufer: "
                      "este modelo lee desde +5C (fila %u), no desde la del rectangulo (fila %u)", c5, (unsigned)w580_latch_get(s, 0xf7001448, 4), (unsigned)w580_latch_get(s, 0xf7001460, 4), sy, y0);
    }
    w580_pr_cuadro.org = org; w580_pr_cuadro.fila = fila; w580_pr_cuadro.x0 = sx; w580_pr_cuadro.y0 = sy; w580_pr_cuadro.w = w; w580_pr_cuadro.h = h;
    w580_pr_cuadro.hecho = false; memset(&w580_pr_cuadro.cl, 0, sizeof(w580_pr_cuadro.cl));
    p->n_gc++; p->en_gc = true; p->gram_abierta = false;
    uint64_t pix0 = p->n_pixeles;
    W580PrRes r = w580_pr_ejecuta(s, fc & 0xff, W580_PR_GC);
    p->en_gc = false;
    if (!r.pixeles) {
        w580_pr_aviso(s, PR_A_PDI | 0x72, "el programa de cuadro (entrada 0x%02X) no ha llegado a mandar los pixeles: el panel no recibe el cuadro", fc & 0xff);
    } else if (p->idx != 0x22) {
        w580_pr_aviso(s, PR_A_GRAM | 4, "el cuadro llega al panel con el registro R%04X elegido, no R0022: el area de datos no es `00 20 xh xl 00 21 yh yl 00 22`; los pixeles %s",
                      p->idx, "se escriben en ese registro, no en la GRAM");
    } else {
        /* el rectángulo frente a la ventana y el cursor: si no casan, el panel envuelve por SU ventana y la imagen sale descolocada */
        unsigned vw = p->hea - p->hsa + 1;
        if (p->gram_ventana && (w != vw || l->panel[0x20] != x0 || l->panel[0x21] != y0)) {
            w580_pr_aviso(s, PR_A_VENTANA | 1, "ventana incoherente con el cuadro: rectangulo del PDICON (%u,%u) %ux%u, cursor del panel (%u,%u), ventana de %u de ancho: las filas no caen donde estan en el bufer",
                          x0, y0, w, h, l->panel[0x20], l->panel[0x21], vw);
        }
    }
    w580_lcd_traza(s, "PDICON\t%" PRIu64 "\t0x%x\t%u\t%u\t%u\t%u\t%s\t%u,%u\t%u..%u x %u..%u\t0x%04x\t%s\tgc=0x%02x bytes=%u pixeles=%" PRIu64, l->cuadros + 1, org, x0, y0, w, h,
                   r.pixeles && p->idx == 0x22 ? "cursor" : "SIN_CURSOR", l->panel[0x20], l->panel[0x21], l->panel[0x50], l->panel[0x51], l->panel[0x52], l->panel[0x53], l->panel[3],
                   w580_pr_visible(l) ? "encendido" : "dormido", fc & 0xff, r.bytes, p->n_pixeles - pix0);
    w580_ca_pdicon_fin(s, &w580_pr_cuadro.cl, org, x0, y0, w, h);
    l->cuadros++; l->dirty = true;
}
/* Lecturas de los bits de ocupado (no se consumen: siguen al cerrojo, que da «libre»). */
static void w580_pr_lee_estado(W580State *s, hwaddr addr)
{
    if (addr == 0xf70011f4) { w580_pr.leyo_pdi = true; }
    else if (addr == 0xf7001408) { w580_pr.leyo_pdicon = true; }
}
static void w580_pr_reset(W580State *s)
{
    struct W580PanelReal *p = &w580_pr;
    unsigned v = p->variante;
    memset(p, 0, offsetof(struct W580PanelReal, n_escribe));
    p->variante = v; p->g = -1;
    w580_pr_basura();
}
static char *w580_get_panel_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj); W580LCD *l = s->lcd; struct W580PanelReal *p = &w580_pr;
    GString *g = g_string_new("");
    const W580PrVariante *v = &w580_pr_variantes[p->variante];
    char idt[24]; snprintf(idt, sizeof(idt), "%04X", v->id);
    g_string_append_printf(g, "panel-real=%s variante=%s («%s», R%04X & %04X = %s)\n", s->panel_real ? "on" : "off", v->corto, v->nombre, W580_PR_REG_ID, W580_PR_REG_MASC,
                           v->id < 0 ? "ninguno de los tres" : idt);
    if (!s->panel_real) { g_string_append(g, "(sin panel-real el modelo de pantalla es el permisivo de siempre)\n"); return g_string_free(g, false); }
    g_string_append_printf(g, "fisico: %s%s%s | R07=%04X R10=%04X R11=%04X R12=%04X R01=%04X R03=%04X R60=%04X R61=%04X ventana=[%u..%u]x[%u..%u] cursor=(%u,%u) CALB=%s\n",
                           w580_pr_visible(l) ? "SE VE" : w580_pr_alimentado(l) ? "alimentado, NO se ve" : "NO se ve", w580_pr_visible(l) ? "" : ": ", w580_pr_por_que_no(l),
                           l->panel[7], l->panel[0x10], l->panel[0x11], l->panel[0x12], l->panel[1], l->panel[3], l->panel[0x60], l->panel[0x61],
                           l->panel[0x50], l->panel[0x51], l->panel[0x52], l->panel[0x53], l->panel[0x20], l->panel[0x21], p->calb ? "si" : "no");
    g_string_append_printf(g, "guion: %s paso %u de %u | init %s, init2 %s%s\n", p->g >= 0 ? w580_pr_guion(p->g)->nombre : "(ninguno)", p->pos, p->g >= 0 ? w580_pr_guion(p->g)->n : 0,
                           p->init_hecho ? "completo" : "SIN completar", p->init2_hecho ? "completo" : "sin completar", p->en_reposo ? ", en reposo" : "");
    uint32_t fc = w580_latch_get(s, 0xf70011fc, 4);
    g_string_append_printf(g, "pdi: escrituras=%" PRIu64 " lecturas=%" PRIu64 " cuadros=%" PRIu64 " bytes_al_panel=%" PRIu64 " registros_escritos=%" PRIu64 " sincronismos=%" PRIu64 " pixeles=%" PRIu64
                           " pixeles_perdidos=%" PRIu64 " | entradas +1FC: cuadro=0x%02X calibrar=0x%02X lectura=0x%02X | velocidad=0x%02X\n", p->n_escribe, p->n_lee, p->n_gc, p->n_bytes, p->n_reg, p->n_sync,
                           p->n_pixeles, p->n_pix_perdidos, fc & 0xff, (fc >> 8) & 0xff, (fc >> 16) & 0xff, (unsigned)(w580_latch_get(s, 0xf70011f8, 4) >> 8) & 0xff);
    g_string_append_printf(g, "faltas: microcodigo=%" PRIu64 " fuera_de_guion=%" PRIu64 " esperas_omitidas=%" PRIu64 " lanzamientos_sin_mirar_ocupado=%" PRIu64 " avisos=%" PRIu64 "%s%s\n", p->n_micro_mal, p->n_fuera,
                           p->n_esperas, p->n_sin_leer, p->n_avisos, p->ultimo_mal[0] ? " | el ultimo: " : "", p->ultimo_mal);
    return g_string_free(g, false);
}
static char *w580_get_panel_variante(Object *obj, Error **errp) { return g_strdup(w580_pr_variantes[w580_pr.variante].corto); }
static void w580_set_panel_variante(Object *obj, const char *value, Error **errp)
{
    for (unsigned i = 0; i < ARRAY_SIZE(w580_pr_variantes); i++) {
        if (!strcmp(value, w580_pr_variantes[i].corto)) { w580_pr.variante = i; W580_MACHINE(obj)->panel_variante_puesta = true; return; }
    }
    error_setg(errp, "panel-variante: hitachi, lpg, sharp o r61504");
}
static bool w580_get_panel_real(Object *obj, Error **errp) { return W580_MACHINE(obj)->panel_real; }
static void w580_set_panel_real(Object *obj, bool value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    s->panel_real = value; s->panel_real_puesto = true;
    if (s->lcd) { s->lcd->dirty = true; w580_pr.fb_viejo = true; }
}
static void w580_pr_class_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "panel-real", w580_get_panel_real, w580_set_panel_real);
    object_class_property_set_description(oc, "panel-real", "Pista E6: el PDI ejecuta su microcodigo y el panel es un R61505 con estado; solo se ve si se inicia bien. Por omision, lo que valga estricto");
    object_class_property_add_str(oc, "panel-variante", w580_get_panel_variante, w580_set_panel_variante);
    object_class_property_set_description(oc, "panel-variante", "Pista E6: que cristal es (lo que contesta R0028 tras CALB y que guion de fabrica le toca): hitachi (por omision), lpg, sharp, r61504");
    object_class_property_add_str(oc, "panel-estado", w580_get_panel_estado, NULL);
    object_class_property_set_description(oc, "panel-estado", "Pista E6: estado fisico del panel, guion en curso, contadores del PDI y faltas");
}
/* Al crear la máquina: panel-real sigue a estricto si nadie lo ha puesto; panel-r28 (E5) puesto sin variante elige la variante. */
static void w580_pr_init(W580State *s)
{
    if (!s->panel_real_puesto) { s->panel_real = w580_es.on; }
    if (s->panel_r28 != UINT32_MAX && !s->panel_variante_puesta) {
        unsigned id = s->panel_r28 & W580_PR_REG_MASC; w580_pr.variante = ARRAY_SIZE(w580_pr_variantes) - 1;
        for (unsigned i = 0; i < ARRAY_SIZE(w580_pr_variantes); i++) { if (w580_pr_variantes[i].id == (int)id) { w580_pr.variante = i; break; } }
    }
    w580_pr.g = -1;
    w580_pr_basura();
}
