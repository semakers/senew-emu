/* Acelerómetro del W580i (pista E2). SPDX-License-Identifier: GPL-2.0-or-later
 * El sensor es ANALÓGICO de dos ejes: sus dos salidas van al ADC del PMU «Vincenne» (bus I2C 0, 0x4A):
 * canal 12 = X, canal 4 = Y, 8 bits (accel_read_xy 0x453FEFD4, pedometer_sample 0x453FECCC). Lo alimenta el
 * GPIO1, máscara 0x02 (dato en 0xF900000A, dirección en 0xF900000C: gpio_write 0x441A8290 usa
 * F9000000 + 10·puerto, DECIMAL).
 *
 * Lo que se modela:
 *   - valor fijo por eje (propiedades acel-x / acel-y = pmu-adc12 / pmu-adc4), escribible EN MARCHA por QMP;
 *   - una señal «t_ms,x,y» (propiedad acel-senal = ruta de un CSV) que se reproduce en TIEMPO VIRTUAL desde el
 *     instante en que se fija, con interpolación lineal; al acabar se queda en el último punto;
 *   - la puerta de alimentación: con el GPIO1 bit 1 a 0 (o sin poner como salida) la conversión da
 *     acel-apagado (0 por defecto) [inferido: una salida analógica sin alimentar cae a masa; el valor real, sin
 *     saber]. acel-puerta=off la quita;
 *   - un registro de cada conversión de los canales 4 y 12 (propiedad acel-registro = ruta de un TSV:
 *     t_virtual_ns, canal, valor, alimentado, ns desde el encendido): es lo que el firmware ha LEÍDO de verdad.
 * Lo que NO dice: la sensibilidad (cuentas por g), el cero real ni la orientación de los ejes. Eso es del aparato. */
static void w580_get_u8(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp);

typedef struct W580AcelPunto W580AcelPunto;

static bool w580_acel_alimentado(W580State *s)
{
    if (!s->acel_puerta) { return true; }
    return (w580_latch_get(s, 0xf900000a, 1) & 2) && (w580_latch_get(s, 0xf900000c, 1) & 2);
}
/* Se llama ANTES de que la escritura llegue al cerrojo: addr es 0xF900000A (dato) o 0xF900000C (dirección). */
static void w580_acel_gpio(W580State *s, hwaddr addr, uint64_t value)
{
    bool dato = addr == 0xf900000a;
    bool antes = (w580_latch_get(s, 0xf900000a, 1) & 2) && (w580_latch_get(s, 0xf900000c, 1) & 2);
    bool d = dato ? (value & 2) : (w580_latch_get(s, 0xf900000a, 1) & 2);
    bool o = dato ? (w580_latch_get(s, 0xf900000c, 1) & 2) : (value & 2);
    bool ahora = d && o;
    if (ahora == antes) { return; }
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (ahora) { s->acel_t_enc = t; s->acel_encendidos++; }
    else if (s->acel_t_enc >= 0) { s->acel_ns_encendido += t - s->acel_t_enc; }
    if (s->acel_log < 40) { s->acel_log++;
        qemu_log_mask(LOG_UNIMP, "W580_ACEL alimentacion=%u t=%" PRId64 " pc=0x%08x lr=0x%08x\n", ahora, t,
                      s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
}
static void w580_acel_senal_ahora(W580State *s, int64_t t, float *x, float *y)
{
    W580AcelPunto *p = s->acel_tabla;
    int64_t r = t - s->acel_t0;
    unsigned n = s->acel_n, i = s->acel_i;
    if (r <= p[0].t_ns) { *x = p[0].x; *y = p[0].y; return; }
    if (r >= p[n - 1].t_ns) { *x = p[n - 1].x; *y = p[n - 1].y; return; }
    if (i >= n - 1 || p[i].t_ns > r) { i = 0; }
    while (i + 1 < n - 1 && p[i + 1].t_ns <= r) { i++; }
    s->acel_i = i;
    int64_t dt = p[i + 1].t_ns - p[i].t_ns;
    float f = dt > 0 ? (float)(r - p[i].t_ns) / (float)dt : 0;
    *x = p[i].x + (p[i + 1].x - p[i].x) * f;
    *y = p[i].y + (p[i + 1].y - p[i].y) * f;
}
static uint8_t w580_acel_u8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }
/* Conversión del ADC del PMU para los canales del acelerómetro: canal 12 = X, 4 = Y. */
static uint8_t w580_acel_convertir(W580State *s, unsigned canal)
{
    unsigned e = canal == 12 ? 0 : 1;
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool alim = w580_acel_alimentado(s);
    uint8_t v;
    if (s->acel_tabla) {
        float x, y;
        w580_acel_senal_ahora(s, t, &x, &y);
        s->pmu_adc[12] = w580_acel_u8(x); s->pmu_adc[4] = w580_acel_u8(y);
    }
    v = alim ? s->pmu_adc[canal] : s->acel_apagado;
    s->acel_conv[e]++;
    if (!alim) { s->acel_conv_apagado[e]++; }
    int64_t desde = alim && s->acel_puerta && s->acel_t_enc >= 0 ? t - s->acel_t_enc : -1;
    if (desde >= 0 && (s->acel_min_asentado < 0 || desde < s->acel_min_asentado)) { s->acel_min_asentado = desde; }
    s->acel_ult[e] = v; s->acel_ult_t = t;
    if (s->acel_f) {
        fprintf(s->acel_f, "%" PRId64 "\t%u\t%u\t%u\t%" PRId64 "\n", t, canal, v, alim, desde);
        fflush(s->acel_f);
    }
    return v;
}
static void w580_acel_quitar_senal(W580State *s)
{
    g_free(s->acel_tabla); s->acel_tabla = NULL; s->acel_n = s->acel_i = 0;
}
static char *w580_get_acel_senal(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->acel_senal); }
static void w580_set_acel_senal(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    g_autofree char *txt = NULL;
    w580_acel_quitar_senal(s);
    g_free(s->acel_senal); s->acel_senal = g_strdup(value);
    if (!value || !*value) { return; }
    if (!g_file_get_contents(value, &txt, NULL, NULL)) { error_setg(errp, "acel-senal: no se puede leer %s", value); return; }
    g_auto(GStrv) l = g_strsplit(txt, "\n", -1);
    unsigned n = g_strv_length(l), k = 0;
    W580AcelPunto *p = g_new0(W580AcelPunto, n + 1);
    for (unsigned i = 0; i < n; i++) {
        char *c = l[i], *fin;
        while (*c == ' ' || *c == '\t') { c++; }
        if (!*c || *c == '#' || !(g_ascii_isdigit(*c) || *c == '-' || *c == '.')) { continue; }   /* comentario o cabecera */
        double t = g_ascii_strtod(c, &fin); if (*fin != ',' && *fin != ';' && *fin != '\t') { continue; }
        double x = g_ascii_strtod(fin + 1, &fin); if (*fin != ',' && *fin != ';' && *fin != '\t') { continue; }
        double y = g_ascii_strtod(fin + 1, &fin);
        if (k && (int64_t)(t * 1e6) < p[k - 1].t_ns) { g_free(p); error_setg(errp, "acel-senal: el tiempo retrocede en la linea %u", i + 1); return; }
        p[k].t_ns = (int64_t)(t * 1e6); p[k].x = x; p[k].y = y; k++;
    }
    if (!k) { g_free(p); error_setg(errp, "acel-senal: %s no tiene ninguna linea «t_ms,x,y»", value); return; }
    s->acel_tabla = p; s->acel_n = k; s->acel_i = 0;
    s->acel_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    qemu_log_mask(LOG_UNIMP, "W580_ACEL senal=%s puntos=%u dura_ms=%" PRId64 " t0=%" PRId64 "\n", value, k,
                  p[k - 1].t_ns / 1000000, s->acel_t0);
}
static char *w580_get_acel_registro(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->acel_registro); }
static void w580_set_acel_registro(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    if (s->acel_f) { fclose(s->acel_f); s->acel_f = NULL; }
    g_free(s->acel_registro); s->acel_registro = g_strdup(value);
    if (!value || !*value) { return; }
    s->acel_f = fopen(value, "w");
    if (!s->acel_f) { error_setg(errp, "acel-registro: no se puede escribir %s", value); return; }
    fprintf(s->acel_f, "t_virtual_ns\tcanal\tvalor\talimentado\tns_desde_encendido\n"); fflush(s->acel_f);
}
static void w580_get_acel_eje(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    unsigned canal = (uintptr_t)opaque;
    if (s->acel_tabla) {       /* con señal: el valor de AHORA */
        float x, y; w580_acel_senal_ahora(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &x, &y);
        s->pmu_adc[12] = w580_acel_u8(x); s->pmu_adc[4] = w580_acel_u8(y);
    }
    visit_type_uint8(v, name, &s->pmu_adc[canal], errp);
}
static void w580_set_acel_eje(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    unsigned canal = (uintptr_t)opaque;
    uint8_t valor;
    if (!visit_type_uint8(v, name, &valor, errp)) { return; }
    w580_acel_quitar_senal(s);            /* un valor fijo a mano cancela la señal en curso */
    s->pmu_adc[canal] = valor;
}
static bool w580_get_acel_puerta(Object *obj, Error **errp) { return W580_MACHINE(obj)->acel_puerta; }
static void w580_set_acel_puerta(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->acel_puerta = value; }
static char *w580_get_acel_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool alim = w580_acel_alimentado(s);
    int64_t enc = s->acel_ns_encendido + (alim && s->acel_puerta && s->acel_t_enc >= 0 ? t - s->acel_t_enc : 0);
    GString *g = g_string_new("");
    float x = s->pmu_adc[12], y = s->pmu_adc[4];
    if (s->acel_tabla) { w580_acel_senal_ahora(s, t, &x, &y); }
    g_string_append_printf(g, "acelerometro: X(canal 12)=%u Y(canal 4)=%u ahora; alimentado=%u (GPIO1 dato=0x%02x dir=0x%02x, puerta %s)\n",
                           w580_acel_u8(x), w580_acel_u8(y), alim, (unsigned)w580_latch_get(s, 0xf900000a, 1),
                           (unsigned)w580_latch_get(s, 0xf900000c, 1), s->acel_puerta ? "modelada" : "quitada");
    if (s->acel_tabla) {
        int64_t r = (t - s->acel_t0) / 1000000, dura = s->acel_tabla[s->acel_n - 1].t_ns / 1000000;
        g_string_append_printf(g, "senal: %s, %u puntos, %" PRId64 " de %" PRId64 " ms%s\n", s->acel_senal, s->acel_n,
                               r < dura ? r : dura, dura, r >= dura ? " (TERMINADA: se queda en el ultimo punto)" : "");
    } else {
        g_string_append(g, "senal: ninguna (valor fijo)\n");
    }
    g_string_append_printf(g, "conversiones: X=%" PRIu64 " (sin alimentar %" PRIu64 ") Y=%" PRIu64 " (sin alimentar %" PRIu64
                           "); ultimo X=%u Y=%u a t=%.3f s\n", s->acel_conv[0], s->acel_conv_apagado[0], s->acel_conv[1],
                           s->acel_conv_apagado[1], s->acel_ult[0], s->acel_ult[1], s->acel_ult_t / 1e9);
    g_string_append_printf(g, "alimentacion: %" PRIu64 " encendidos, %.3f s encendido en total, minimo entre encender y convertir = %.3f ms\n",
                           s->acel_encendidos, enc / 1e9, s->acel_min_asentado < 0 ? -1.0 : s->acel_min_asentado / 1e6);
    return g_string_free(g, false);
}
static void w580_acel_reset(W580State *s)
{
    s->acel_t_enc = -1; s->acel_min_asentado = -1; s->acel_ns_encendido = 0; s->acel_encendidos = 0;
    memset(s->acel_conv, 0, sizeof(s->acel_conv)); memset(s->acel_conv_apagado, 0, sizeof(s->acel_conv_apagado));
    s->acel_log = 0;
}
static void w580_acel_instance_init(W580State *s)
{
    s->acel_puerta = true; s->acel_apagado = 0; s->acel_t_enc = -1; s->acel_min_asentado = -1;
    s->pmu_adc[12] = 125; s->pmu_adc[4] = 125;       /* cero del sensor: el podómetro resta 0x7D (FUN_453ff08c) */
}
static void w580_acel_class_init(ObjectClass *oc)
{
    object_class_property_add(oc, "acel-x", "uint8", w580_get_acel_eje, w580_set_acel_eje, NULL, (void *)(uintptr_t)12);
    object_class_property_set_description(oc, "acel-x", "Acelerometro, eje X (canal 12 del ADC del PMU), 0..255; fijarlo cancela la senal");
    object_class_property_add(oc, "acel-y", "uint8", w580_get_acel_eje, w580_set_acel_eje, NULL, (void *)(uintptr_t)4);
    object_class_property_set_description(oc, "acel-y", "Acelerometro, eje Y (canal 4 del ADC del PMU), 0..255; fijarlo cancela la senal");
    object_class_property_add_str(oc, "acel-senal", w580_get_acel_senal, w580_set_acel_senal);
    object_class_property_set_description(oc, "acel-senal", "Ruta de un CSV t_ms,x,y que se reproduce en tiempo virtual desde ahora; vacio = ninguna");
    object_class_property_add_str(oc, "acel-registro", w580_get_acel_registro, w580_set_acel_registro);
    object_class_property_set_description(oc, "acel-registro", "Ruta de un TSV donde se apunta cada conversion de los canales 4 y 12; vacio = cerrar");
    object_class_property_add_bool(oc, "acel-puerta", w580_get_acel_puerta, w580_set_acel_puerta);
    object_class_property_set_description(oc, "acel-puerta", "on (por defecto): sin el GPIO1 bit 1 el sensor no da senal (lee acel-apagado)");
    object_class_property_add(oc, "acel-apagado", "uint8", w580_get_u8, w580_get_u8, NULL, (void *)offsetof(W580State, acel_apagado));
    object_class_property_set_description(oc, "acel-apagado", "Lo que da el ADC con el sensor sin alimentar (0 por defecto, inferido)");
    object_class_property_add_str(oc, "acel-estado", w580_get_acel_estado, NULL);
    object_class_property_set_description(oc, "acel-estado", "Valores de ahora, alimentacion, senal en curso y cuenta de conversiones");
}
