/* W580 (pista E5): indicador de «panel encendido E iluminado». SPDX-License-Identifier: GPL-2.0-or-later
 *
 * No es una regla: es un ESTADO que se lee (propiedad «luces», `emu/w580 estado`) para comparar lado a lado lo que
 * el main original deja escrito cuando enseña el standby con lo que deja nuestro sistema. No cambia nada del
 * comportamiento (solo guarda un historial de cambios), con o sin estricto.
 *
 * Las luces candidatas son las que maneja el código del original (docs/hal.md §4.6) [observado en su código]:
 *   PMU   : I2C 0, 0x4A: registro 0x43 bit 7 = encendida, 0x45 = nivel (el original solo escribe 0x50)
 *   PWM A : F9000036 = nivel (8 bits), F9000032 bit 2 = habilitado, pin P3.1 a 1
 *   PWM B : F900007C = nivel (10 bits; periodo F9000078 = 0x400), F9000032 bit 3, pin P3.2 a 1
 *   chip  : I2C 1, 0x40 («DCDC LED»): 0x00 bit 3 = elevador, 0x01/0x02 = qué salidas, 0x09/0x0A/0x2F = niveles
 * CUÁL de ellas ilumina el cristal: [sin saber] (no se ha visto en un aparato). El veredicto «como el original» compara
 * con la firma que el original deja en standby (W580_LUZ_REF_*, medida en el emulador con el chip 0x40 presente). */
typedef struct { uint8_t panel, pmu43, pmu45, pmuA0, pmuA1, pmu36, pmu37, pmuB0, p3dat, p3dir, chip_hay, chip[8]; uint16_t pwm_hab, pwm_a, pwm_b, pwm_per, ruta3; } W580LuzEstado;
static const uint8_t w580_luz_chip_reg[8] = { 0x00, 0x01, 0x02, 0x09, 0x0a, 0x2f, 0x21, 0x22 };
static struct { W580LuzEstado ult; GString *hist; unsigned n; bool hay; } w580_luz;

static void w580_luz_estado(W580State *s, W580LuzEstado *e)
{
    uint8_t *r = s->i2c[0].regs[W580_PMU], *c = s->i2c[1].regs[0x40];
    memset(e, 0, sizeof(*e));
    e->panel = s->lcd && w580_panel_encendido(s->lcd);
    e->pmu43 = r[0x43]; e->pmu45 = r[0x45]; e->pmuA0 = r[0xa0]; e->pmuA1 = r[0xa1]; e->pmu36 = r[0x36]; e->pmu37 = r[0x37]; e->pmuB0 = r[0xb0];
    e->pwm_hab = w580_latch_get(s, 0xf9000032, 2); e->pwm_a = w580_latch_get(s, 0xf9000036, 2); e->pwm_b = w580_latch_get(s, 0xf900007c, 2);
    e->pwm_per = w580_latch_get(s, 0xf9000078, 2); e->ruta3 = w580_latch_get(s, 0xf9000048, 2);
    e->p3dat = w580_latch_get(s, 0xf900001e, 1); e->p3dir = w580_latch_get(s, 0xf9000020, 1);
    e->chip_hay = w580_i2c_presente(&s->i2c[1], 0x40);
    for (unsigned i = 0; i < 8; i++) { e->chip[i] = c[w580_luz_chip_reg[i]]; }
}
static bool w580_luz_pmu(const W580LuzEstado *e) { return (e->pmu43 & 0x80) && e->pmu45; }
static bool w580_luz_pwm_a(const W580LuzEstado *e) { return (e->pwm_hab & 4) && e->pwm_a && (e->p3dat & e->p3dir & 2); }
static bool w580_luz_pwm_b(const W580LuzEstado *e) { return (e->pwm_hab & 8) && e->pwm_b && (e->p3dat & e->p3dir & 4); }
/* chip 0x40 [observado en el emulador con el chip presente, bitácora E5]: la salida de «02 bits 6..7 + nivel 2F» es la ÚNICA luz
 * encendida mientras el original enseña el logo (2F = 0x84) y la que atenúa en reposo (0x2F -> 0x06): [inferido] la de la pantalla.
 * Las de «01 bits 0..1 + 09» y «01 bits 2..3 + 0A» entran al llegar al standby y se apagan a los ~26 s sin teclas: [inferido] teclado. */
static bool w580_luz_chip(const W580LuzEstado *e) { return e->chip_hay && (e->chip[0] & 8) && (e->chip[2] & 0xc0) && e->chip[5]; }
static bool w580_luz_chip_otras(const W580LuzEstado *e) { return e->chip_hay && (e->chip[0] & 8) && (((e->chip[1] & 3) && e->chip[3]) || ((e->chip[1] & 0xc) && e->chip[4])); }
#define W580_LUZ_REF_TXT "chip 0x40: 00 bit 3, 02 bits 6..7 y nivel 2F != 0 (0x84 en el logo, 0x2F en standby, 0x06 atenuada); PMU 43.7 = 0, PWM apagados"
#define W580_LUZ_REF(a, b, c, d, e) (d)
static void w580_luz_linea(GString *g, const W580LuzEstado *e)
{
    g_string_append_printf(g, "panel=%s | PMU 43=%02x 45=%02x -> %s | PWM hab(F9000032)=%04x A(36)=%04x B(7C)=%04x per(78)=%04x P3 dato=%02x dir=%02x ruta(48)=%04x -> A %s, B %s | "
                           "chip 0x40 %s 00=%02x 01=%02x 02=%02x 09=%02x 0A=%02x 2F=%02x 21=%02x 22=%02x -> luz 2F %s, luces 09/0A %s | PMU A0=%02x A1=%02x 36=%02x 37=%02x B0=%02x",
                           e->panel ? "encendido" : "dormido", e->pmu43, e->pmu45, w580_luz_pmu(e) ? "ENCENDIDA" : "apagada",
                           e->pwm_hab, e->pwm_a, e->pwm_b, e->pwm_per, e->p3dat, e->p3dir, e->ruta3, w580_luz_pwm_a(e) ? "ENCENDIDA" : "apagada", w580_luz_pwm_b(e) ? "ENCENDIDA" : "apagada",
                           e->chip_hay ? "presente" : "NO CONTESTA", e->chip[0], e->chip[1], e->chip[2], e->chip[3], e->chip[4], e->chip[5], e->chip[6], e->chip[7],
                           !e->chip_hay ? "[sin saber]" : w580_luz_chip(e) ? "ENCENDIDA" : "apagada", !e->chip_hay ? "[sin saber]" : w580_luz_chip_otras(e) ? "ENCENDIDAS" : "apagadas", e->pmuA0, e->pmuA1, e->pmu36, e->pmu37, e->pmuB0);
}
static void w580_leds_apunta(W580State *s);
static void w580_leds_json(W580State *s, GString *g);
/* se llama tras cada escritura que puede cambiar una luz o el panel; apunta el estado si ha cambiado */
static void w580_luz_muestra(W580State *s)
{
    W580LuzEstado e;
    if (!s->lcd) { return; }
    w580_leds_apunta(s);                        /* (pista S5) el historial de las seis luces, aparte */
    w580_luz_estado(s, &e);
    if (w580_luz.hay && !memcmp(&e, &w580_luz.ult, sizeof(e))) { return; }
    w580_luz.ult = e; w580_luz.hay = true; w580_luz.n++;
    if (!w580_luz.hist) { w580_luz.hist = g_string_new(NULL); }
    if (w580_luz.hist->len > 96 * 1024) { return; }
    g_string_append_printf(w580_luz.hist, "%9.3f s  ", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    w580_luz_linea(w580_luz.hist, &e);
    g_string_append_printf(w580_luz.hist, "  pc=%08x\n", s->cpu->env.regs[15]);
}
/* La firma del original con el standby en pantalla, medida en el emulador (bitácora E5): ver w580_get_luces. */
static char *w580_get_luces(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    W580LuzEstado e;
    GString *g = g_string_new(NULL);
    w580_luz_estado(s, &e);
    bool a = w580_luz_pmu(&e), b = w580_luz_pwm_a(&e), c = w580_luz_pwm_b(&e), d = w580_luz_chip(&e);
    g_string_append(g, "luces: "); w580_luz_linea(g, &e); g_string_append_c(g, '\n');
    g_string_append_printf(g, "veredicto: panel %s; luces encendidas segun el modelo: %s%s%s%s%s. ", e.panel ? "ENCENDIDO" : "DORMIDO",
                           a ? "PMU " : "", b ? "PWM_A " : "", c ? "PWM_B " : "", d ? "chip_0x40(2F) " : "", (a || b || c || d) ? "" : "NINGUNA");
#ifdef W580_LUZ_REF_TXT
    bool como = e.panel && W580_LUZ_REF(a, b, c, d, e);
    g_string_append_printf(g, "Como el original en standby (%s): %s. ", W580_LUZ_REF_TXT, como ? "SI" : "NO");
#endif
    g_string_append_printf(g, "Cual ilumina el cristal: [sin saber]%s\n", e.chip_hay ? "" : "; el chip 0x40 no contesta (propiedad i2c=0.0x4a:1.0x40 para que conteste)");
    g_string_append(g, "leds: "); w580_leds_json(s, g); g_string_append_c(g, '\n');       /* (pista S5) las seis luces, una a una */
    return g_string_free(g, false);
}
static char *w580_get_luces_historia(Object *obj, Error **errp)
{
    return g_strdup(w580_luz.hist ? w580_luz.hist->str : "");
}
/* ---- (pista S5) LAS SEIS LUCES, UNA A UNA: propiedades «luces-leds» (JSON) y «luces-leds-historia» -----------------
 * Añadido al indicador de E5 sin tocarlo. Qué es cada salida sale de leer el main original (pista L, docs/luces.md):
 *   pantalla      (ledid 0)  chip: 02 bits 6..7 + nivel 2F (0..0x84), con el elevador (00 bit 3)          [observado en el código]
 *   teclado_1     (ledid 3)  chip: 01 bits 0..1 + nivel 09 (0..0x10)                                       [inferido: teclado, zona 1]
 *   teclado_2     (ledid 2)  chip: 01 bits 2..3 + nivel 0A (0..0x10)                                       [inferido: teclado, zona 2]
 *   teclado_desl  (ledid 1)  los dos PWM del GPIO: A (F9000036, F9000032 bit 2, P3.1) y B (F900007C / 4)   [inferido, fuerte: teclado del deslizador]
 *   lateral_1/2   las dos ternas RGB: campos en 02 / 04 bits 0..5, niveles 0B..0D / 13..15 (R, G, B), con la bomba (00 bit 2)
 *                                                                                                          [inferido, fuerte: las luces laterales]
 * Un campo de terna a 1 = nivel fijo; a 2 = nivel x PWM del chip; a 3 = por el patrón (aquí, como fijo).
 * EL PWM DEL CHIP (registros 16 / 17) SE MODELA [inferido, docs/luces.md §2.1]: escribir 16 con el bit 1 arranca una
 * rampa de 0 a FF, con el bit 2 de FF a 0, y dura según los bits 3..5 (6 = 50 ms, 7 = 95, 4 = 100, 5 = 190, 2 = 500,
 * 3 = 950, 0 = 1000, 1 = 1900); sin bit de sentido el PWM vale lo que diga 17. NADA de esto se ha medido en un aparato:
 * el chip del emulador es un banco de registros; lo que se enseña es «cómo se vería si el chip es como se cree». */
static struct { int64_t t0; uint8_t sentido, vel; GString *hist; uint8_t ult[24]; bool hay; uint32_t n_w; } w580_leds;   /* n_w: escrituras I2C al chip (el coste) */
static const uint16_t w580_leds_rampa_ms[8] = { 1000, 1900, 500, 950, 100, 190, 50, 95 };
static const uint8_t w580_leds_reg[] = { 0x00, 0x01, 0x02, 0x04, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15, 0x16, 0x17, 0x2f, 0x23 };
static unsigned w580_leds_pwm(W580State *s)
{
    if (!w580_leds.sentido) { return s->i2c[1].regs[0x40][0x17]; }
    int64_t ms = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w580_leds.t0) / 1000000, d = w580_leds_rampa_ms[w580_leds.vel & 7];
    unsigned v = ms >= d ? 255 : (unsigned)(ms * 255 / d);
    return w580_leds.sentido == 1 ? v : 255 - v;
}
static void w580_leds_apunta(W580State *s)
{
    uint8_t *c = s->i2c[1].regs[0x40], e[24];
    W580LuzEstado z;
    unsigned n = 0;
    w580_luz_estado(s, &z);
    for (unsigned i = 0; i < G_N_ELEMENTS(w580_leds_reg); i++) { e[n++] = c[w580_leds_reg[i]]; }
    e[n++] = z.pwm_hab & 0xff; e[n++] = z.pwm_a & 0xff; e[n++] = z.pwm_b & 0xff; e[n++] = z.pwm_b >> 8; e[n++] = z.p3dat & z.p3dir & 6;
    while (n < sizeof(e)) { e[n++] = 0; }
    if (w580_leds.hay && !memcmp(e, w580_leds.ult, sizeof(e))) { return; }
    memcpy(w580_leds.ult, e, sizeof(e)); w580_leds.hay = true;
    if (!w580_leds.hist) { w580_leds.hist = g_string_new(NULL); }
    if (w580_leds.hist->len > 192 * 1024) { return; }
    g_string_append_printf(w580_leds.hist, "%9.3f s  00=%02x 01=%02x 02=%02x 04=%02x | 09=%02x 0A=%02x 2F=%02x | 0B..0D=%02x %02x %02x 13..15=%02x %02x %02x | 16=%02x 17=%02x 23=%02x | "
                           "PWM hab=%02x A=%02x B=%03x P3.1/2=%x\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9,
                           c[0x00], c[0x01], c[0x02], c[0x04], c[0x09], c[0x0a], c[0x2f], c[0x0b], c[0x0c], c[0x0d], c[0x13], c[0x14], c[0x15],
                           c[0x16], c[0x17], c[0x23], z.pwm_hab & 0xff, z.pwm_a & 0xff, z.pwm_b & 0x3ff, (z.p3dat & z.p3dir & 6) >> 1);
}
static void w580_luz_chip_w(W580State *s, uint8_t reg, uint8_t value)
{
    w580_leds.n_w++;
    if (reg == 0x16) {
        w580_leds.sentido = value & 2 ? 1 : value & 4 ? 2 : 0;
        w580_leds.vel = (value >> 3) & 7;
        if (w580_leds.sentido) { w580_leds.t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); }
    }
}
static void w580_leds_json(W580State *s, GString *g)
{
    uint8_t *c = s->i2c[1].regs[0x40];
    W580LuzEstado z;
    w580_luz_estado(s, &z);
    bool chip = z.chip_hay, elev = chip && (c[0] & 8), bomba = chip && (c[0] & 4);
    unsigned pwm = w580_leds_pwm(s);
    unsigned a = w580_luz_pwm_a(&z) ? z.pwm_a & 0xff : 0, b = w580_luz_pwm_b(&z) ? (z.pwm_b & 0x3ff) >> 2 : 0;
    g_string_append_printf(g, "{\"t_ms\":%" PRId64 ",\"chip\":%s,\"panel\":%s,\"elevador\":%s,\"bomba\":%s,", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000,
                           chip ? "true" : "false", z.panel ? "true" : "false", elev ? "true" : "false", bomba ? "true" : "false");
    g_string_append_printf(g, "\"pantalla\":%u,\"pantalla_max\":132,\"teclado_1\":%u,\"teclado_2\":%u,\"teclado_max\":16,\"teclado_desl\":%u,\"teclado_desl_b\":%u,\"teclado_desl_max\":255,",
                           elev && (c[2] & 0xc0) ? c[0x2f] : 0, elev && (c[1] & 3) ? c[0x09] : 0, elev && (c[1] & 0xc) ? c[0x0a] : 0, a, b);
    for (unsigned t = 0; t < 2; t++) {
        uint8_t campos = c[t ? 4 : 2], *niv = &c[t ? 0x13 : 0x0b];
        unsigned v[3];
        for (unsigned k = 0; k < 3; k++) {
            unsigned m = (campos >> (2 * k)) & 3;
            v[k] = !bomba || !m ? 0 : m == 2 ? niv[k] * pwm / 255 : niv[k];
        }
        g_string_append_printf(g, "\"lateral_%u\":[%u,%u,%u],\"lateral_%u_nivel\":[%u,%u,%u],\"lateral_%u_modo\":[%u,%u,%u],", t + 1, v[0], v[1], v[2],
                               t + 1, niv[0], niv[1], niv[2], t + 1, campos & 3, (campos >> 2) & 3, (campos >> 4) & 3);
    }
    g_string_append_printf(g, "\"pwm\":%u,\"pwm_sentido\":%u,\"pwm_vel\":%u,\"reg16\":%u,\"reg17\":%u,\"chip_escrituras\":%u,\"p26_forzado\":%d}", pwm, w580_leds.sentido, w580_leds.vel, c[0x16], c[0x17], w580_leds.n_w,
                           s->gpio_in_mask[2] & 0x40 ? (s->gpio_in_val[2] >> 6) & 1 : -1);
}
static char *w580_get_luces_leds(Object *obj, Error **errp)
{
    GString *g = g_string_new(NULL);
    w580_leds_json(W580_MACHINE(obj), g);
    return g_string_free(g, false);
}
static char *w580_get_luces_leds_historia(Object *obj, Error **errp)
{
    return g_strdup(w580_leds.hist ? w580_leds.hist->str : "");
}
static void w580_luz_class_init(ObjectClass *oc)
{
    object_class_property_add_str(oc, "luces-leds", w580_get_luces_leds, NULL);
    object_class_property_set_description(oc, "luces-leds", "Pista S5: las seis luces (pantalla, teclado 1 y 2, teclado del deslizador, las dos laterales RGB) con su nivel y color de AHORA, en JSON; el PWM del chip va modelado");
    object_class_property_add_str(oc, "luces-leds-historia", w580_get_luces_leds_historia, NULL);
    object_class_property_set_description(oc, "luces-leds-historia", "Pista S5: cada cambio de los registros de las seis luces (chip 0x40 y PWM del GPIO), con su tiempo virtual");
    object_class_property_add_str(oc, "luces", w580_get_luces, NULL);
    object_class_property_set_description(oc, "luces", "Pista E5: panel encendido y estado de las luces candidatas (PMU, PWM A/B, chip de LED 0x40 del I2C 1), con veredicto");
    object_class_property_add_str(oc, "luces-historia", w580_get_luces_historia, NULL);
    object_class_property_set_description(oc, "luces-historia", "Pista E5: cada cambio del estado de luces y panel, con su tiempo virtual");
}
