/* Pantalla del W580 por el bloque gráfico «XGAM» del DB2020. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * NO es el PDI del DB2010 (F7000100 + DMA PL080): aquí hay dos piezas [observado en e13 y en el pseudo-C]:
 *   PDI    F7001000: órdenes al panel (Renesas R61505). Bytes alineados a la derecha hasta +0x0FC, un bit
 *          orden/dato por byte hasta +0x11C, +0x1DC lanza (1 escribe, 3 lee registro), +0x1F4 bit 0 = ocupado.
 *   PDICON F7001400: manda el CUADRO desde la memoria gráfica sin pasar por la CPU:
 *          +0x40 ancho|alto<<16 del panel, +0x44 bits por píxel del panel, +0x48 x|y<<16 y +0x4C ancho|alto<<16
 *          del rectángulo — la esquina es la del rectángulo DENTRO del búfer de origen (y en el panel): [observado en
 *          e32: leyendo las filas 0..h-1 el menú salía con la barra de estado repetida en y=112 y 167] —, +0x50 origen (desplazamiento dentro de la SDRAM: 0xE80000 = EXT_GRAM 0x20E80000;
 *          FUN_441af0c4), +0x54 píxeles por fila del origen, +0x58 bits por píxel del origen,
 *          +0x04 <- 2 (o 3) lanza la transferencia (FUN_441adb8c / FUN_441adcf4), +0x08 bit 0 = ocupado.
 * Este modelo ejecuta la transferencia del PDICON en el acto (nunca «ocupado») y pinta lo que el main manda,
 * en RGB565. Sin retroiluminación.
 *
 * Pista E3 (bitacora_E3.md), tres cosas más:
 *   MMIACC F7001800 («acelerador 2D»): cola de órdenes en +0x04. El main original manda UNA orden tras cada cuadro
 *          [observado: 183 de 183 transferencias en e3_01]: 0x251, destino, origen, palabras = copia de memoria
 *          gráfica (FUN_441af71c, llamada por FUN_449d5b94 desde gld_display_refresh): copia el búfer recién enviado
 *          sobre el otro, para que el siguiente cuadro se dibuje encima de lo último que se vio. Sin esa copia el
 *          búfer de dibujo va DOS cuadros atrasado en todo lo que no se repinta: era el «defecto de restos».
 *          Las demás órdenes vistas (0x100/0x101/0x102 zona de trabajo, 0x112/0x113 recorte, 0x10E) llevan un
 *          argumento y aquí no hacen nada. +0x08: bit 3 = cola ocupada, bit 1 = trabajando; el modelo copia en el
 *          acto, así que leen 0 (lo que ya daba el cerrojo).
 *   Panel: el programa «gc» del PDI manda, antes de los píxeles, los 10 bytes del área de datos
 *          (`00 20 xh xl 00 21 yh yl 00 22`: R20/R21 = cursor de la GRAM, R22 = escribir). El modelo coloca el
 *          cuadro DONDE DICE ESE CURSOR, dentro de la ventana R50..R53 (con R03 en horizontal, ID = 3 y AM = 0:
 *          al pasar del borde derecho de la ventana salta al izquierdo de la fila siguiente) [inferido de la hoja
 *          del R61505 y de hal.md §4.3; observado: en el original cursor == esquina del rectángulo y ventana
 *          completa en 183 de 183]. Si el área de datos no trae un `pdidataarea` reconocible se coloca como antes,
 *          con x,y del PDICON, y se avisa (W580_PDICON_SIN_CURSOR).
 *   Panel dormido: con R07 sin D1:D0 o R10 en reposo la consola (y las capturas) salen en NEGRO (pedido por la
 *          pista S); la memoria del panel se conserva y reaparece al encender. `panel-dormido-negro=off` lo quita.
 *   `pantalla-registro=FICHERO`: una línea TSV por transferencia del PDICON, por orden del MMIACC, por registro del
 *          panel escrito y por tecla (kp-cell), con el tiempo virtual. Sin tope (lo acota quien lo pide).
 */
struct W580LCD {
    W580State *s;
    QemuConsole *console;
    uint32_t fb[240 * 320];
    bool dirty;
    uint64_t cuadros, pixeles_no_negros;
    unsigned log_pdi, log_con, log_panel, log_lee;
    uint32_t codigo;
    uint16_t panel[256];
    /* pista E3 */
    uint32_t mmi_op, mmi_arg[3];
    unsigned mmi_n, mmi_faltan, log_mmi, log_cursor;
    uint64_t mmi_copias, mmi_bytes, mmi_desconocidas;
    bool negro_dormido, pintado_negro;
    FILE *traza_f;
};

static void G_GNUC_PRINTF(2, 3) w580_lcd_traza(W580State *s, const char *fmt, ...)
{
    W580LCD *l = s->lcd;
    if (!l || !l->traza_f) { return; }
    va_list ap;
    fprintf(l->traza_f, "%.3f\t", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6);
    va_start(ap, fmt); vfprintf(l->traza_f, fmt, ap); va_end(ap);
    fputc('\n', l->traza_f); fflush(l->traza_f);
}
#include "w580-panel.inc"
/* Pista E6: con panel-real «encendido» es el estado FÍSICO del modelo del R61505 (alimentado y visualizando). */
static bool w580_panel_encendido(W580LCD *l) { return l->s->panel_real ? w580_pr_visible(l) : (l->panel[7] & 3) && !(l->panel[0x10] & 7); }
static void w580_lcd_update(void *opaque)
{
    W580LCD *l = opaque;
    DisplaySurface *surface = qemu_console_surface(l->console);
    bool negro = l->negro_dormido && !w580_panel_encendido(l);
    if ((!l->dirty && negro == l->pintado_negro) || !surface) { return; }
    if (l->s->panel_real && !negro) { w580_pr.fb_viejo = true; w580_pr_a_fb(l); }
    for (unsigned y = 0; y < 320; y++) {
        if (negro) { memset(surface_data(surface) + y * surface_stride(surface), 0, 240 * 4); }
        else { memcpy(surface_data(surface) + y * surface_stride(surface), l->fb + y * 240, 240 * 4); }
    }
    l->dirty = false; l->pintado_negro = negro;
    dpy_gfx_update(l->console, 0, 0, 240, 320);
}
static void w580_lcd_invalidate(void *opaque) { ((W580LCD *)opaque)->dirty = true; }
static const GraphicHwOps w580_lcd_graphics = { .invalidate = w580_lcd_invalidate, .gfx_update = w580_lcd_update };

static void w580_pdicon_transfer(W580State *s, uint32_t orden)
{
    W580LCD *l = s->lcd;
    uint32_t pos = w580_latch_get(s, 0xf7001448, 4), tam = w580_latch_get(s, 0xf700144c, 4);
    uint32_t org = w580_latch_get(s, 0xf7001450, 4), fila = w580_latch_get(s, 0xf7001454, 4);
    uint32_t bpp = w580_latch_get(s, 0xf7001458, 4);
    unsigned x0 = pos & 0x7ff, y0 = (pos >> 16) & 0x7ff, w = tam & 0x7ff, h = (tam >> 16) & 0x7ff;
    if (l->log_con < 300) { l->log_con++;
        qemu_log_mask(LOG_UNIMP, "W580_PDICON orden=%u x=%u y=%u w=%u h=%u origen=0x%08x fila=%u bpp_origen=%u panel=0x%08x bpp_panel=%u "
                      "ctl=0x%08x ventana_panel=[%u..%u]x[%u..%u] AC=(%u,%u) R03=0x%04x pc=0x%08x lr=0x%08x\n", orden, x0, y0, w, h, org, fila, bpp,
                      (unsigned)w580_latch_get(s, 0xf7001440, 4), (unsigned)w580_latch_get(s, 0xf7001444, 4),
                      (unsigned)w580_latch_get(s, 0xf7001410, 4), l->panel[0x50], l->panel[0x51], l->panel[0x52], l->panel[0x53],
                      l->panel[0x20], l->panel[0x21], l->panel[3], s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    /* (estética, giro, 26-oct) EL GIRO DEL PDICON (+60 = código, +5C = esquina de partida), cuadro ENTERO, 16 bpp: lo que hace senew
     * de lado o boca abajo (hal/lcd.c lcd_enviar_girado) y el visor del original en «Landscape». Correspondencias MEDIDAS en el aparato
     * (docs/estetica.md §7): código 5 = el arriba del cuadro (320x240) en el borde DERECHO del panel; 6 = en el IZQUIERDO (y su izquierda
     * abajo); 3 = boca abajo (240x320). Sin modelo: otros códigos, rectángulos parciales y el panel de verdad (pista E6). */
    {
        uint32_t cod = w580_latch_get(s, 0xf7001460, 4);
        if (cod && !s->panel_real && bpp == 16 && x0 == 0 && y0 == 0 && w == 240 && h == 320 && org < 0x1000000 && (cod == 5 || cod == 6 || cod == 3)) {
            const unsigned an = cod == 3 ? 240 : 320, al = cod == 3 ? 320 : 240;
            uint16_t *c = g_new(uint16_t, (size_t)fila * al);
            w580_ca.hw = true;
            bool leido = fila >= an && address_space_read(&address_space_memory, 0x20000000 + org, MEMTXATTRS_UNSPECIFIED, c, (size_t)fila * al * 2) == MEMTX_OK;
            w580_ca.hw = false;
            if (leido) {
                for (unsigned py = 0; py < 320; py++) for (unsigned px = 0; px < 240; px++) {
                    unsigned sx = cod == 5 ? py : cod == 6 ? an - 1 - py : 239 - px, sy = cod == 5 ? al - 1 - px : cod == 6 ? px : 319 - py;
                    uint16_t v = le16_to_cpu(c[sy * fila + sx]);
                    unsigned r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
                    l->fb[py * 240 + px] = (r << 16) | (g << 8) | b;
                }
                w580_lcd_traza(s, "PDICON_GIRO	%u	0x%x	fila=%u", cod, org, fila);
                l->cuadros++; l->dirty = true;
            }
            g_free(c);
            return;
        }
    }
    if (bpp != 16 || !w || !h || x0 + w > 240 || y0 + h > 320 || fila < w || org >= 0x1000000) {
        static unsigned n;
        if (n++ < 50) { qemu_log_mask(LOG_UNIMP, "W580_PDICON_SIN_MODELO (formato o rectangulo no previsto)\n"); }
        w580_lcd_traza(s, "PDICON_SIN_MODELO\t%u\t0x%x\t%u\t%u\t%u\t%u\tbpp=%u fila=%u", orden, org, x0, y0, w, h, bpp, fila);
        if (s->panel_real) { w580_pr_aviso(s, PR_A_PDI | 0x73, "PDICON: rectangulo (%u,%u) %ux%u, fila %u, %u bpp de origen: fuera de lo que el modelo sabe mandar; el panel no recibe nada", x0, y0, w, h, fila, bpp); }
        return;
    }
    if (s->panel_real) { w580_pr_pdicon(s, org, fila, x0, y0, w, h); return; }       /* pista E6: el cuadro pasa por el microcódigo y por el R61505 */
    /* Lo que el programa «gc» manda al panel antes de los píxeles: el área de datos del PDI (10 bytes hasta +0xFF). */
    uint8_t ar[10];
    for (unsigned i = 0; i < 10; i++) { ar[i] = w580_latch_get(s, 0xf70010f6 + i, 1); }
    bool hay_cursor = ar[0] == 0 && ar[1] == 0x20 && ar[4] == 0 && ar[5] == 0x21 && ar[8] == 0 && ar[9] == 0x22;
    unsigned px = x0, py = y0, hsa = 0, hea = 239, vsa = 0, vea = 319;
    bool ventana = false;
    if (hay_cursor) {
        l->panel[0x20] = (ar[2] << 8) | ar[3]; l->panel[0x21] = (ar[6] << 8) | ar[7];
        px = l->panel[0x20]; py = l->panel[0x21];
        /* Ventana R50..R53 y modo de entrada R03: solo el barrido horizontal creciente (ID = 3, AM = 0), que es el
         * único que usan el original (0x1230) y el valor de reset (0x0030). */
        if (l->panel[0x50] <= l->panel[0x51] && l->panel[0x51] <= 239 && l->panel[0x52] <= l->panel[0x53] && l->panel[0x53] <= 319
            && (l->panel[3] & 0x38) == 0x30 && px >= l->panel[0x50] && px <= l->panel[0x51] && py >= l->panel[0x52] && py <= l->panel[0x53]) {
            hsa = l->panel[0x50]; hea = l->panel[0x51]; vsa = l->panel[0x52]; vea = l->panel[0x53]; ventana = true;
        }
        if ((!ventana || px != x0 || py != y0) && l->log_cursor < 50) { l->log_cursor++;
            qemu_log_mask(LOG_UNIMP, "W580_PDICON_CURSOR cursor=(%u,%u) rect=(%u,%u) %ux%u ventana=[%u..%u]x[%u..%u] R03=0x%04x%s\n", px, py, x0, y0, w, h,
                          l->panel[0x50], l->panel[0x51], l->panel[0x52], l->panel[0x53], l->panel[3],
                          ventana ? "" : " (ventana, modo o cursor sin modelo: se coloca en el cursor sin envolver)");
        }
        if (px >= 240 || py >= 320) { px = x0; py = y0; ventana = false; }
    } else if (l->log_cursor < 50) { l->log_cursor++;
        qemu_log_mask(LOG_UNIMP, "W580_PDICON_SIN_CURSOR area[F6..FF]=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x: se coloca con x,y del PDICON (%u,%u); "
                      "el panel real recibiria esos bytes antes de los pixeles\n", ar[0], ar[1], ar[2], ar[3], ar[4], ar[5], ar[6], ar[7], ar[8], ar[9], x0, y0);
    }
    w580_lcd_traza(s, "PDICON\t%" PRIu64 "\t0x%x\t%u\t%u\t%u\t%u\t%s\t%u,%u\t%u..%u x %u..%u\t0x%04x\t%s", l->cuadros + 1, org, x0, y0, w, h,
                   hay_cursor ? "cursor" : "SIN_CURSOR", px, py, l->panel[0x50], l->panel[0x51], l->panel[0x52], l->panel[0x53], l->panel[3],
                   w580_panel_encendido(l) ? "encendido" : "dormido");
    uint16_t *linea = g_new(uint16_t, w);
    unsigned cx = px, cy = py;       /* contador de dirección de la GRAM del panel */
    W580CaLectura cl = { 0 };        /* pista E5: caché de datos (solo con estricto o aprender) */
    for (unsigned y = 0; y < h; y++) {
        hwaddr dir = 0x20000000 + org + ((hwaddr)(y0 + y) * fila + x0) * 2;
        w580_ca.hw = true;      /* pista E6: lo lee el hardware, no la CPU (cuenta con estricto-cache-lecturas) */
        bool leido = address_space_read(&address_space_memory, dir, MEMTXATTRS_UNSPECIFIED, linea, w * 2) == MEMTX_OK;
        w580_ca.hw = false;
        if (!leido) { break; }
        w580_ca_hw_lee(s, dir, (uint8_t *)linea, w * 2, &cl);
        for (unsigned x = 0; x < w; x++) {
            uint16_t v = le16_to_cpu(linea[x]);
            unsigned r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
            if (ventana) {
                l->fb[cy * 240 + cx] = (r << 16) | (g << 8) | b;
                if (++cx > hea) { cx = hsa; if (++cy > vea) { cy = vsa; } }
            } else if (py + y < 320 && px + x < 240) {
                l->fb[(py + y) * 240 + px + x] = (r << 16) | (g << 8) | b;
            }
        }
    }
    g_free(linea);
    w580_ca_pdicon_fin(s, &cl, org, x0, y0, w, h);
    l->cuadros++; l->dirty = true;
}
/* MMIACC: cola de órdenes de F7001804. Ver la cabecera. Las direcciones son desplazamientos dentro de la memoria
 * gráfica, como el origen del PDICON (PD_XGAM_GRAM_PtrToOffset: dirección - 0x20000000 con los bancos del original). */
static void w580_mmiacc_cola(W580State *s, uint32_t v)
{
    W580LCD *l = s->lcd;
    if (!l->mmi_faltan) {
        l->mmi_op = v; l->mmi_n = 0;
        l->mmi_faltan = v == 0x251 ? 3 : (v & 0xff00) == 0x100 ? 1 : 0;
        if (!l->mmi_faltan) {
            l->mmi_desconocidas++;
            if (l->log_mmi < 60) { l->log_mmi++;
                qemu_log_mask(LOG_UNIMP, "W580_MMIACC orden 0x%x SIN MODELO pc=0x%08x lr=0x%08x\n", v, s->cpu->env.regs[15], s->cpu->env.regs[14]);
            }
            w580_lcd_traza(s, "MMIACC_SIN_MODELO\t0x%x", v);
        }
        return;
    }
    l->mmi_arg[l->mmi_n++] = v;
    if (--l->mmi_faltan) { return; }
    if (l->mmi_op != 0x251) {
        if (l->log_mmi < 60) { l->log_mmi++; qemu_log_mask(LOG_UNIMP, "W580_MMIACC orden 0x%x arg=0x%x (sin efecto en el modelo)\n", l->mmi_op, l->mmi_arg[0]); }
        w580_lcd_traza(s, "MMIACC\t0x%x\t0x%x", l->mmi_op, l->mmi_arg[0]);
        return;
    }
    uint32_t dst = l->mmi_arg[0], src = l->mmi_arg[1];
    uint64_t n = (uint64_t)l->mmi_arg[2] * 4;
    bool ok = n && dst < 0x1000000 && src < 0x1000000 && dst + n <= 0x1000000 && src + n <= 0x1000000;
    if (ok && !w580_ca_mmiacc(s, dst, src, n)) {      /* pista E5: con estricto o aprender la copia dentro de la memoria gráfica la hace el modelo de caché */
        uint8_t *tmp = g_malloc(n);
        w580_ca.hw = true;
        ok = address_space_read(&address_space_memory, 0x20000000 + src, MEMTXATTRS_UNSPECIFIED, tmp, n) == MEMTX_OK
          && address_space_write(&address_space_memory, 0x20000000 + dst, MEMTXATTRS_UNSPECIFIED, tmp, n) == MEMTX_OK;
        w580_ca.hw = false;
        g_free(tmp);
    }
    if (ok) { l->mmi_copias++; l->mmi_bytes += n; }
    if (l->log_mmi < 60 || !ok) { l->log_mmi++;
        qemu_log_mask(LOG_UNIMP, "W580_MMIACC copia destino=0x%x origen=0x%x palabras=0x%x%s pc=0x%08x lr=0x%08x\n", dst, src, l->mmi_arg[2],
                      ok ? "" : " FUERA DE LA SDRAM: no se hace", s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
    w580_lcd_traza(s, "MMIACC\t0x251\t0x%x\t0x%x\t0x%x\t%s", dst, src, l->mmi_arg[2], ok ? "copiado" : "NO_COPIADO");
}
/* Una escritura de registros del panel es «índice (2 bytes, marcados como orden) + valor (2 bytes)», varias por
 * transferencia. El primer byte de la transferencia lleva el bit 31 de +0x11C y los datos acaban en +0xFF, así que
 * el tamaño sale del último bit de orden: n = k_último + 3 [observado en los 660 primeros W580_PDI de e22: 4, 8, 12
 * y 24 bytes]. Se guardan los registros del R61505 (0x07 control de pantalla, 0x10 potencia) solo para SABER si el
 * panel está encendido; el dibujo no depende de ellos. */
static void w580_pdi_orden(W580State *s, uint32_t orden)
{
    W580LCD *l = s->lcd;
    if (s->panel_real) { w580_pr_pdi(s, orden); return; }      /* pista E6 */
    uint32_t rs = w580_latch_get(s, 0xf700111c, 4);
    if (orden == 1 && rs) {
        unsigned ultimo = 31 - ctz32(rs), n = ultimo + 3;
        if (n <= 32 && !(n & 3)) {
            for (unsigned i = 0; i + 3 < n; i += 4) {
                hwaddr a = 0xf7001100 - n + i;
                if ((rs >> (30 - i) & 3) != 3) { continue; }
                unsigned reg = (w580_latch_get(s, a, 1) << 8) | w580_latch_get(s, a + 1, 1);
                unsigned val = (w580_latch_get(s, a + 2, 1) << 8) | w580_latch_get(s, a + 3, 1);
                bool cambia = reg < 256 && l->panel[reg] != val;
                if (reg < 256) { l->panel[reg] = val; }
                if (reg == 0x07 || reg == 0x10) { l->dirty = true; w580_luz_muestra(s); }       /* puede cambiar encendido <-> dormido */
                w580_lcd_traza(s, "PANEL\t0x%04x\t0x%04x", reg, val);
                if ((reg == 0x07 || reg == 0x10) && cambia && l->log_panel < 400) { l->log_panel++;
                    qemu_log_mask(LOG_UNIMP, "W580_PANEL reg=0x%02x <- 0x%04x t=%" PRId64 " ms (cuadro %" PRIu64 ")\n", reg, val,
                                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), l->cuadros);
                }
            }
        }
    }
    if (orden == 3) {
        /* Lectura de un registro del panel: índice en +0xFE/+0xFF; el resultado es el byte 1 de +0x1F4 y hay dos
         * programas (byte bajo / byte alto) elegidos por el byte 2 de +0x1FC (docs/hal.md §4.5 D). */
        unsigned reg = (w580_latch_get(s, 0xf70010fe, 1) << 8) | w580_latch_get(s, 0xf70010ff, 1);
        uint32_t fc = w580_latch_get(s, 0xf70011fc, 4);
        /* El registro 0 es el código de dispositivo: 0x1505 en un R61505 [inferido: hoja de datos; el main lo lee cada
         * 2 s y, con 0, apaga y reinicia el panel en bucle (e27..e30)]. Programa 0x70 = «readhigh», 0x9C = «read»
         * [inferido del orden de carga de los programas de dispdriver0.dat: write 0x00, gc 0x1C, readhigh 0x70, read 0x9C]. */
        unsigned val = reg == 0 ? l->codigo : (reg == 0x28 && s->panel_r28 != UINT32_MAX) ? (s->panel_r28 & 0xffff) : reg < 256 ? l->panel[reg] : 0;
        unsigned prog = (fc >> 16) & 0xff;
        w580_latch_set(s, 0xf70011f4, 4, ((prog == 0x70 ? val >> 8 : val) & 0xff) << 8);
        if (l->log_lee < 300) { l->log_lee++;
            qemu_log_mask(LOG_UNIMP, "W580_PANEL lee reg=0x%04x (guardado 0x%04x) 1FC=0x%08x t=%" PRId64 " ms pc=0x%08x lr=0x%08x\n", reg, val, fc,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), s->cpu->env.regs[15], s->cpu->env.regs[14]);
        }
    }
    if (l->log_pdi < 600) { l->log_pdi++;
        char hex[80]; unsigned n = 0;
        for (hwaddr a = 0xf70010e8; a < 0xf7001100; a++) { n += snprintf(hex + n, 4, "%02x", (unsigned)w580_latch_get(s, a, 1)); }
        qemu_log_mask(LOG_UNIMP, "W580_PDI orden=%u datos[E8..FF]=%s rs=0x%08x e0=0x%08x pc=0x%08x lr=0x%08x\n", orden, hex,
                      rs, (unsigned)w580_latch_get(s, 0xf70011e0, 4), s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
}
/* Devuelve true si la escritura queda consumida. Las demás siguen al cerrojo (y al registro W580_ES). */
static bool w580_lcd_write(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    if (addr == 0xf7001404 && size == 4) {
        if (value == 2 || value == 3) { w580_pdicon_transfer(s, value); }
        else { qemu_log_mask(LOG_UNIMP, "W580_PDICON orden=%u SIN MODELO pc=0x%08x\n", (unsigned)value, s->cpu->env.regs[15]); }
        return true;
    }
    if (addr == 0xf70011dc && size == 4) { w580_pdi_orden(s, value); return true; }
    if (addr == 0xf7001804 && size == 4) { w580_mmiacc_cola(s, value); }       /* no se consume: sigue al cerrojo */
    return false;
}
static char *w580_get_pantalla(Object *obj, Error **errp)
{
    W580LCD *l = W580_MACHINE(obj)->lcd;
    unsigned no_negros = 0;
    if (W580_MACHINE(obj)->panel_real) { w580_pr_a_fb(l); }
    for (unsigned i = 0; i < 240 * 320; i++) { if (l->fb[i]) { no_negros++; } }
    return g_strdup_printf("cuadros=%" PRIu64 " pixeles_no_negros=%u panel: R07=0x%04x R10=0x%04x (%s)\n", l->cuadros, no_negros,
                           l->panel[7], l->panel[0x10], w580_panel_encendido(l) ? "encendido" : "apagado o en reposo");
}
static char *w580_get_pantalla_mmiacc(Object *obj, Error **errp)
{
    W580LCD *l = W580_MACHINE(obj)->lcd;
    return g_strdup_printf("mmiacc: copias=%" PRIu64 " bytes=%" PRIu64 " ordenes_sin_modelo=%" PRIu64 " panel: cursor=(%u,%u) ventana=[%u..%u]x[%u..%u] R03=0x%04x\n",
                           l->mmi_copias, l->mmi_bytes, l->mmi_desconocidas, l->panel[0x20], l->panel[0x21],
                           l->panel[0x50], l->panel[0x51], l->panel[0x52], l->panel[0x53], l->panel[3]);
}
static void w580_lcd_traza_abrir(W580State *s, Error **errp)
{
    W580LCD *l = s->lcd;
    if (!l) { return; }                 /* al lanzar (-M …,pantalla-registro=F) todavía no hay pantalla: lo abre w580_lcd_init */
    if (l->traza_f) { fclose(l->traza_f); l->traza_f = NULL; }
    if (s->lcd_traza_ruta && *s->lcd_traza_ruta) {
        l->traza_f = fopen(s->lcd_traza_ruta, "w");
        if (!l->traza_f) { error_setg(errp, "no se pudo abrir %s", s->lcd_traza_ruta); return; }
        fprintf(l->traza_f, "# t_ms\tPDICON\tn\torigen\tx\ty\tw\th\tcursor?\tcursor_x,y\tventana R50..R53\tR03\tpanel | MMIACC\torden\tdestino\torigen\tpalabras | "
                            "PANEL\treg\tvalor | TECLA\tcelda\tpulsada\n");
    }
}
static char *w580_get_pantalla_registro(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->lcd_traza_ruta); }
static void w580_set_pantalla_registro(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_free(s->lcd_traza_ruta); s->lcd_traza_ruta = g_strdup(value);
    w580_lcd_traza_abrir(s, errp);
}
static bool w580_get_negro_dormido(Object *obj, Error **errp) { return !W580_MACHINE(obj)->lcd_dormido_visible; }
static void w580_set_negro_dormido(Object *obj, bool value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    s->lcd_dormido_visible = !value;
    if (s->lcd) { s->lcd->negro_dormido = value; s->lcd->dirty = true; }
}
static void w580_lcd_class_init(ObjectClass *oc)
{
    object_class_property_add_str(oc, "pantalla-mmiacc", w580_get_pantalla_mmiacc, NULL);
    object_class_property_set_description(oc, "pantalla-mmiacc", "Copias hechas por el MMIACC (F7001804) y cursor/ventana del panel");
    object_class_property_add_str(oc, "pantalla-registro", w580_get_pantalla_registro, w580_set_pantalla_registro);
    object_class_property_set_description(oc, "pantalla-registro", "Fichero TSV: cada transferencia del PDICON, orden del MMIACC, registro del panel y tecla, con el tiempo virtual");
    object_class_property_add_bool(oc, "panel-dormido-negro", w580_get_negro_dormido, w580_set_negro_dormido);
    object_class_property_set_description(oc, "panel-dormido-negro", "Con el panel apagado o en reposo la consola sale en negro (por defecto on)");
    w580_pr_class_init(oc);
}
static void w580_lcd_reset(W580State *s)
{
    W580LCD *l = s->lcd;
    memset(l->fb, 0, sizeof(l->fb));
    memset(l->panel, 0, sizeof(l->panel));
    /* Valores de reset del R61505 que importan para colocar el cuadro [inferido de la hoja de datos]: ventana completa
     * y barrido horizontal creciente. El original los escribe igualmente en su guion «init». */
    l->panel[0x03] = 0x0030; l->panel[0x51] = 239; l->panel[0x53] = 319;
    l->mmi_faltan = 0; l->mmi_n = 0;
    l->dirty = true;
    w580_pr_reset(s);
}
static void w580_lcd_init(W580State *s)
{
    W580LCD *l = g_new0(W580LCD, 1);
    s->lcd = l; l->s = s; l->codigo = s->panel_id; l->negro_dormido = !s->lcd_dormido_visible;
    w580_lcd_traza_abrir(s, &error_fatal);
    w580_pr_init(s);
    l->console = graphic_console_init(NULL, 0, &w580_lcd_graphics, l);
    qemu_console_resize(l->console, 240, 320);
}
