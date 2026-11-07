/* Pista E2: lo que pidió la pista S (work/bitacora_S.md, «PARA LA PISTA E») y la persistencia de la NOR.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * TODO es opcional y, sin sus propiedades, la máquina se comporta exactamente como antes. */

/* ---------------------------------------------------------------- entradas de GPIO forzadas (gpio-in)
 * Puerto p en F9000000 + 10·p (DECIMAL): +0 dato, +2 dirección (1 = salida), +4 máscara de IRQ (1 = el pin
 * interrumpe), +6 nivel que dispara, +8 pendientes [esto último, inferido del DB2010] (docs/hal.md §9;
 * FUN_441a8400 fija +6 al nivel CONTRARIO al actual en el modo 2). IRQ 65..69 = banco 2 bits 5..9.
 * Solo los pines FORZADOS con la propiedad se modelan: leen el nivel forzado mientras sean entradas e interrumpen
 * (por nivel: pin == bit de +6, con +4 a 1). Los demás siguen en el cerrojo, como antes. */
static bool w580_ms_gpio(W580State *s, bool puesta, Error **errp);     /* pista E9 (w580-ms.inc) */
static uint8_t w580_gpio_pend(W580State *s, unsigned p)
{
    hwaddr b = 0xf9000000 + 10 * p;
    uint8_t dir = w580_latch_get(s, b + 2, 1), en = w580_latch_get(s, b + 4, 1), niv = w580_latch_get(s, b + 6, 1);
    return s->gpio_in_mask[p] & ~dir & en & ~(s->gpio_in_val[p] ^ niv);
}
static void w580_gpio_read_fix(W580State *s, hwaddr addr, uint64_t *out)
{
    unsigned o = addr - 0xf9000000, p = o / 10, r = o % 10;
    if (p > 4 || !s->gpio_in_mask[p]) { return; }
    if (r == 0) {
        uint8_t m = s->gpio_in_mask[p] & ~(uint8_t)w580_latch_get(s, 0xf9000000 + 10 * p + 2, 1);
        *out = (*out & ~(uint64_t)m) | (s->gpio_in_val[p] & m);
    } else if (r == 8) {
        *out |= w580_gpio_pend(s, p);
    }
}
static char *w580_get_gpio_in(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    GString *g = g_string_new("");
    for (unsigned p = 0; p < 5; p++) {
        for (unsigned b = 0; b < 8; b++) {
            if (s->gpio_in_mask[p] >> b & 1) { g_string_append_printf(g, "%s%u.%u=%u", g->len ? ":" : "", p, b, s->gpio_in_val[p] >> b & 1); }
        }
    }
    return g_string_free(g, false);
}
static void w580_set_gpio_in(Object *obj, const char *value, Error **errp)
{
    /* «P.B=V» separados por ':' (la coma es de -M): 2.6=1:0.0=0 ; V = 0, 1 o '-' (dejar de forzar). Se acumula. */
    W580State *s = W580_MACHINE(obj);
    g_auto(GStrv) v = g_strsplit(value, ":", -1);
    for (unsigned i = 0; v[i]; i++) {
        const char *c = v[i];
        if (!*c) { continue; }
        if (!(c[0] >= '0' && c[0] <= '4' && c[1] == '.' && c[2] >= '0' && c[2] <= '7' && c[3] == '=' &&
              (c[4] == '0' || c[4] == '1' || c[4] == '-') && !c[5])) {
            error_setg(errp, "gpio-in: «%s» no es PUERTO.BIT=0|1|- (puertos 0..4, bits 0..7, separados por ':')", c);
            return;
        }
        unsigned p = c[0] - '0'; uint8_t m = 1u << (c[2] - '0');
        if (c[4] == '-') { s->gpio_in_mask[p] &= ~m; s->gpio_in_val[p] &= ~m; }
        else { s->gpio_in_mask[p] |= m; s->gpio_in_val[p] = (s->gpio_in_val[p] & ~m) | (c[4] == '1' ? m : 0); }
        qemu_log_mask(LOG_UNIMP, "W580_GPIO entrada P%u.%c %c\n", p, c[2], c[4]);
        if (p == 3 && m == 0x10 && c[4] != '-' && !w580_ms_gpio(s, c[4] == '0', errp)) { return; }      /* pista E9: P3.4 es la tarjeta Memory Stick */
    }
    if (s->cpu) { w580_irq_update(s); }
}

/* ---------------------------------------------------------------- reset y apagado «de verdad» (opcionales)
 * reset-real=on: el «reset forzado» del original (sys_forced_reset 0x441A7164: F9010030 <- 0 y luego F901002C <- 0)
 *   reinicia la máquina en vez de quedarse en su bucle. [observado en la traza `final`] el arranque normal del
 *   original no escribe nunca F9010030 (solo las llaves AA/5A/A5 en +10/+24/+2C).
 * apagado-real=on: PMU 0x41 <- 0 con F9050058 bit 0 a 0 apaga la máquina (QEMU termina), como pide la pista S. */
/* Pista E6 (punto 2 de S3, hallazgo A1 de la pista R): el reset forzado del original son TRES cosas: F9010030 <- 0,
 * F901002C <- 0 y F9010014 <- 1 EN BUCLE (0x441A7174..7A; el pseudo-C se come lo último). Antes `reset-real` saltaba con
 * las dos primeras. Ahora: las dos primeras ARMAN, y la máquina se reinicia al primer F9010014 <- 1 —lo que hace el
 * original— o, si no llega, a los W580_RESET_PERRO_MS (el mismo plazo [inferido] del perro del modo estricto: armado
 * muerde solo). [sin saber] qué es +14: [inferido] «vence ya». */
#define W580_RESET_PERRO_MS 2000
static void w580_reset_vence(void *opaque)
{
    W580State *s = opaque;
    if (!s->reset_real || !s->wdt_armado) { return; }
    s->wdt_armado = false;
    qemu_log_mask(LOG_UNIMP, "W580_RESET_FORZADO sin F9010014 <- 1: a los %d ms de armar el perro (F9010030 <- 0, F901002C <- 0) [inferido el plazo]\n", W580_RESET_PERRO_MS);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}
static void w580_reset_mirar(W580State *s, hwaddr addr, uint64_t value)
{
    if (!s->reset_real) { return; }
    if (addr == 0xf9010030) { s->wdt_30_cero = value == 0; }
    else if (addr == 0xf901002c && value == 0 && s->wdt_30_cero) {
        s->wdt_30_cero = false; s->wdt_armado = true;
        qemu_log_mask(LOG_UNIMP, "W580_RESET_ARMADO pc=0x%08x (espera a F9010014 <- 1, o %d ms)\n", s->cpu->env.regs[15], W580_RESET_PERRO_MS);
        if (!s->wdt_reset_timer) { s->wdt_reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_reset_vence, s); }
        timer_mod(s->wdt_reset_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + W580_RESET_PERRO_MS * INT64_C(1000000));
    } else if (addr == 0xf9010014 && (value & 1) && s->wdt_armado) {
        s->wdt_armado = false; timer_del(s->wdt_reset_timer);
        qemu_log_mask(LOG_UNIMP, "W580_RESET_FORZADO pc=0x%08x (F9010014 <- 1)\n", s->cpu->env.regs[15]);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}
static void w580_apagado_mirar(W580State *s)
{
    if (s->apagado_real && s->pmu_41_cero && !(s->rtc_regs[0x58] & 1)) {
        if (!w580_fa_deja_apagar(s)) { return; }       /* pista E8: apagado-falla (también sin cargador) */
        if (!w580_bat_deja_apagar(s)) { return; }      /* pista E7: con cargador o USB y apaga-con-cargador=off no apaga; queda pendiente */
        qemu_log_mask(LOG_UNIMP, "W580_APAGADO pc=0x%08x\n", s->cpu->env.regs[15]);
        s->pmu_41_cero = false;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}
static bool w580_get_reset_real(Object *obj, Error **errp) { return W580_MACHINE(obj)->reset_real; }
static void w580_set_reset_real(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->reset_real = value; }
static bool w580_get_apagado_real(Object *obj, Error **errp) { return W580_MACHINE(obj)->apagado_real; }
static void w580_set_apagado_real(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->apagado_real = value; }

/* ---------------------------------------------------------------- UART 0 a un chardev (-serial)
 * Con «-serial …» (SERIE=… en emu/lanzar.sh) lo transmitido por la UART 0 sale además por ahí y lo recibido entra en
 * una FIFO: +00 saca un byte, +2E dice cuántos hay, y el estado (+14) levanta 0x1041 mientras haya algo (los mismos
 * bits que el modelo del DB2010, w800-uart.inc de SEW800iEmu) -> IRQ 46 según la máscara +12. Sin -serial, como antes. */
static int w580_uart0_cabe(void *opaque) { W580State *s = opaque; return sizeof(s->uart0_rx) - s->uart0_rx_n; }
static void w580_uart0_recibe(void *opaque, const uint8_t *buf, int size)
{
    W580State *s = opaque;
    for (int i = 0; i < size && s->uart0_rx_n < sizeof(s->uart0_rx); i++) {
        s->uart0_rx[(s->uart0_rx_head + s->uart0_rx_n++) % sizeof(s->uart0_rx)] = buf[i];
    }
    w580_irq_update(s);
}
static void w580_uart0_init(W580State *s)
{
    Chardev *chr = serial_hd(0);
    if (chr) {
        qemu_chr_fe_init(&s->uart0_chr, chr, &error_abort);
        qemu_chr_fe_set_handlers(&s->uart0_chr, w580_uart0_cabe, w580_uart0_recibe, NULL, NULL, s, NULL, true);
        s->uart0_hay = true;
    }
}

/* ---------------------------------------------------------------- generador de tonos F9020000 -> WAV (wav-tono)
 * Lo pide la pista S (como la máquina del W600 de su pista S4): mientras F9020000 & 0x40, onda cuadrada de periodo
 * +4 y ancho +6, en ciclos de 13 MHz. Los registros siguen en el cerrojo; aquí solo se MIRAN las escrituras y se
 * vuelca a un WAV mono de 8 bits y 22 050 Hz el tramo transcurrido (en tiempo virtual) con el estado anterior.
 * [sin probar con un sistema que toque: escrito a partir de la descripción de S; el original no ha tocado
 * F9020000 en ninguna traza mía]. */
#define W580_WAV_HZ 44100       /* pista E4: 16 bits, 0 en silencio y +-16384 sonando (lo que mide sistema/herr/medir_wav.py) */
static void w580_tono_cabecera(W580State *s)
{
    uint32_t n = s->tono_muestras, v;
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0";
    n *= 2; v = 36 + n; memcpy(h + 4, &v, 4);
    v = W580_WAV_HZ; memcpy(h + 24, &v, 4); v *= 2; memcpy(h + 28, &v, 4);
    h[32] = 2; h[34] = 16; memcpy(h + 36, "data", 4); memcpy(h + 40, &n, 4);
    fseek(s->tono_f, 0, SEEK_SET); fwrite(h, 1, 44, s->tono_f); fseek(s->tono_f, 0, SEEK_END); fflush(s->tono_f);
}
static void w580_tono_avanza(W580State *s)
{
    if (!s->tono_f) { return; }
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t hasta = (uint64_t)((t - s->tono_t0) * (int64_t)W580_WAV_HZ / 1000000000);
    uint32_t ctl = w580_latch_get(s, 0xf9020000, 2), per = w580_latch_get(s, 0xf9020004, 2), anc = w580_latch_get(s, 0xf9020006, 2);
    bool suena = (ctl & 0x40) && per > 0;
    if (hasta > s->tono_muestras + (uint64_t)W580_WAV_HZ * 600) { hasta = s->tono_muestras + (uint64_t)W580_WAV_HZ * 600; s->tono_t0 = t - (int64_t)hasta * 1000000000 / W580_WAV_HZ; }
    while (s->tono_muestras < hasta) {
        int16_t m = 0;
        if (suena) {
            double fase = fmod((double)s->tono_muestras * 13000000.0 / W580_WAV_HZ, (double)per);
            m = fase < (double)anc ? 16384 : -16384;
        }
        fputc(m & 255, s->tono_f); fputc((m >> 8) & 255, s->tono_f); s->tono_muestras++;
    }
}
static void w580_tono_mirar(W580State *s, hwaddr addr, uint64_t value)
{
    if (!s->tono_f) { return; }
    w580_tono_avanza(s);          /* el tramo hasta ahora, con el estado que había ANTES de esta escritura */
    if (s->tono_log < 2000) { s->tono_log++;
        qemu_log_mask(LOG_UNIMP, "W580_TONO +%u <- 0x%04x t=%" PRId64 "\n", (unsigned)(addr - 0xf9020000), (unsigned)value,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}
static void w580_tono_cerrar(W580State *s)
{
    if (!s->tono_f) { return; }
    w580_tono_avanza(s); w580_tono_cabecera(s); fclose(s->tono_f); s->tono_f = NULL;
}
static char *w580_get_wav_tono(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->tono_ruta); }
static void w580_set_wav_tono(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    w580_tono_cerrar(s);
    g_free(s->tono_ruta); s->tono_ruta = g_strdup(value);
    if (!value || !*value) { return; }
    s->tono_f = fopen(value, "wb");
    if (!s->tono_f) { error_setg(errp, "wav-tono: no se puede escribir %s", value); return; }
    s->tono_muestras = 0; s->tono_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); s->tono_log = 0;
    w580_tono_cabecera(s);
}

/* ---------------------------------------------------------------- generador de tonos F9020000 -> SUCESOS (tono-sucesos)
 * Pista W (el banco web suena): en vez de muestras, CADA CAMBIO de lo que suena, con su instante en tiempo virtual, en una
 * cola de 512 que quien lee la propiedad se lleva y vacía. La propiedad es de solo lectura y devuelve JSON:
 *   {"ahora":NS,"ctl":C,"per":P,"anc":A,"seq":N,"perdidos":K,"s":[[T_NS,CTL,PER,ANC],…]}
 * ahora = tiempo virtual de la lectura; ctl/per/anc = los tres registros (+0, +4, +6) como están AHORA; s = los sucesos
 * acumulados desde la lectura anterior (el estado de los tres registros DESPUÉS de cada escritura que cambió lo que suena);
 * seq = el número de orden del primero de s (cuenta todos los habidos); perdidos = cuántos se han tirado por llenarse la
 * cola (se tira el más viejo). Suena cuando ctl & 0x40 y per > 0: cuadrada de 13 MHz / per, arriba anc de cada per ciclos.
 * NO HACE NADA hasta la primera lectura: la cola se enciende entonces (y esa lectura ya da el estado de ahora), así que
 * una máquina a la que nadie le pide la propiedad es la misma de antes. Una escritura que no cambia lo que suena (el
 * periodo de una nota con el generador callado) no es un suceso; el estado de después lo lleva el siguiente. */
static bool w580_tsuc_suena(unsigned ctl, unsigned per) { return (ctl & 0x40) && per > 0; }
static void w580_tsuc_mirar(W580State *s, hwaddr addr, unsigned size, uint64_t value)
{
    uint8_t r[8];
    for (unsigned i = 0; i < 8; i++) { r[i] = w580_latch_get(s, 0xf9020000 + i, 1); }
    unsigned ctl0 = r[0] | r[1] << 8, per0 = r[4] | r[5] << 8, anc0 = r[6] | r[7] << 8;
    for (unsigned i = 0; i < size && (addr - 0xf9020000) + i < 8; i++) { r[(addr - 0xf9020000) + i] = (value >> (8 * i)) & 255; }
    unsigned ctl = r[0] | r[1] << 8, per = r[4] | r[5] << 8, anc = r[6] | r[7] << 8;
    bool antes = w580_tsuc_suena(ctl0, per0), ahora = w580_tsuc_suena(ctl, per);
    if ((!antes && !ahora) || (ctl == ctl0 && per == per0 && anc == anc0)) { return; }
    if (s->tsuc_n == G_N_ELEMENTS(s->tsuc)) { s->tsuc_ini = (s->tsuc_ini + 1) % G_N_ELEMENTS(s->tsuc); s->tsuc_n--; s->tsuc_perdidos++; }
    unsigned k = (s->tsuc_ini + s->tsuc_n++) % G_N_ELEMENTS(s->tsuc);
    s->tsuc[k].t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); s->tsuc[k].ctl = ctl; s->tsuc[k].per = per; s->tsuc[k].anc = anc;
    s->tsuc_seq++;
}
static char *w580_get_tono_sucesos(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    GString *g = g_string_new(NULL);
    g_string_append_printf(g, "{\"ahora\":%" PRId64 ",\"ctl\":%u,\"per\":%u,\"anc\":%u,\"seq\":%" PRIu64 ",\"perdidos\":%" PRIu64 ",\"s\":[",
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), (unsigned)w580_latch_get(s, 0xf9020000, 2), (unsigned)w580_latch_get(s, 0xf9020004, 2),
                           (unsigned)w580_latch_get(s, 0xf9020006, 2), s->tsuc_seq - s->tsuc_n, s->tsuc_perdidos);
    for (unsigned i = 0; i < s->tsuc_n; i++) {
        unsigned k = (s->tsuc_ini + i) % G_N_ELEMENTS(s->tsuc);
        g_string_append_printf(g, "%s[%" PRId64 ",%u,%u,%u]", i ? "," : "", s->tsuc[k].t, s->tsuc[k].ctl, s->tsuc[k].per, s->tsuc[k].anc);
    }
    g_string_append(g, "]}");
    s->tsuc_ini = s->tsuc_n = 0; s->tsuc_on = true;
    return g_string_free(g, false);
}

/* ---------------------------------------------------------------- persistencia de la NOR (nor-persistente)
 * Si la propiedad tiene una ruta, al SALIR QEMU (quit por QMP, SIGTERM, apagado) se vuelcan ahí los 64 MiB de la NOR
 * (a RUTA.tmp y luego rename, para no dejar un fichero a medias). Con «kill -9» no se guarda nada. */
static void w580_al_salir(Notifier *n, void *data)
{
    W580State *s = container_of(n, W580State, salida);
    w580_es_volcar(s);
    w580_tono_cerrar(s);
    if (s->acel_f) { fclose(s->acel_f); s->acel_f = NULL; }
    if (s->nor && s->nor_persistente && *s->nor_persistente) {
        g_autofree char *tmp = g_strdup_printf("%s.tmp", s->nor_persistente);
        if (w580_nor_volcar(s->nor, tmp) && rename(tmp, s->nor_persistente) == 0) {
            qemu_log_mask(LOG_UNIMP, "W580_NOR persistida en %s\n", s->nor_persistente);
        } else {
            error_report("w580: no se pudo persistir la NOR en %s", s->nor_persistente);
        }
    }
}
static char *w580_get_nor_persistente(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->nor_persistente); }
static void w580_set_nor_persistente(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_free(s->nor_persistente); s->nor_persistente = g_strdup(value);
}

static void w580_extra_init(W580State *s)
{
    w580_uart0_init(s);
    s->salida.notify = w580_al_salir;
    qemu_add_exit_notifier(&s->salida);
}
static void w580_extra_reset(W580State *s)
{
    s->wdt_30_cero = false; s->pmu_41_cero = false;
    s->wdt_armado = false; if (s->wdt_reset_timer) { timer_del(s->wdt_reset_timer); }
    s->uart0_rx_head = s->uart0_rx_n = 0;
}
static void w580_extra_class_init(ObjectClass *oc)
{
    object_class_property_add_str(oc, "gpio-in", w580_get_gpio_in, w580_set_gpio_in);
    object_class_property_set_description(oc, "gpio-in", "Entradas de GPIO forzadas: PUERTO.BIT=0|1|- separadas por ':' (p. ej. 2.6=1:0.0=0); se acumulan; con IRQ 65..69");
    object_class_property_add_bool(oc, "reset-real", w580_get_reset_real, w580_set_reset_real);
    object_class_property_set_description(oc, "reset-real", "on: F9010030<-0 y F901002C<-0 (reset forzado del original) reinician la maquina");
    object_class_property_add_bool(oc, "apagado-real", w580_get_apagado_real, w580_set_apagado_real);
    object_class_property_set_description(oc, "apagado-real", "on: PMU 0x41<-0 con F9050058 bit 0 a 0 apaga la maquina (QEMU termina)");
    object_class_property_add_str(oc, "wav-tono", w580_get_wav_tono, w580_set_wav_tono);
    object_class_property_set_description(oc, "wav-tono", "Ruta de un WAV (mono, 16 bits, 44100 Hz) con lo que toca el generador de tonos F9020000");
    object_class_property_add_str(oc, "tono-sucesos", w580_get_tono_sucesos, NULL);
    object_class_property_set_description(oc, "tono-sucesos", "Pista W: los cambios del generador de tonos F9020000 desde la lectura anterior, con su tiempo virtual, en JSON (leerla los entrega y vacia la cola; la cola se enciende con la primera lectura)");
    object_class_property_add_str(oc, "nor-persistente", w580_get_nor_persistente, w580_set_nor_persistente);
    object_class_property_set_description(oc, "nor-persistente", "Ruta donde se vuelca la NOR al salir QEMU (persistencia automatica)");
}
