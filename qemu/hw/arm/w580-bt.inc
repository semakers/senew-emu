/* Pista BT: la UART 3 del DB2020 (0xFB080000) con un STLC2500 FALSO al otro lado, y las reglas [uart3] y [bt] del estricto.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Para qué: que bt/port/bt_hw_w580.c (y, de paso, el HCITL del firmware ORIGINAL) hablen A NIVEL DE REGISTROS con un chip
 * que contesta: bt/prueba/stlc2500_falso.py --emu (bt/prueba_emu/LEEME.md). Sin la propiedad bt-uart NO cambia nada: la
 * UART 3 sigue como antes (sin nadie al otro lado). Las reglas del estricto de abajo valen con y sin el modelo.
 *
 * Qué se modela y de dónde sale (docs/bluetooth.md §2–§3, docs/hal.md §8; el original es la referencia):
 *   +00 L  dato recibido: saca un byte de la FIFO de RX                       FUN_4421fec0 lee +00 tantas veces como dice +2E [observado]
 *   +02 E  dato a transmitir: entra en la FIFO de TX (8 bits bajos)           FUN_442207fc; el original escribe medias palabras de su búfer
 *   +0A    bit 8: escala lenta (460 800 / div) o rápida (1 843 200 / div); bits 0..2 = 7: vacía las FIFO     FUN_4421ff98, FUN_4422070c
 *   +0C    formato: bits 0..4 = 1 es 8N1 (el único que usa el HCITL); bit 5 = break (el «deja dormir» del HCITL)
 *   +0E L  bit 4 = transmisor VACÍO (FIFO y registro de desplazamiento): ahora es de verdad, tarda lo que tarda la línea
 *   +12    máscara; +14 estado, lo que mira la ISR del original (0x44220B8E): bit 0 = la FIFO de RX llegó al umbral (+06),
 *          bit 6 = «tiempo de espera»: hay datos y la línea lleva 4 caracteres quieta (se reconoce con +14 <- 0x40), bit 1 = hay
 *          sitio para transmitir. [inferido] el umbral y los 4 caracteres (lo de un 16550); el modelo de la UART 0 encendía
 *          0x1041 con el primer byte y el HCITL del original, con eso, se desincronizaba (bt/prueba_emu/LEEME.md §4)
 *   +16    flujo (uarts_set_flow 0x44220444: 0x80 = RTS del teléfono automático, 0x40 = obedecer el CTS); el HCITL pone 0xC0
 *   +20    divisor;  +2C L sitio libre en la FIFO de TX;  +2E L bytes en la FIFO de RX
 *   el resto (+04 +06 +08 +18..+1E +22 +24 +26) se guarda en el cerrojo como antes: no cambian nada de lo que se modela.
 * El tiempo: cada byte cruza el cable en 10 bits / baudios de tiempo VIRTUAL, en los dos sentidos. Lo que el chip manda y no
 * cabe en la FIFO de RX se queda en el chip SI el teléfono tiene RTS automático (+16 bit 7) y, si no, se PIERDE (desbordamiento).
 * El CTS (+16 bit 6) no hace nada: el chip falso siempre está listo para recibir [inferido; el STLC2500 real tiene búferes].
 * El tamaño de las FIFO es bt-fifo (256 por omisión: +2C cabe en 9 bits y el HCITL pide bloques de 0x200 [inferido]).
 *
 * nRESET = SYSCON+04 bit 14 (0 = reset, Do_SYSCON_SetExternalReset 0x4423598C [observado]) y P2.0 (RX del teléfono = TX del
 * chip en reposo): la máquina NO decide cuándo vive el chip. Cada flanco de nRESET va al falso por el chardev de control
 * (bt-ctl) y es el FALSO quien contesta «VIVO 1» cuando ha salido del reset; solo entonces P2.0 lee 1 (como gpio-in).
 *
 * Protocolo del chardev de control (líneas de texto; de la máquina al falso):
 *   RESET 1 tx=N maquina      reset de la máquina (el cerrojo vuelve a 0): NO cuenta como pulso
 *   RESET 1 tx=N              nRESET a 0 (chip en reset). N = bytes que el teléfono ha puesto en el cable hasta ese momento
 *   RESET 0 tx=N ms=D         nRESET a 1 tras D ms a 0 (D = -1: nunca se vio a 0)
 *   LINEA baudios=B fmt=0xF flujo=0xL tx=N    la línea del teléfono cambia: los bytes N, N+1… ya salen así
 * (del falso a la máquina):  VIVO 0|1
 */

typedef struct W580Bt W580Bt;
#define W580_BT_BASE 0xfb080000u
#define W580_BT_NS_MIN 20000          /* el temporizador de la línea no corre más a menudo que cada 20 µs (mueve varios bytes de golpe) */

static uint32_t w580_bt_umbral(W580State *s)        /* +06: el umbral de la FIFO de RX [inferido]; 0 = la FIFO entera */
{
    uint32_t u = w580_latch_get(s, W580_BT_BASE + 0x06, 2) & 0x1ff;
    return u && u <= s->bt.fifo ? u : s->bt.fifo;
}
static bool w580_bt_puerta_abierta(W580State *s) { return !(w580_latch_get(s, 0xf909001a, 2) & 0x10); }   /* puerta 35 */
static uint32_t w580_bt_baudios(W580State *s)
{
    uint32_t div = w580_latch_get(s, W580_BT_BASE + 0x20, 2), ref = (w580_latch_get(s, W580_BT_BASE + 0x0a, 2) & 0x100) ? 460800 : 1843200;
    return div ? ref / div : 0;
}
static void w580_bt_ctl(W580State *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void w580_bt_ctl(W580State *s, const char *fmt, ...)
{
    if (!s->bt.hay_ctl) { return; }
    va_list ap; va_start(ap, fmt);
    g_autofree char *m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_autofree char *l = g_strdup_printf("%s\n", m);
    qemu_chr_fe_write_all(&s->bt.ctl, (const uint8_t *)l, strlen(l));
    if (s->bt.log < 400) { s->bt.log++; qemu_log_mask(LOG_UNIMP, "W580_BT ctl> %s t=%.6f\n", m, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9); }
}
static void w580_bt_p20(W580State *s, bool v)
{
    s->bt.vivo = v;
    s->gpio_in_mask[2] |= 1;
    s->gpio_in_val[2] = (s->gpio_in_val[2] & ~1) | (v ? 1 : 0);
    w580_irq_update(s);
}
/* La línea del teléfono: se le dice al falso cuando cambia (con la posición en el flujo de bytes en que cambia). */
static void w580_bt_linea(W580State *s)
{
    W580Bt *b = &s->bt;
    b->baud = w580_bt_baudios(s);
    b->fmt = w580_latch_get(s, W580_BT_BASE + 0x0c, 2);
    b->flujo = w580_latch_get(s, W580_BT_BASE + 0x16, 2);
    if (!b->modelo || (b->baud == b->baud_dicho && b->fmt == b->fmt_dicho && b->flujo == b->flujo_dicho)) { return; }
    b->baud_dicho = b->baud; b->fmt_dicho = b->fmt; b->flujo_dicho = b->flujo; b->n_lineas++;
    w580_bt_ctl(s, "LINEA baudios=%u fmt=0x%x flujo=0x%x tx=%" PRIu64, b->baud, b->fmt, b->flujo, b->n_tx);
}
static void w580_bt_arma(W580State *s)
{
    W580Bt *b = &s->bt;
    if (!b->modelo || timer_pending(b->t)) { return; }
    if (!b->tx_n && !b->et_n && !(b->rx_n && !b->to_dado)) { return; }
    if (!b->baud || !w580_bt_puerta_abierta(s)) { return; }      /* sin reloj ni divisor la línea no se mueve: se rearma al cambiarlos */
    int64_t ahora = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), byte = 10000000000LL / b->baud;
    b->t_ant = ahora; b->resto_ns = 0;
    timer_mod(b->t, ahora + MAX(byte, W580_BT_NS_MIN));
}
static void w580_bt_paso(void *opaque)
{
    W580State *s = opaque; W580Bt *b = &s->bt;
    int64_t ahora = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!b->baud || !w580_bt_puerta_abierta(s)) { return; }
    int64_t byte = 10000000000LL / b->baud, el = ahora - b->t_ant + b->resto_ns;
    uint64_t n = el / byte; b->resto_ns = el % byte; b->t_ant = ahora;
    bool algo = false;
    if (n > 8192) { n = 8192; }
    /* TX: del teléfono al chip */
    uint64_t k = MIN(n, b->tx_n);
    while (k) {
        uint32_t trozo = MIN(k, b->fifo - b->tx_i);
        qemu_chr_fe_write_all(&b->chr, b->tx + b->tx_i, trozo);
        b->tx_i = (b->tx_i + trozo) % b->fifo; b->tx_n -= trozo; b->n_tx += trozo; k -= trozo; algo = true;
    }
    /* RX: del chip al teléfono */
    for (uint64_t i = 0; i < n && b->et_n; i++) {
        if (b->rx_n >= b->fifo) {
            if (b->flujo & 0x80) { b->n_retenidos++; break; }      /* RTS automático: el chip se espera */
            b->n_perdidos++;                                       /* sin control de flujo: el byte se pierde */
            if (b->log < 400) { b->log++; qemu_log_mask(LOG_UNIMP, "W580_BT DESBORDAMIENTO: FIFO de RX llena sin RTS automatico (+16=0x%x); byte perdido\n", b->flujo); }
            w580_es_aviso(s, "uart3", 0xb7370000, "DESBORDAMIENTO de la FIFO de RX de la UART 3 (%u B) sin RTS automático (+16 = 0x%X): se pierden bytes del chip; el original pone +16 = 0xC0",
                          b->fifo, b->flujo);
        } else {
            b->rx[(b->rx_i + b->rx_n++) % b->fifo] = b->et[b->et_i]; b->n_rx++; b->t_rx_ult = ahora; b->to_dado = false;
        }
        b->et_i = (b->et_i + 1) % sizeof(b->et); b->et_n--; algo = true;
    }
    if (b->rx_n && !b->to_dado && ahora - b->t_rx_ult >= 4 * byte) {    /* 4 caracteres sin nada nuevo: «tiempo de espera» */
        b->to_dado = true; s->uart_pending[2] |= 0x40; algo = true;
    }
    if (algo) { qemu_chr_fe_accept_input(&b->chr); w580_irq_update(s); }
    bool lleno_y_retenido = b->rx_n >= b->fifo && (b->flujo & 0x80) && !b->tx_n;
    if (((b->tx_n || b->et_n) && !lleno_y_retenido) || (b->rx_n && !b->to_dado)) { timer_mod(b->t, ahora + MAX(byte, W580_BT_NS_MIN)); }
}
static int w580_bt_cabe(void *opaque) { W580State *s = opaque; return sizeof(s->bt.et) - s->bt.et_n; }
static void w580_bt_recibe(void *opaque, const uint8_t *buf, int size)
{
    W580State *s = opaque; W580Bt *b = &s->bt;
    for (int i = 0; i < size && b->et_n < sizeof(b->et); i++) { b->et[(b->et_i + b->et_n++) % sizeof(b->et)] = buf[i]; }
    w580_bt_arma(s);
}
static int w580_bt_ctl_cabe(void *opaque) { return 64; }
static void w580_bt_ctl_recibe(void *opaque, const uint8_t *buf, int size)
{
    W580State *s = opaque; W580Bt *b = &s->bt;
    for (int i = 0; i < size; i++) {
        if (buf[i] != '\n') { if (b->cbuf_n < sizeof(b->cbuf) - 1) { b->cbuf[b->cbuf_n++] = buf[i]; } continue; }
        b->cbuf[b->cbuf_n] = 0; b->cbuf_n = 0;
        if (!strncmp(b->cbuf, "VIVO ", 5)) {
            w580_bt_p20(s, b->cbuf[5] == '1');
            if (b->log < 400) { b->log++; qemu_log_mask(LOG_UNIMP, "W580_BT ctl< %s (P2.0 = %d) t=%.6f\n", b->cbuf, b->vivo, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9); }
        }
    }
}

/* ---------------------------------------------------------------- nRESET: SYSCON+04 bit 14 (se llama tras cada escritura) */
#define W580_BT_RESET_MS_ORIGINAL 100     /* el pulso del original: delay(100) entre SetExternalReset(2,1) y (2,0) [observado] */
static void w580_bt_syscon(W580State *s)
{
    W580Bt *b = &s->bt;
    int nivel = (w580_latch_get(s, 0xf9090004, 2) >> 14) & 1;
    if (nivel == b->reset_nivel) { return; }
    int64_t ahora = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int antes = b->reset_nivel;
    b->reset_nivel = nivel;
    if (!nivel) {                                   /* a 0: chip en reset; su TX deja de estar en reposo */
        b->reset_bajo_ns = ahora; b->n_resets++;
        if (b->modelo) { w580_bt_p20(s, false); b->et_n = 0; w580_bt_ctl(s, "RESET 1 tx=%" PRIu64, b->n_tx); }
        return;
    }
    int64_t ms = antes == 0 ? (ahora - b->reset_bajo_ns) / 1000000 : -1;
    if (antes == 0 && ms < W580_BT_RESET_MS_ORIGINAL - 10) {
        w580_es_aviso(s, "bt", 0xb7000004, "pulso de nRESET del Bluetooth (SYSCON+04 bit 14) de %" PRId64 " ms: el original lo deja %d ms a 0 (EILD_HCITL_Driver_Process, delay(100))",
                      ms, W580_BT_RESET_MS_ORIGINAL);
    }
    if (b->modelo) { w580_bt_ctl(s, "RESET 0 tx=%" PRIu64 " ms=%" PRId64, b->n_tx, ms); }
}

/* ---------------------------------------------------------------- reglas [uart3] del estricto (con y sin modelo)
 * Lo que el ORIGINAL escribe en cada registro de la UART 3: unión de las 87 tablas aprendidas de la calibración
 * (work/emu/trazas/e4_*, e5_* .aprendido.tsv; el original ejecuta uarts_open(3) y su HCITL en cada arranque), de lo que su código pone
 * y la calibración no llega a ejecutar (marcado «código»): el ack de +14 (ISR 0x44220B8E), la velocidad de 0xFCFC; y de lo que el
 * original escribió CON ESTE MODELO al pasar a 921 600 (bt/prueba_emu/LEEME.md §4: +06 = 0x6B, +08 = 0xB6). */
static const struct { uint8_t off, n; uint16_t v[6]; const char *de; } w580_bt_valores[] = {
    { 0x04, 1, { 7 },                              "uarts_hw_defaults" },
    { 0x06, 3, { 0x80, 0xd0, 0x6b },               "uarts_hw_defaults / FUN_4421ff98 (0x6B: el original a 921 600, visto con este modelo)" },
    { 0x08, 3, { 0x80, 0xf6, 0xb6 },               "uarts_hw_defaults / FUN_4421ff98 (0xB6: el original a 921 600, visto con este modelo)" },
    { 0x0a, 3, { 7, 0x100, 0 },                    "uarts_open, FUN_4422033c (escala rápida)" },
    { 0x0c, 3, { 0, 0x1801, 0x1821 },              "uarts_hw_defaults, uarts_set_format, el «deja dormir» del HCITL (bit 5)" },
    { 0x12, 5, { 0, 0x1380, 0x13c1, 0x13c3, 0x41 }, "uarts_int_mask_set/clear" },
    { 0x14, 2, { 0x40, 0x18 },                     "código: la ISR 0x44220B8E reconoce 0x40 y 0x18" },
    { 0x16, 2, { 0, 0xc0 },                        "uarts_hw_defaults; uarts_set_flow(h, 1, 1) del HCITL (FUN_4415d0d4) = 0xC0" },
    { 0x18, 1, { 1 },                              "uarts_hw_defaults" },
    { 0x1a, 1, { 0x38 },                           "uarts_hw_defaults" },
    { 0x1c, 1, { 0x11 },                           "uarts_hw_defaults (XON)" },
    { 0x1e, 1, { 0x13 },                           "uarts_hw_defaults (XOFF)" },
    { 0x20, 3, { 4, 0x10, 2 },                     "uarts_hw_defaults (4), uarts_set_format 115 200 (16); código: 921 600 tras 0xFCFC (2)" },
    { 0x22, 1, { 9 },                              "uarts_hw_defaults" },
    { 0x24, 2, { 0xa, 1 },                         "uarts_hw_defaults / FUN_4421ff98" },
    { 0x26, 1, { 13000 },                          "uarts_hw_defaults" },
};
static bool w580_bt_modelado(unsigned off)
{
    static const uint8_t m[] = { 0x00, 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x24, 0x26, 0x2c, 0x2e };
    for (unsigned i = 0; i < sizeof(m); i++) { if (m[i] == off) { return true; } }
    return false;
}
static void w580_bt_es(W580State *s, hwaddr addr, unsigned size, bool write, uint64_t value)
{
    unsigned off = addr - W580_BT_BASE;
    if (!w580_es.on) { return; }
    if (!w580_bt_modelado(off & ~1u)) {
        w580_es_aviso(s, "uart3", 0xb7300000 | off, "%s de la UART 3 +%02X: registro SIN MODELO (el emulador no sabe qué hace; el original no lo toca al usar el Bluetooth)",
                      write ? "escritura" : "lectura", off);
        return;
    }
    if (size != 2 || (off & 1)) {
        w580_es_aviso(s, "uart3", 0xb7310000 | off, "acceso de %u bytes a la UART 3 +%02X: el original solo usa medias palabras alineadas (ldrh/strh)", size, off);
    }
    if (!write || off == 0x02) { return; }
    for (unsigned i = 0; i < G_N_ELEMENTS(w580_bt_valores); i++) {
        if (w580_bt_valores[i].off != off) { continue; }
        bool ok = false;
        for (unsigned j = 0; j < w580_bt_valores[i].n; j++) { ok |= w580_bt_valores[i].v[j] == (value & 0xffff); }
        if (!ok) {
            GString *g = g_string_new(NULL);
            for (unsigned j = 0; j < w580_bt_valores[i].n; j++) { g_string_append_printf(g, "%s0x%X", j ? ", " : "", w580_bt_valores[i].v[j]); }
            w580_es_aviso(s, "uart3", 0xb7320000 | (off << 8) | (value & 0xff), "UART 3 +%02X <- 0x%X: el original nunca escribe ese valor (escribe %s: %s)",
                          off, (unsigned)(value & 0xffff), g->str, w580_bt_valores[i].de);
            g_string_free(g, true);
        }
        break;
    }
    if (off == 0x20) {
        uint32_t ref = (w580_latch_get(s, W580_BT_BASE + 0x0a, 2) & 0x100) ? 460800 : 1843200, bd = value & 0xffff ? ref / (value & 0xffff) : 0;
        if (bd != 115200 && bd != 921600) {
            w580_es_aviso(s, "uart3", 0xb7330000 | (value & 0xffff), "UART 3 a %u baudios (+20 = %u con +0A bit 8 = %d): el original solo pone 115 200 (y 921 600 tras 0xFCFC)",
                          bd, (unsigned)(value & 0xffff), ref == 460800);
        }
        if (s->bt.modelo && s->bt.tx_n) {
            w580_es_aviso(s, "uart3", 0xb7340000, "cambio de divisor de la UART 3 con %u bytes aún por salir: el original espera +0E bit 4 (uarts_set_format) y esos bytes salen a la velocidad nueva",
                          s->bt.tx_n);
        }
    }
}

/* ---------------------------------------------------------------- los registros, con el modelo puesto */
static bool w580_bt_lee(W580State *s, unsigned off, uint64_t *out)
{
    W580Bt *b = &s->bt;
    switch (off) {
    case 0x00:
        *out = 0;
        if (b->rx_n) {
            *out = b->rx[b->rx_i]; b->rx_i = (b->rx_i + 1) % b->fifo; b->rx_n--;
            if (!b->rx_n) { s->uart_pending[2] &= ~0x40; }                /* vacía: ya no hay «tiempo de espera» */
            w580_bt_arma(s); w580_irq_update(s);
        }
        else {
            b->n_vacia++;
            w580_es_aviso(s, "uart3", 0xb7350000, "lectura de +00 de la UART 3 con la FIFO de RX vacía (+2E = 0): el original lee solo los que dice +2E");
        }
        return true;
    case 0x0e: *out = b->tx_n ? 0 : 0x10; return true;
    case 0x2c: *out = b->fifo - b->tx_n; return true;
    case 0x2e: *out = b->rx_n; return true;
    }
    return false;
}
static bool w580_bt_escribe(W580State *s, unsigned off, uint64_t value)
{
    W580Bt *b = &s->bt;
    if (off == 0x02) {
        if (b->reset_nivel == 0) {
            b->n_tx_en_reset++;
            w580_es_aviso(s, "bt", 0xb7000002, "se transmite por la UART 3 con el chip en reset (SYSCON+04 bit 14 a 0): el original solo abre la UART tras soltarlo y ver P2.0 a 1");
        }
        if (b->tx_n >= b->fifo) {
            b->n_tx_lleno++;
            w580_es_aviso(s, "uart3", 0xb7360000, "escritura en +02 de la UART 3 con la FIFO de TX llena (+2C = 0): el byte se pierde; el original espera +2C != 0 (FUN_442207fc)");
            return true;
        }
        b->tx[(b->tx_i + b->tx_n++) % b->fifo] = value & 0xff;
        w580_bt_arma(s); w580_irq_update(s);
        return true;
    }
    return false;
}
/* tras cualquier escritura en la UART 3 (ya en el cerrojo): la línea, y vaciar las FIFO con +0A bits 0..2 = 7 */
static void w580_bt_tras_escribir(W580State *s, unsigned off, uint64_t value)
{
    W580Bt *b = &s->bt;
    if (off == 0x0a && (value & 7) == 7) { b->tx_n = b->rx_n = 0; s->uart_pending[2] &= ~0x40; }
    if (off == 0x0a || off == 0x0c || off == 0x16 || off == 0x20) { w580_bt_linea(s); w580_bt_arma(s); }
    if (off == 0x12) { w580_irq_update(s); }
}

/* ---------------------------------------------------------------- vida */
static void w580_bt_reset(W580State *s)
{
    W580Bt *b = &s->bt;
    b->reset_nivel = -1;
    if (!b->modelo) { return; }
    timer_del(b->t);
    b->tx_n = b->rx_n = b->tx_i = b->rx_i = 0;
    b->baud = b->baud_dicho = 0; b->fmt = b->fmt_dicho = b->flujo = b->flujo_dicho = 0;
    w580_bt_p20(s, false);
    /* reset de la máquina: el cerrojo de SYSCON vuelve a 0. Qué nivel tiene nRESET al encender es [sin saber]: el falso lo toma
     * como «sin alimentar/sin saber» y NO como un pulso; para despertar exige que el firmware lo baje y lo suba él (reset_nivel = -1) */
    w580_bt_ctl(s, "RESET 1 tx=%" PRIu64 " maquina", b->n_tx);
}
static void w580_bt_init(W580State *s)
{
    W580Bt *b = &s->bt;
    b->reset_nivel = -1;
    if (!b->fifo || b->fifo > sizeof(b->tx)) { b->fifo = 256; }
    if (!b->uart_id || !*b->uart_id) { return; }
    Chardev *chr = qemu_chr_find(b->uart_id);
    if (!chr) { error_report("bt-uart: no hay ningún -chardev con id=%s", b->uart_id); exit(1); }
    qemu_chr_fe_init(&b->chr, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&b->chr, w580_bt_cabe, w580_bt_recibe, NULL, NULL, s, NULL, true);
    if (b->ctl_id && *b->ctl_id) {
        Chardev *c = qemu_chr_find(b->ctl_id);
        if (!c) { error_report("bt-ctl: no hay ningún -chardev con id=%s", b->ctl_id); exit(1); }
        qemu_chr_fe_init(&b->ctl, c, &error_fatal);
        qemu_chr_fe_set_handlers(&b->ctl, w580_bt_ctl_cabe, w580_bt_ctl_recibe, NULL, NULL, s, NULL, true);
        b->hay_ctl = true;
    }
    b->modelo = true;
    b->t = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_bt_paso, s);
    w580_bt_p20(s, false);
}
static char *w580_bt_get_uart(Object *obj, Error **errp) { W580State *s = W580_MACHINE(obj); return g_strdup(s->bt.uart_id ? s->bt.uart_id : ""); }
static void w580_bt_set_uart(Object *obj, const char *v, Error **errp) { W580State *s = W580_MACHINE(obj); g_free(s->bt.uart_id); s->bt.uart_id = g_strdup(v); }
static char *w580_bt_get_ctl(Object *obj, Error **errp) { W580State *s = W580_MACHINE(obj); return g_strdup(s->bt.ctl_id ? s->bt.ctl_id : ""); }
static void w580_bt_set_ctl(Object *obj, const char *v, Error **errp) { W580State *s = W580_MACHINE(obj); g_free(s->bt.ctl_id); s->bt.ctl_id = g_strdup(v); }
static char *w580_bt_get_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj); W580Bt *b = &s->bt;
    if (!b->modelo) { return g_strdup("modelo BT apagado (sin bt-uart)\n"); }
    return g_strdup_printf("t=%.3f s | nRESET=%d P2.0=%d | linea %u baudios fmt=0x%x flujo=0x%x | puerta 35 %s | FIFO %u: tx=%u rx=%u en_el_chip=%u\n"
                           "cuentas: tx=%" PRIu64 " rx=%" PRIu64 " perdidos=%" PRIu64 " retenidos_por_RTS=%" PRIu64 " tx_llena=%" PRIu64 " tx_en_reset=%" PRIu64
                           " lecturas_vacias=%" PRIu64 " resets=%" PRIu64 " cambios_de_linea=%" PRIu64 "\n",
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, b->reset_nivel, b->vivo, b->baud, b->fmt, b->flujo,
                           w580_bt_puerta_abierta(s) ? "abierta" : "CERRADA", b->fifo, b->tx_n, b->rx_n, b->et_n,
                           b->n_tx, b->n_rx, b->n_perdidos, b->n_retenidos, b->n_tx_lleno, b->n_tx_en_reset, b->n_vacia, b->n_resets, b->n_lineas);
}
static void w580_bt_u32(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    visit_type_uint32(v, name, (uint32_t *)((char *)obj + (uintptr_t)opaque), errp);
}
static void w580_bt_class_init(ObjectClass *oc)
{
    object_class_property_add_str(oc, "bt-uart", w580_bt_get_uart, w580_bt_set_uart);
    object_class_property_set_description(oc, "bt-uart", "Pista BT: id de un -chardev al otro lado de la UART 3 (el STLC2500 falso, bt/prueba/stlc2500_falso.py --emu). Solo al lanzar");
    object_class_property_add_str(oc, "bt-ctl", w580_bt_get_ctl, w580_bt_set_ctl);
    object_class_property_set_description(oc, "bt-ctl", "Pista BT: id del -chardev de control (nRESET, linea, P2.0). Solo al lanzar");
    object_class_property_add(oc, "bt-fifo", "uint32", w580_bt_u32, w580_bt_u32, NULL, (void *)offsetof(W580State, bt.fifo));
    object_class_property_set_description(oc, "bt-fifo", "Pista BT: bytes de cada FIFO de la UART 3 (1..512; por omision 256) [inferido]");
    object_class_property_add_str(oc, "bt-estado", w580_bt_get_estado, NULL);
    object_class_property_set_description(oc, "bt-estado", "Pista BT: nRESET, P2.0, linea, FIFO y cuentas de la UART 3");
}
