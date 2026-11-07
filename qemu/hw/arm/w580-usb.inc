/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ---------------------------------------------------------------- USB de dispositivo (pista UEMU, 2026-10-14)
 * El controlador de función de 0xFC030000: un UDC de la familia TI OMAP1 (docs/usb.md, pista USB0: mapa de registros
 * sacado del original y cotejado con omap_udc.h de Linux). Registros de 16 bits; la ventana de datos +08 admite 8 y 16.
 * IRQ 62 «geni» (EP0 y DS_CHG) = banco 2 bit 2, 63 «non_iso» (EPn) = bit 3, 64 «iso» (SOF) = bit 4. El DMA (IRQ 58/59)
 * NO se modela: el original solo usa PIO en lo leído (docs/usb.md §6).
 *
 * Qué hace el modelo [inferido de omap_udc y de lo que hace el original; nada visto en el aparato]:
 *   DEVSTAT (+20): ATT = VBUS (propiedad usb-vbus, la misma del PMU 23 bit 7). Con el pull-up (SYSCON1 bit 0) y un
 *     anfitrión enchufado (usb-host), un reset del bus pone USB_RESET un rato (W580_USB_T_RESET) y luego DEF.
 *     SET_ADDRESS lo contesta el hardware (decodificación automática; el original nunca toca la dirección): ADD.
 *     SYSCON2 = DEV_CFG pone CFG, CLR_CFG la quita. Cada cambio de DEVSTAT levanta IRQ_SRC.DS_CHG.
 *   IRQ_SRC (+30): se borra escribiendo 1. SETUP se borra al dejar de leer el paquete (EP_NUM sin SETUP_SEL),
 *     salvo que haya llegado otro entretanto (lo que relee el original en FUN_44217a08).
 *   EPN_STAT (+34): el EP IN de número más bajo con EPN_TX pendiente (bits 3..0) y el OUT con EPN_RX (bits 11..8);
 *     borrar EPN_TX/EPN_RX en IRQ_SRC despacha ESE y, si quedan más, el bit vuelve a 1.
 *   FIFO: un búfer por EP y sentido (el reparto de la RAM de 2 KB solo se vigila en el estricto). IN: lo escrito en
 *     DATA con el EP seleccionado + CTRL.SET_FIFO_EN = paquete listo; el siguiente IN del anfitrión se lo lleva
 *     (ACK, IRQ EP0_TX / EPN_TX). OUT: SET_FIFO_EN = listo para recibir; el siguiente OUT entra (ACK, RXFSTAT,
 *     IRQ EP0_RX / EPN_RX) y el FIFO queda «lleno» hasta el próximo SET_FIFO_EN. Sin armar: NAK. Con HALT: STALL.
 *     SETUP siempre entra (y desarma el EP0 en los dos sentidos, como OMAP).
 *
 * El anfitrión: un chardev (propiedad usb-host=ID de -chardev) con mensajes de 8 bytes de cabecera
 *   { u8 tipo, u8 ep, u16 len (LE), u32 seq (LE) } + len bytes. Del anfitrión al modelo:
 *     0x01 RESET del bus          -> 0x81 cuando termina (o 0x87 si no hay pull-up/VBUS)
 *     0x02 SETUP (8 bytes)        -> 0x81 ACK
 *     0x03 OUT ep (datos)         -> 0x81 ACK | 0x83 NAK | 0x84 STALL
 *     0x04 IN ep (len = máx., SIN carga) -> 0x82 DATOS | 0x83 NAK | 0x84 STALL
 *     0x05 ESTADO                 -> 0x85 { u16 DEVSTAT, u16 SYSCON1, u8 dirección, u8 vbus }
 *     0x06 SUSPENDER / 0x07 REANUDAR -> 0x81
 *   0x87 = «no conectado» (sin VBUS o sin pull-up): el cable está pero el teléfono no se deja ver. 0x88 = petición mala.
 *   Lo hace correr emu/herr/uemu_anfitrion.py (anfitrión simulado: enumera y habla MTP) y emu/herr/uemu_usbip.py
 *   (servidor USB/IP: el teléfono emulado aparece en Linux con «usbip attach»; docs/emulador_usb.md).
 */
#define W580_USB_T_RESET  INT64_C(10000000)      /* 10 ms virtuales de USB_RESET (USB 2.0 §7.1.7.5: >= 10 ms) */
#define W580_USB_T_RECUP  INT64_C(10000000)      /* y 10 ms de «reset recovery» (§9.2.6.2) antes de contestar al anfitrión: sin ellos el
                                                    original, reconfigurado, pierde el primer SETUP tras un reset [observado en el emulador] */
enum { USB_EP0_TX = 1, USB_EP0_RX = 2, USB_SETUP = 4, USB_DS_CHG = 8, USB_EPN_TX = 0x10, USB_EPN_RX = 0x20, USB_SOF = 0x80 };
enum { USB_ATT = 1, USB_DEF = 2, USB_ADD = 4, USB_CFG = 8, USB_SUS = 0x10, USB_RST = 0x20 };
enum { USB_SF_FULL = 1, USB_SF_EMPTY = 2, USB_SF_FIFO_EN = 4, USB_SF_ACK = 8, USB_SF_NAK = 0x10, USB_SF_STALL = 0x20, USB_SF_HALTED = 0x40 };

static bool w580_usb_es(W580State *s) { return w580_es.on; }
static int64_t w580_usb_ahora(void) { return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); }
static W580UsbEp *w580_usb_ep(W580State *s, unsigned n, bool in) { return &s->usb.ep[n & 15][in ? 1 : 0]; }
static W580UsbEp *w580_usb_sel(W580State *s) { return w580_usb_ep(s, s->usb.ep_num & 15, s->usb.ep_num & 0x10); }
/* tamaño máximo según la configuración (+80 / +80+4n / +C0+4n): código 0..7 = 8 << código (7 = 1023, ISO) */
static unsigned w580_usb_cfg_tam(uint16_t c) { unsigned k = (c >> 12) & 7; return k == 7 ? 1023 : 8u << k; }
static uint16_t w580_usb_cfg(W580State *s, unsigned n, bool in) { return n == 0 ? s->usb.ep0 : in ? s->usb.ep_tx[n] : s->usb.ep_rx[n]; }
static bool w580_usb_valido(W580State *s, unsigned n, bool in) { return n == 0 || (w580_usb_cfg(s, n, in) & 0x8000); }
static unsigned w580_usb_maxp(W580State *s, unsigned n, bool in) { return w580_usb_cfg_tam(w580_usb_cfg(s, n, in)); }

/* Línea `bit` del banco 2 con las fuentes `m` (ya filtradas por IRQ_EN). Por nivel (ILR bit 1): lo pendiente es la línea.
 * Por flanco —[observado] el original configura la 62 con ILR = 0x78, flanco—: cada fuente NUEVA da un flanco, aunque la
 * línea ya estuviera arriba por otra [inferido: si no, con EP0_TX y EP0_RX seguidos el original se queda sin enterarse del
 * segundo; visto en el emulador con el primer modelo, que solo daba flanco al subir la línea]. */
static void w580_usb_linea(W580State *s, unsigned bit, uint16_t m, uint16_t *antes)
{
    if (s->irq_config[2][bit] & 2) {
        if (m) { s->irq_level[2] |= 1u << bit; } else { s->irq_level[2] &= ~(1u << bit); }
    } else {
        if (m & ~*antes) { s->irq_latch[2] |= 1u << bit; }
        if (m) { s->irq_level[2] |= 1u << bit; } else { s->irq_level[2] &= ~(1u << bit); }
    }
    *antes = m;
}
static void w580_usb_lineas(W580State *s)
{
    W580Usb *u = &s->usb; uint16_t q = u->irq_src, e = u->irq_en;
    uint16_t geni = (q & 7 & ((e & 1) ? 7 : 0)) | (q & e & USB_DS_CHG);
    w580_usb_linea(s, 2, geni, &u->ant[0]);                                  /* IRQ 62 geni: EP0 y DS_CHG */
    w580_usb_linea(s, 3, q & e & (USB_EPN_TX | USB_EPN_RX), &u->ant[1]);     /* IRQ 63 non_iso: EPn */
    w580_usb_linea(s, 4, q & e & USB_SOF, &u->ant[2]);                       /* IRQ 64 iso: SOF */
}
static uint16_t w580_usb_devstat(W580State *s) { return s->usb.devstat | (s->bat.usb ? USB_ATT : 0); }
static void w580_usb_ds(W580State *s, uint16_t nuevo)
{
    W580Usb *u = &s->usb;
    uint16_t antes = w580_usb_devstat(s);
    u->devstat = nuevo & 0x7e;
    if ((w580_usb_devstat(s) != antes || (nuevo & 0x8000)) && (u->irq_en & USB_DS_CHG)) { u->irq_src |= USB_DS_CHG; u->n_ds++; }   /* solo con DS_CHG_IE (docs/usb.md §9.2) */
    w580_irq_update(s);
}
static uint16_t w580_usb_epn_stat(W580State *s)
{
    W580Usb *u = &s->usb; uint16_t r = 0;
    if (u->pend_tx) { r |= ctz32(u->pend_tx); }
    if (u->pend_rx) { r |= ctz32(u->pend_rx) << 8; }
    return r;
}
static void w580_usb_recalc_epn(W580State *s)
{
    W580Usb *u = &s->usb;
    if (u->pend_tx) { u->irq_src |= USB_EPN_TX; } else { u->irq_src &= ~USB_EPN_TX; }
    if (u->pend_rx) { u->irq_src |= USB_EPN_RX; } else { u->irq_src &= ~USB_EPN_RX; }
}
/* lo que se ve del bus: conectado = VBUS y pull-up */
static bool w580_usb_visible(W580State *s) { return s->bat.usb && (s->usb.syscon1 & 1); }
static void w580_usb_ep_vaciar(W580UsbEp *e) { e->n = e->i = 0; e->armado = false; }
static void w580_usb_bus_suelto(W580State *s)
{
    W580Usb *u = &s->usb;
    for (unsigned n = 0; n < 16; n++) { w580_usb_ep_vaciar(&u->ep[n][0]); w580_usb_ep_vaciar(&u->ep[n][1]); }
    u->direccion = 0; u->dir_pend = -1; u->stall0 = false;
}

/* ---------------------------------------------------------------- el estricto, regla [usb] */
static void w580_usb_aviso_data(W580State *s, bool write)
{
    uint16_t d = w580_usb_devstat(s);
    if (d & USB_RST) {
        w580_es_aviso(s, "usb", 1 | write << 8, "%s de DATA (+08) con DEVSTAT.USB_RESET = 1: el original no toca el FIFO durante el reset del bus (FUN_44219398/44219660)", write ? "escritura" : "lectura");
    } else if (write && !(d & USB_ATT)) {
        w580_es_aviso(s, "usb", 2, "escritura de DATA (+08) sin VBUS (DEVSTAT.ATT = 0): el original lo prohíbe (FUN_44219660)");
    }
}
/* reparto de la RAM del FIFO: 2 KB, punteros en unidades de 8 bytes; el original para con pánico si puntero + tamaño > 0x7F8 */
static void w580_usb_es_reparto(W580State *s, unsigned n, bool in, uint16_t v)
{
    if (!w580_usb_es(s) || (n && !(v & 0x8000))) { return; }
    unsigned a = (v & 0x7ff) * 8, t = w580_usb_cfg_tam(v);
    if (a + t > 0x800) {
        w580_es_aviso(s, "usb", 0x30 | n << 4 | in, "EP%u %s: FIFO en 0x%03x + %u bytes se sale de la RAM de 2 KB (el original exige puntero + tamaño <= 0x7F8, FUN_44218430)", n, in ? "IN" : "OUT", a, t);
        return;
    }
    for (unsigned m = 0; m < 16; m++) {
        for (unsigned d = 0; d < 2; d++) {
            if (m == 0 && d == 1) { continue; }                       /* el EP0 tiene una sola configuración (+80) para los dos sentidos */
            if (m == n && (n == 0 || d == in)) { continue; }          /* él mismo */
            uint16_t c = w580_usb_cfg(s, m, d);
            if (m && !(c & 0x8000)) { continue; }
            unsigned b = (c & 0x7ff) * 8, tb = w580_usb_cfg_tam(c);
            if (a < b + tb && b < a + t) {
                w580_es_aviso(s, "usb", 0x400 | n << 4 | in, "EP%u %s: su FIFO [0x%03x, 0x%03x) pisa el de EP%u%s [0x%03x, 0x%03x): el original los reparte sin solaparse (FUN_44218430)",
                              n, n ? (in ? "IN" : "OUT") : "", a, a + t, m, m ? (d ? " IN" : " OUT") : "", b, b + tb);
                return;
            }
        }
    }
}

/* La puerta 26 (SYSCON +18 bit 1) y el reset 20 (+12 bit 1) NO son del USB: [observado en el emulador, 2026-10-14 07:52] el
 * original, al enchufar VBUS, lee IRQ_SRC y conecta el pull-up con F9090018 bit 1 a 1 Y F9090012 bit 1 a 0 (docs/usb.md §11 ya
 * lo dudaba). Por eso no hay regla de puerta para el USB: solo se apunta como suceso del estricto la primera vez. */
static void w580_usb_es_puerta(W580State *s, hwaddr addr, bool write)
{
    if (!w580_es_activo() || !w580_es.syscon_visto || s->usb.puerta_dicha) { return; }
    bool parado = w580_latch_get(s, 0xf9090018, 2) & 2, en_reset = !(w580_latch_get(s, 0xf9090012, 2) & 2);
    if (parado || en_reset) {
        s->usb.puerta_dicha = true;
        w580_es_suceso("usb", "%s de 0x%08x con F9090018 bit 1 = %d y F9090012 bit 1 = %d (la puerta 26 / bloque 20 de docs/hal.md no son del USB) pc=0x%08x",
                       write ? "escritura" : "lectura", (unsigned)addr, parado, !en_reset, w580_es_pc(s));
        qemu_log_mask(LOG_UNIMP, "W580_USB acceso con la puerta 26 cerrada=%d / bloque 20 en reset=%d: no son del USB\n", parado, en_reset);
    }
}

/* ---------------------------------------------------------------- registros */
static uint64_t w580_usb_lee(W580State *s, hwaddr addr, unsigned size)
{
    W580Usb *u = &s->usb; unsigned r = addr & 0xffff;
    uint64_t v = 0;
    w580_usb_es_puerta(s, addr, false);
    switch (r) {
    case 0x04: v = u->ep_num; break;
    case 0x08: case 0x09: {
        w580_usb_aviso_data(s, false);
        for (unsigned k = 0; k < size && k < 2; k++) {
            uint8_t b = 0;
            if (u->ep_num & 0x40) { b = u->setup_i < 8 ? u->setup[u->setup_i++] : 0; }
            else {
                W580UsbEp *e = w580_usb_ep(s, u->ep_num & 15, false);
                if (u->ep_num & 0x10) { w580_es_aviso(s, "usb", 3, "lectura de DATA con un EP IN seleccionado (EP_NUM = 0x%02x): del FIFO de un IN no se lee", u->ep_num); }
                else if (e->i < e->n) { b = e->buf[e->i++]; }
                else { w580_es_aviso(s, "usb", 4 | (u->ep_num & 15) << 4, "lectura de DATA del EP%u OUT más allá de lo recibido (%u bytes, RXFSTAT): el original lee exactamente RXFSTAT", u->ep_num & 15, e->n); }
            }
            v |= (uint64_t)b << (8 * k);
        }
        break;
    }
    case 0x10: {
        W580UsbEp *e = w580_usb_sel(s);
        if (!(u->ep_num & 0x20)) { w580_es_aviso(s, "usb", 5, "lectura de STAT_FLG (+10) sin EP_SEL (EP_NUM = 0x%02x): el original solo lo lee con el EP seleccionado (FUN_44219398)", u->ep_num); }
        v = (e->armado ? USB_SF_FIFO_EN : 0) | (e->ack ? USB_SF_ACK : 0) | (e->stall ? USB_SF_STALL : 0) | (e->halt ? USB_SF_HALTED : 0) |
            (e->n == e->i ? USB_SF_EMPTY : 0) | (e->n > e->i && e->n >= w580_usb_maxp(s, u->ep_num & 15, u->ep_num & 0x10) ? USB_SF_FULL : 0) |
            (!(u->ep_num & 0x10) && e->n == 0 ? 0x8000 : 0);
        break;
    }
    case 0x14: {
        W580UsbEp *e = w580_usb_ep(s, u->ep_num & 15, false);
        if (!(u->ep_num & 0x20)) { w580_es_aviso(s, "usb", 6, "lectura de RXFSTAT (+14) sin EP_SEL (EP_NUM = 0x%02x): el original solo lo lee con el EP seleccionado (FUN_44219398)", u->ep_num); }
        v = (e->n - e->i) & 0x3ff;
        break;
    }
    case 0x18: v = u->syscon1; break;
    case 0x1c: v = 0; break;
    case 0x20: v = w580_usb_devstat(s); break;
    case 0x24: v = (w580_usb_ahora() / 1000000) & 0x7ff; break;
    case 0x28: v = u->irq_en; break;
    case 0x2c: v = u->dma_irq_en; break;
    case 0x30: v = u->irq_src; break;
    case 0x34: v = w580_usb_epn_stat(s); break;
    case 0x80: v = u->ep0; break;
    case 0x108: v = u->r108; break;
    default:
        if (r > 0x80 && r < 0xc0 && !(r & 3)) { v = u->ep_rx[(r - 0x80) / 4]; }
        else if (r > 0xc0 && r < 0x100 && !(r & 3)) { v = u->ep_tx[(r - 0xc0) / 4]; }
        else { v = w580_latch_get(s, addr, size); }
    }
    if (u->log < 20000) { u->log++; qemu_log_mask(LOG_UNIMP, "W580_USB R +%02x = 0x%04x pc=0x%08x\n", r, (unsigned)v, s->cpu ? s->cpu->env.regs[15] : 0); }
    return v;
}
static void w580_usb_ctrl(W580State *s, uint16_t v)
{
    W580Usb *u = &s->usb; unsigned n = u->ep_num & 15; bool in = u->ep_num & 0x10;
    W580UsbEp *e = w580_usb_ep(s, n, in);
    if ((v & 1) && n == 0) { w580_es_aviso(s, "usb", 7, "CTRL = RESET_EP con el EP0 (EP_NUM = 0x%02x): el original lo prohíbe (FUN_44219660)", u->ep_num); }
    if ((v & 4) && n && !w580_usb_valido(s, n, in)) {
        w580_es_aviso(s, "usb", 8 | n << 4 | in << 8, "SET_FIFO_EN del EP%u %s, que no está configurado (EP_%s(%u) sin VALID)", n, in ? "IN" : "OUT", in ? "TX" : "RX", n);
    }
    if (v & 1) { w580_usb_ep_vaciar(e); e->ack = e->stall = false; }                         /* RESET_EP */
    if (v & 2) { e->n = e->i = 0; e->armado = false; }                                       /* CLR_EP */
    if (v & 4) {                                                                             /* SET_FIFO_EN */
        if (in && e->armado) {
            w580_es_aviso(s, "usb", 9 | n << 4, "SET_FIFO_EN del EP%u IN con el paquete anterior aún sin recoger (STAT_FLG.FIFO_EN = 1): ese paquete se pierde", n);
        }
        if (!in) { e->n = e->i = 0; }
        e->armado = true; e->ack = false;
        u->n_armados++;
    }
    if (v & 0x40) { e->halt = true; }                                                        /* SET_HALT */
    if (v & 0x80) { e->halt = false; e->stall = false; if (n == 0) { u->stall0 = false; } }  /* CLR_HALT */
}
static void w580_usb_escribe(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    W580Usb *u = &s->usb; unsigned r = addr & 0xffff; uint16_t v = value;
    w580_usb_es_puerta(s, addr, true);
    if (u->log < 20000) { u->log++; qemu_log_mask(LOG_UNIMP, "W580_USB W +%02x = 0x%04x pc=0x%08x\n", r, v, s->cpu ? s->cpu->env.regs[15] : 0); }
    switch (r) {
    case 0x04:
        if ((u->ep_num & 0x40) && !(v & 0x40)) {           /* se acabó de leer el SETUP: se borra salvo que haya otro */
            if (!u->setup_otro) { u->irq_src &= ~USB_SETUP; }
            u->setup_otro = false;
        }
        if (v & 0x40) { u->setup_i = 0; }
        u->ep_num = v & 0x7f;
        break;
    case 0x08: case 0x09: {
        w580_usb_aviso_data(s, true);
        unsigned n = u->ep_num & 15; W580UsbEp *e = w580_usb_ep(s, n, true);
        if (!(u->ep_num & 0x10) || (u->ep_num & 0x40)) {
            w580_es_aviso(s, "usb", 10, "escritura de DATA sin un EP IN seleccionado (EP_NUM = 0x%02x)", u->ep_num);
            break;
        }
        for (unsigned k = 0; k < size && k < 2; k++) {
            unsigned maxp = w580_usb_maxp(s, n, true);
            if (e->n >= maxp) {
                w580_es_aviso(s, "usb", 11 | n << 4, "EP%u IN: se escriben más de %u bytes (wMaxPacketSize del EP) en el FIFO: se desborda sobre el EP de al lado", n, maxp);
            }
            if (e->n < W580_USB_FIFO_TAM) { e->buf[e->n++] = value >> (8 * k); }
        }
        break;
    }
    case 0x0c: w580_usb_ctrl(s, v); break;
    case 0x18: {
        uint16_t antes = u->syscon1;
        if ((v & 1) && !(antes & 1) && !s->bat.usb) {
            w580_es_aviso(s, "usb", 12, "SYSCON1.PULLUP_EN = 1 sin VBUS (DEVSTAT.ATT = 0): el original solo conecta el pull-up con ATT = 1 (FUN_44217f34)");
        }
        u->syscon1 = v & 0x1ff;
        if ((antes ^ v) & 1) {
            u->n_pullup++;
            qemu_log_mask(LOG_UNIMP, "W580_USB pull-up %s t=%.3f s\n", v & 1 ? "DENTRO (el PC nos ve)" : "fuera", w580_usb_ahora() / 1e9);
            if (!(v & 1)) { w580_usb_bus_suelto(s); w580_usb_ds(s, w580_usb_devstat(s) & ~(USB_DEF | USB_ADD | USB_CFG | USB_RST | USB_SUS)); }
        }
        break;
    }
    case 0x1c:
        if (v & 0x20) { u->stall0 = true; }                                   /* STALL_CMD: STALL del EP0 hasta el próximo SETUP */
        if (v & 0x08) { w580_usb_ds(s, w580_usb_devstat(s) | USB_CFG); u->n_cfg++; }        /* DEV_CFG */
        if (v & 0x04) { w580_usb_ds(s, w580_usb_devstat(s) & ~USB_CFG); }                   /* CLR_CFG */
        break;
    case 0x28: u->irq_en = v; w580_irq_update(s); break;
    case 0x2c: u->dma_irq_en = v; break;
    case 0x30:
        if (v & USB_EPN_TX && u->pend_tx) { u->pend_tx &= u->pend_tx - 1; }    /* despacha el EP que enseña EPN_STAT */
        if (v & USB_EPN_RX && u->pend_rx) { u->pend_rx &= u->pend_rx - 1; }
        u->irq_src &= ~(v & ~(USB_EPN_TX | USB_EPN_RX));
        w580_usb_recalc_epn(s);
        w580_irq_update(s);
        break;
    case 0x80: case 0x108: default:
        if (r == 0x108) { u->r108 = v; break; }
        if (r >= 0x80 && r < 0x100 && !(r & 3)) {
            unsigned n = ((r - 0x80) / 4) & 15; bool in = r >= 0xc0;
            if (u->syscon1 & 0x100) {
                w580_es_aviso(s, "usb", 13, "escritura de la configuración de EP (+%02x = 0x%04x) con SYSCON1.CFG_LOCK = 1: el original abre el cerrojo antes (FUN_4421822c)", r, v);
            }
            if (u->syscon1 & 0x100) { break; }                     /* con CFG_LOCK la escritura no entra [inferido de OMAP: docs/usb.md §9.5] */
            if (r == 0x80) { u->ep0 = v; } else if (in) { u->ep_tx[n] = v; } else { u->ep_rx[n] = v; }
            w580_usb_es_reparto(s, r == 0x80 ? 0 : n, r == 0x80 ? false : in, v);
            break;
        }
        w580_latch_set(s, addr, size, value);
    }
}

/* ---------------------------------------------------------------- el anfitrión (chardev) */
static void w580_usb_manda(W580State *s, uint8_t tipo, uint8_t ep, uint32_t seq, const uint8_t *p, uint16_t n)
{
    W580Usb *u = &s->usb;
    if (!u->hay_host) { return; }
    uint8_t h[8] = { tipo, ep, n & 255, n >> 8, seq, seq >> 8, seq >> 16, seq >> 24 };
    qemu_chr_fe_write_all(&u->chr, h, 8);
    if (n) { qemu_chr_fe_write_all(&u->chr, p, n); }
}
static void w580_usb_fin_reset(void *opaque)
{
    W580State *s = opaque; W580Usb *u = &s->usb;
    if (u->reset_fase == 1) {
        if (!(w580_usb_devstat(s) & USB_RST)) { u->reset_fase = 0; return; }
        w580_usb_ds(s, (w580_usb_devstat(s) & ~USB_RST) | USB_DEF);
        u->reset_fase = 2; timer_mod(u->t_reset, w580_usb_ahora() + W580_USB_T_RECUP);
        return;
    }
    if (u->reset_fase == 2) { u->reset_fase = 0; w580_usb_manda(s, 0x81, 0, u->reset_seq, NULL, 0); }
}
static void w580_usb_mensaje(W580State *s, uint8_t tipo, uint8_t ep, uint32_t seq, const uint8_t *p, uint16_t n)
{
    W580Usb *u = &s->usb; unsigned num = ep & 15;
    u->n_msg++;
    if (tipo == 5) {
        uint16_t d = w580_usb_devstat(s);
        uint8_t r[6] = { d, d >> 8, u->syscon1, u->syscon1 >> 8, u->direccion, s->bat.usb };
        w580_usb_manda(s, 0x85, 0, seq, r, 6); return;
    }
    if (!w580_usb_visible(s)) { w580_usb_manda(s, 0x87, ep, seq, NULL, 0); return; }
    switch (tipo) {
    case 1:                                                        /* reset del bus */
        w580_usb_bus_suelto(s); u->n_resets++;
        u->reset_seq = seq;
        w580_usb_ds(s, (w580_usb_devstat(s) & ~(USB_DEF | USB_ADD | USB_CFG | USB_SUS)) | USB_RST | 0x8000);
        u->reset_fase = 1; timer_mod(u->t_reset, w580_usb_ahora() + W580_USB_T_RESET);
        return;
    case 6: case 7:
        w580_usb_ds(s, tipo == 6 ? (w580_usb_devstat(s) | USB_SUS) : (w580_usb_devstat(s) & ~USB_SUS));
        w580_usb_manda(s, 0x81, 0, seq, NULL, 0); return;
    }
    if (w580_usb_devstat(s) & USB_RST) { w580_usb_manda(s, 0x83, ep, seq, NULL, 0); return; }
    if (tipo == 2) {                                               /* SETUP: siempre entra */
        if (n != 8) { w580_usb_manda(s, 0x88, ep, seq, NULL, 0); return; }
        u->n_setup++;
        w580_usb_ep_vaciar(w580_usb_ep(s, 0, false)); w580_usb_ep_vaciar(w580_usb_ep(s, 0, true));
        u->stall0 = false;
        if (p[0] == 0x00 && p[1] == 5) {                           /* SET_ADDRESS: lo contesta el hardware (decodificación automática) */
            u->dir_pend = p[2] & 0x7f;
            w580_usb_manda(s, 0x81, 0, seq, NULL, 0); return;
        }
        u->dir_pend = -1;
        if (u->ep_num & 0x40) { u->setup_otro = true; }        /* llegó otro mientras se leía el anterior */
        memcpy(u->setup, p, 8); u->setup_i = 0;
        u->irq_src |= USB_SETUP;
        w580_irq_update(s);
        w580_usb_manda(s, 0x81, 0, seq, NULL, 0); return;
    }
    bool in = tipo == 4;
    if ((tipo != 3 && tipo != 4) || (num && !w580_usb_valido(s, num, in)) || (num && !(w580_usb_devstat(s) & USB_CFG))) {
        w580_usb_manda(s, 0x88, ep, seq, NULL, 0); return;
    }
    W580UsbEp *e = w580_usb_ep(s, num, in);
    if (num == 0 && in && u->dir_pend >= 0) {                     /* fase de estado de SET_ADDRESS: la contesta el hardware */
        u->direccion = u->dir_pend; u->dir_pend = -1;
        w580_usb_manda(s, 0x82, ep, seq, NULL, 0);
        w580_usb_ds(s, w580_usb_devstat(s) | USB_ADD);
        return;
    }
    if (e->halt || (num == 0 && u->stall0)) { e->stall = true; w580_usb_manda(s, 0x84, ep, seq, NULL, 0); return; }
    if (!e->armado) { w580_usb_manda(s, 0x83, ep, seq, NULL, 0); return; }
    if (in) {
        unsigned k = MIN(e->n, (unsigned)n ? n : e->n);
        w580_usb_manda(s, 0x82, ep, seq, e->buf, k);
        e->n = e->i = 0; e->armado = false; e->ack = true;
        u->bytes_in += k;
        if (num == 0) { u->irq_src |= USB_EP0_TX; } else { u->pend_tx |= 1u << num; }
    } else {
        unsigned maxp = w580_usb_maxp(s, num, false);
        if (n > maxp) { w580_usb_manda(s, 0x88, ep, seq, NULL, 0); return; }     /* el anfitrión manda más que wMaxPacketSize */
        memcpy(e->buf, p, n); e->n = n; e->i = 0; e->armado = false; e->ack = true;
        u->bytes_out += n;
        if (num == 0) { u->irq_src |= USB_EP0_RX; } else { u->pend_rx |= 1u << num; }
        w580_usb_manda(s, 0x81, ep, seq, NULL, 0);
    }
    w580_usb_recalc_epn(s);
    w580_irq_update(s);
}
static int w580_usb_cabe(void *opaque) { W580State *s = opaque; return sizeof(s->usb.rx) - s->usb.rx_n; }
static void w580_usb_recibe(void *opaque, const uint8_t *buf, int size)
{
    W580State *s = opaque; W580Usb *u = &s->usb;
    size = MIN(size, (int)(sizeof(u->rx) - u->rx_n));
    memcpy(u->rx + u->rx_n, buf, size); u->rx_n += size;
    for (;;) {
        if (u->rx_n < 8) { break; }
        uint16_t n = u->rx[2] | u->rx[3] << 8;
        uint16_t pl = u->rx[0] == 4 ? 0 : n;                       /* en IN, len es lo máximo que se quiere y no hay carga */
        if (pl > sizeof(u->rx) - 8) { u->rx_n = 0; break; }       /* basura: se tira todo */
        if (u->rx_n < 8u + pl) { break; }
        uint32_t seq = u->rx[4] | u->rx[5] << 8 | u->rx[6] << 16 | (uint32_t)u->rx[7] << 24;
        w580_usb_mensaje(s, u->rx[0], u->rx[1], seq, u->rx + 8, n);
        memmove(u->rx, u->rx + 8 + pl, u->rx_n - 8 - pl); u->rx_n -= 8 + pl;
    }
}
static void w580_usb_evento(void *opaque, QEMUChrEvent ev)
{
    W580State *s = opaque;
    if (ev == CHR_EVENT_CLOSED) { s->usb.rx_n = 0; qemu_log_mask(LOG_UNIMP, "W580_USB el anfitrión se fue\n"); }
    if (ev == CHR_EVENT_OPENED) { s->usb.rx_n = 0; qemu_log_mask(LOG_UNIMP, "W580_USB anfitrión conectado\n"); }
}

/* ---------------------------------------------------------------- VBUS, reset, init, propiedades */
/* lo llama w580-bateria.inc al cambiar usb-vbus */
static void w580_usb_vbus(W580State *s, bool antes)
{
    if (!s->usb.modelo) { return; }
    if (!s->bat.usb) { w580_usb_bus_suelto(s); s->usb.devstat &= ~(USB_DEF | USB_ADD | USB_CFG | USB_RST | USB_SUS); }
    if (antes != s->bat.usb && (s->usb.irq_en & USB_DS_CHG)) { s->usb.irq_src |= USB_DS_CHG; s->usb.n_ds++; }
    w580_irq_update(s);
}
static void w580_usb_reset(W580State *s)
{
    W580Usb *u = &s->usb;
    if (!u->modelo) { return; }
    u->ep_num = u->syscon1 = u->irq_en = u->irq_src = u->dma_irq_en = u->devstat = u->ep0 = u->r108 = 0;
    memset(u->ep_rx, 0, sizeof(u->ep_rx)); memset(u->ep_tx, 0, sizeof(u->ep_tx));
    u->pend_tx = u->pend_rx = 0; u->setup_i = 0; u->setup_otro = false; memset(u->ant, 0, sizeof(u->ant)); u->reset_fase = 0;
    w580_usb_bus_suelto(s);
    if (u->t_reset) { timer_del(u->t_reset); }
}
static void w580_usb_init(W580State *s)
{
    W580Usb *u = &s->usb;
    if (u->host && *u->host) {
        Chardev *chr = qemu_chr_find(u->host);
        if (!chr) { error_report("usb-host: no hay ningún -chardev con id=%s", u->host); exit(1); }
        qemu_chr_fe_init(&u->chr, chr, &error_fatal);
        qemu_chr_fe_set_handlers(&u->chr, w580_usb_cabe, w580_usb_recibe, w580_usb_evento, NULL, s, NULL, true);
        u->hay_host = true; u->modelo = true;
    }
    if (!u->modelo) { return; }
    u->t_reset = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_usb_fin_reset, s);
    w580_usb_reset(s);
}
static bool w580_usb_get_modelo(Object *obj, Error **errp) { return W580_MACHINE(obj)->usb.modelo; }
static void w580_usb_set_modelo(Object *obj, bool v, Error **errp) { W580_MACHINE(obj)->usb.modelo = v; }
static char *w580_usb_get_host(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->usb.host ? W580_MACHINE(obj)->usb.host : ""); }
static void w580_usb_set_host(Object *obj, const char *v, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_free(s->usb.host); s->usb.host = g_strdup(v);
    if (v && *v) { s->usb.modelo = true; }
}
static char *w580_usb_get_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj); W580Usb *u = &s->usb;
    if (!u->modelo) { return g_strdup("modelo USB apagado (udc=off y sin usb-host)\n"); }
    uint16_t d = w580_usb_devstat(s);
    GString *g = g_string_new(NULL);
    g_string_append_printf(g, "t=%.3f s | VBUS=%s anfitrion=%s | DEVSTAT=%04x%s%s%s%s%s%s | SYSCON1=%04x (pull-up %s) | IRQ_EN=%02x IRQ_SRC=%02x EPN_STAT=%04x | direccion=%u\n",
                           w580_usb_ahora() / 1e9, s->bat.usb ? "on" : "off", u->hay_host ? u->host : "(ninguno)", d,
                           d & USB_ATT ? " ATT" : "", d & USB_DEF ? " DEF" : "", d & USB_ADD ? " ADD" : "", d & USB_CFG ? " CFG" : "",
                           d & USB_SUS ? " SUS" : "", d & USB_RST ? " RESET" : "", u->syscon1, u->syscon1 & 1 ? "dentro" : "fuera",
                           u->irq_en, u->irq_src, w580_usb_epn_stat(s), u->direccion);
    g_string_append_printf(g, "EP0=%04x", u->ep0);
    for (unsigned n = 1; n < 16; n++) {
        if (u->ep_rx[n] & 0x8000) { g_string_append_printf(g, " OUT%u=%04x", n, u->ep_rx[n]); }
        if (u->ep_tx[n] & 0x8000) { g_string_append_printf(g, " IN%u=%04x", n, u->ep_tx[n]); }
    }
    g_string_append_printf(g, "\nmensajes=%" PRIu64 " resets=%" PRIu64 " setup=%" PRIu64 " armados=%" PRIu64 " bytes_in=%" PRIu64 " bytes_out=%" PRIu64 " ds_chg=%" PRIu64 " pull-up=%" PRIu64 " dev_cfg=%" PRIu64 "\n",
                           u->n_msg, u->n_resets, u->n_setup, u->n_armados, u->bytes_in, u->bytes_out, u->n_ds, u->n_pullup, u->n_cfg);
    return g_string_free(g, false);
}
static void w580_usb_class_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "udc", w580_usb_get_modelo, w580_usb_set_modelo);
    object_class_property_set_description(oc, "udc", "Pista UEMU: on = modelo del controlador USB de dispositivo (FC030000, IRQ 62/63/64; docs/emulador_usb.md). Solo al lanzar");
    object_class_property_add_str(oc, "usb-host", w580_usb_get_host, w580_usb_set_host);
    object_class_property_set_description(oc, "usb-host", "Pista UEMU: id de un -chardev por el que habla el anfitrion USB (emu/herr/uemu_anfitrion.py o uemu_usbip.py); implica udc=on. Solo al lanzar");
    object_class_property_add_str(oc, "usb-estado", w580_usb_get_estado, NULL);
    object_class_property_set_description(oc, "usb-estado", "Pista UEMU: estado del controlador USB y cuentas");
}
