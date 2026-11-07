/* Pista E8: INYECCIÓN DE FALLOS en la máquina w580 (I²C, lecturas del PMU, teclado, apagado).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Para qué: la auditoría de seguridad del sistema propio (docs/auditoria_seguridad_s7.md) halló fallos que solo se
 * manifiestan cuando el I²C o el PMU FALLAN, y este emulador no sabía fallar. Nada de esto es «lo que hace el chip»:
 * son AVERÍAS A PROPÓSITO para ensayar lo que hace el firmware ante ellas. Sin tocar ninguna propiedad de aquí la
 * máquina hace EXACTAMENTE lo de antes. Todas valen al lanzar (-M) y en caliente (qom-set).
 *
 *   i2c-fallo=REGLA[/REGLA…]      hasta 8 reglas; «nada» o vacío las quita. Poner la propiedad SUSTITUYE las reglas y
 *                                 pone sus contadores a cero. REGLA = TIPO[:campo=valor]…
 *        TIPO   nack     el esclavo no contesta (NACK): la transacción sigue «terminando pasos» (el bit 3 de +2 llega),
 *                        lo que cambia es el estado (+6: 0x20 / 0x48 / 0x30), no se escribe nada y lo leído es 0xFF
 *               ocupado  el controlador se queda colgado: el paso NO termina (el bit 3 de +2 no llega y no hay IRQ 70/71)
 *                        hasta que el firmware empiece otra transacción (START) o mande STOP, o hasta que el cuelgue SE SUELTE
 *                        (vence el ms de su regla, o se cambia i2c-fallo): entonces el paso termina, tarde, como si nada
 *        campos bus=0|1 (0)   esc=0x4a (cualquiera)   reg=0x61 (cualquiera)   sent=e|l (escritura / lectura; cualquiera)
 *               n=N      solo las próximas N transacciones que casen (sin n ni ms: para siempre)
 *               ms=T     solo durante T ms VIRTUALES desde que se pone la propiedad (al lanzar: desde el reset)
 *               cada=K   falla 1 de cada K de las que casan (la K-ésima, la 2K-ésima…)
 *               salta=M  deja pasar bien las M primeras que casen
 *               fase=inicio|paso|parada   solo «ocupado»: dónde se cuelga. paso (por omisión) = en el paso en que la
 *                        regla casa; inicio = el START no sale (el bit 5 de +2 se queda a 1; solo mira bus);
 *                        parada = la transacción va bien entera y es el STOP el que no sale (el bit 4 se queda a 1)
 *        Dónde casa una regla (una transacción = de START a STOP, con START repetido incluido):
 *          sin reg ni sent → en la DIRECCIÓN (primer byte): el esclavo entero no contesta
 *          con reg y/o sent=e → en el byte de DATO que se escribe en ese registro (escritura que NO LLEGA)
 *          con reg y/o sent=l → en la dirección de LECTURA tras el START repetido (ya se sabe qué registro se lee)
 *   pmu-77-fijo=N | no            las lecturas de 0x75/0x76/0x77 dan siempre N (0..255); no / -1 = sin fijar
 *   pmu-6a-fijo=N | no            la conversión con 0x70 <- 0x6A (resultado en 0x71) da siempre N. Independientes
 *   apagado-falla=on|off|N        con apagado-real=on: PMU 0x41 <- 0 NO apaga (también SIN cargador). on = nunca;
 *                                 N = los N primeros intentos no apagan y el siguiente sí. Cada intento = una línea
 *                                 W580_APAGADO_NEGADO en el registro
 *   teclas-atascadas=CELDA[:CELDA…]   celdas pulsadas PARA SIEMPRE (30 = encendido a masa, 35 = línea ON del PMU): ni
 *                                 kp-cell ni teclas-arranque-ms las sueltan. «nada» o vacío las suelta
 *   kp-foto=nivel|irq|rearme|clavada   qué son los bits 27..31 de FB010000 («filas en el momento de la interrupción»):
 *        nivel   (por omisión, lo de siempre) el estado de AHORA de las teclas a masa
 *        irq     una FOTO: se toma en el reset y cada vez que el teclado da su interrupción (cambio de teclas con el
 *                bit 9 de +0 a 1); entre fotos NO cambia aunque la tecla se suelte
 *        rearme  como irq, pero solo hay foto nueva si el firmware rearmó (escribió el bit 8 a 1) después de la anterior
 *                (en este modo el bit 8 se LEE a 0: si se leyera a 1, como en los demás modos, cualquier «+0 |= otro bit» rearmaría)
 *        clavada la foto del reset no cambia nunca
 *        [sin saber] cuál es el aparato: el original SOLO lee esos bits dentro de su rutina de interrupción
 *        (FUN_441acfc8 manda +0 y +4 crudos al proceso; keypad_decode_signal 0x441ACF3C usa +0 >> 27), es decir, justo
 *        cuando foto y nivel coinciden; fuera de la interrupción barre a mano y lee los bits 22..26 (keypad_scan_manual
 *        0x441ACE70). Los bits 22..26 de este modelo son SIEMPRE el nivel de ahora.
 *   fallos-estado (lectura)       una línea por regla con sus contadores, y lo demás
 * Cada fallo inyectado deja una línea W580_FALLO en el registro (las 400 primeras). Poner i2c-fallo en caliente da además
 * otras 3000 líneas W580_I2C (el tope de siempre) desde ese instante, para ver los reintentos del firmware. */
enum { W580_FA_NACK = 1, W580_FA_OCUPADO = 2 };                 /* tipo de regla, y modo de la transacción en curso */
enum { W580_FF_PASO, W580_FF_INICIO, W580_FF_PARADA };
enum { W580_FS_CUALQUIERA, W580_FS_E, W580_FS_L };
enum { W580_FP_INICIO, W580_FP_DIR, W580_FP_DATO, W580_FP_DIR_L };   /* punto de decisión */
enum { W580_KF_NIVEL, W580_KF_IRQ, W580_KF_REARME, W580_KF_CLAVADA };
static const char *const w580_fa_tecla_nombre[] = { "apaga", "reinicia", "nada" };
#define W580_PMU_RESET_FRIO_OMISION false      /* pmu-reset por omisión: caliente (P1-M1 de AUD4) */
static void w580_pmu_tecla(W580State *s, bool abajo);
static void w580_i2c_suelta(W580I2C *b, const char *por);
static int64_t w580_fa_ahora(void) { return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); }
static void G_GNUC_PRINTF(2, 3) w580_fa_log(W580State *s, const char *fmt, ...)
{
    if (s->fa.log >= 400) { return; }
    s->fa.log++;
    va_list ap; va_start(ap, fmt);
    g_autofree char *t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_UNIMP, "W580_FALLO %s t=%.3f s pc=0x%08x lr=0x%08x\n", t, w580_fa_ahora() / 1e9,
                  current_cpu ? s->cpu->env.regs[15] : 0, current_cpu ? s->cpu->env.regs[14] : 0);
    w580_es_suceso("fallo", "%s", t);
}
/* ---------------------------------------------------------------- I²C */
static void w580_fa_i2c_empieza(W580I2C *b) { b->fa_modo = 0; b->fa_vistas = 0; b->fa_parada = false; b->fa_primera = true; }
/* ¿Dispara alguna regla en este punto de la transacción? 0 = no; W580_FA_NACK; W580_FA_OCUPADO. Las de fase=parada
 * no devuelven nada: dejan apuntado que el STOP de esta transacción se colgará. */
static int w580_fa_i2c(W580I2C *b, int punto)
{
    W580State *s = b->s;
    struct W580Fallos *f = &s->fa;
    int64_t t = 0;
    if (b->fa_exento) { return 0; }              /* un paso que estuvo colgado y se suelta: se ejecuta sin volver a mirar las reglas */
    for (unsigned i = 0; i < f->nr; i++) {
        struct W580FalloI2C *r = &f->r[i];
        if (r->bus != (int)b->idx || (b->fa_vistas >> i & 1)) { continue; }
        bool filtro = r->reg >= 0 || r->sent != W580_FS_CUALQUIERA;
        if (r->fase == W580_FF_INICIO) { if (punto != W580_FP_INICIO) { continue; } }
        else if (punto == W580_FP_INICIO) { continue; }
        else {
            if (r->esc >= 0 && r->esc != b->slave) { continue; }
            if (punto == W580_FP_DIR) { if (filtro) { continue; } }
            else if (punto == W580_FP_DATO) { if (!filtro || r->sent == W580_FS_L || (r->reg >= 0 && r->reg != b->reg)) { continue; } }
            else {      /* dirección de lectura */
                if (!filtro) { if (!b->fa_primera) { continue; } }       /* una lectura sin puntero: es su primer byte */
                else if (r->sent == W580_FS_E || (r->reg >= 0 && r->reg != b->reg)) { continue; }
            }
        }
        b->fa_vistas |= 1u << i;                 /* casa: esta transacción cuenta una vez para esta regla */
        if (r->ms) { if (!t) { t = w580_fa_ahora(); } if (t > r->hasta_ns) { continue; } }
        r->vistos++;
        if ((int64_t)r->vistos <= r->salta) { continue; }
        if (r->cada > 1 && ((int64_t)r->vistos - r->salta) % r->cada) { continue; }
        if (r->n >= 0 && (int64_t)r->hechos >= r->n) { continue; }
        r->hechos++;
        b->fa_hasta = r->tipo == W580_FA_OCUPADO && r->ms ? r->hasta_ns : 0;      /* un cuelgue de una regla con ms se suelta solo al vencer */
        if (r->fase == W580_FF_PARADA) { b->fa_parada = true; continue; }
        if (r->tipo == W580_FA_NACK) { f->n_nack++; } else { f->n_colgado++; }
        w580_fa_log(s, "I2C%u regla %u %s%s: esclavo=0x%02x reg=0x%02x %s (van %" PRIu64 " de %" PRIu64 " que casan)", b->idx, i,
                    r->tipo == W580_FA_NACK ? "NACK" : "OCUPADO", r->fase == W580_FF_INICIO ? " en el START" : "",
                    punto == W580_FP_INICIO ? 0 : b->slave, punto == W580_FP_DATO || (punto == W580_FP_DIR_L && !b->fa_primera) ? b->reg : 0,
                    punto == W580_FP_INICIO ? "START" : punto == W580_FP_DIR ? "direccion" : punto == W580_FP_DATO ? "escritura que no llega" : "lectura",
                    r->hechos, r->vistos);
        return r->tipo;
    }
    return 0;
}
/* ---------------------------------------------------------------- apagado */
static bool w580_fa_deja_apagar(W580State *s)       /* lo pregunta w580_apagado_mirar con las condiciones del apagado ya cumplidas */
{
    struct W580Fallos *f = &s->fa;
    if (f->apaga_tecla && (s->i2c[0].regs[0x4a][0x23] & 1)) {      /* P1-M4 de AUD4: apagar con la tecla de encendido TODAVÍA pulsada */
        f->apaga_tecla_n++; s->pmu_41_cero = false;
        qemu_log_mask(LOG_UNIMP, "W580_APAGADO_NEGADO PMU 0x41 <- 0 con la tecla de encendido pulsada (23 bit 0): apaga-con-tecla=%s -> %s pc=0x%08x t=%.3f s\n",
                      f->apaga_tecla == 1 ? "reinicia" : "nada", f->apaga_tecla == 1 ? "la maquina se REINICIA en frio (se apaga y la tecla la vuelve a encender)" : "no pasa nada, sigue encendida",
                      current_cpu ? s->cpu->env.regs[15] : 0, w580_fa_ahora() / 1e9);
        if (f->apaga_tecla == 1) { f->pmu_arrancado = false; qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET); }
        return false;
    }
    if (!f->apagado_falla) { return true; }
    if (f->apagado_falla > 0) { f->apagado_falla--; }
    f->apagado_negados++;
    s->pmu_41_cero = false;                         /* el intento se gasta: para apagar hay que volver a escribir 0x41 <- 0 */
    qemu_log_mask(LOG_UNIMP, "W580_APAGADO_NEGADO PMU 0x41 <- 0: NO apaga por apagado-falla (fallo inyectado, pista E8); intento negado n.o %" PRIu64
                  ", quedan %" PRId64 " por negar (-1 = todos) pc=0x%08x t=%.3f s\n", f->apagado_negados, f->apagado_falla,
                  current_cpu ? s->cpu->env.regs[15] : 0, w580_fa_ahora() / 1e9);
    w580_es_suceso("fallo", "apagado negado n=%" PRIu64, f->apagado_negados);
    return false;
}
/* ---------------------------------------------------------------- teclado */
static uint8_t w580_fa_kp_masa(W580State *s) { return s->kp_masa | s->fa.kp_at_masa; }
static uint8_t w580_fa_kp_col(W580State *s, unsigned c) { return s->kp_cols[c] | s->fa.kp_at_cols[c]; }
/* bits 27..31 de FB010000: máscara de teclas a masa PULSADAS que ve el firmware */
static uint8_t w580_fa_kp_foto(W580State *s) { return s->fa.kp_foto_modo == W580_KF_NIVEL ? w580_fa_kp_masa(s) : s->fa.kp_foto; }
static void w580_fa_kp_suceso(W580State *s)         /* el teclado da (o daría) su interrupción: w580_kp_debounce */
{
    struct W580Fallos *f = &s->fa;
    if (f->kp_foto_modo == W580_KF_NIVEL || f->kp_foto_modo == W580_KF_CLAVADA || !(s->kp_control & 0x200)) { return; }
    if (f->kp_foto_modo == W580_KF_REARME) { if (!f->kp_foto_armada) { return; } f->kp_foto_armada = false; }
    f->kp_foto = w580_fa_kp_masa(s); f->kp_fotos++;
}
static void w580_fa_kp_escribe(W580State *s, uint32_t value) { if (value & 0x100) { s->fa.kp_foto_armada = true; } }
static void w580_fa_kp_reset(W580State *s)          /* tras teclas-arranque: la foto del reset */
{
    s->fa.kp_foto = w580_fa_kp_masa(s); s->fa.kp_foto_armada = true;
    if (s->fa.kp_at_on) { w580_pmu_tecla(s, true); }
}
static bool w580_fa_kp_pon(W580State *s, const char *txt, bool aplicar, Error **errp)
{
    uint8_t cols[6] = { 0 }, masa = 0; bool on = false;
    if (txt && *txt && strcmp(txt, "nada") && strcmp(txt, "no")) {
        g_auto(GStrv) v = g_strsplit(txt, ":", -1);
        for (unsigned i = 0; v[i]; i++) {
            char *fin = NULL; unsigned long c = strtoul(v[i], &fin, 0);
            if (!*v[i] || *fin || c > 35) { error_setg(errp, "teclas-atascadas: celdas 0..35 separadas por ':' (30 = encendido a masa, 35 = ON del PMU), o nada"); return false; }
            if (c == 35) { on = true; } else if (c >= 30) { masa |= 1u << (c - 30); } else { cols[c / 5] |= 1u << (c % 5); }
        }
    }
    if (!aplicar) { return true; }
    memcpy(s->fa.kp_at_cols, cols, 6); s->fa.kp_at_masa = masa;
    bool antes = s->fa.kp_at_on; s->fa.kp_at_on = on;
    if (s->cpu) {
        if (on != antes) { w580_pmu_tecla(s, on); }
        timer_mod(s->kp_timer, w580_fa_ahora() + 5000000);
        w580_fa_log(s, "teclas-atascadas=%s", txt && *txt ? txt : "nada");
    }
    return true;
}
/* ---------------------------------------------------------------- propiedades */
static char *w580_fa_get_i2c(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->fa.i2c_txt ?: ""); }
static void w580_fa_set_i2c(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580FalloI2C nr[8]; unsigned n = 0;
    int64_t ahora = s->cpu ? w580_fa_ahora() : 0;
    if (*value && strcmp(value, "nada") && strcmp(value, "no")) {
        g_auto(GStrv) reglas = g_strsplit(value, "/", -1);
        for (unsigned i = 0; reglas[i]; i++) {
            if (!*reglas[i]) { continue; }
            if (n == 8) { error_setg(errp, "i2c-fallo: como mucho 8 reglas"); return; }
            struct W580FalloI2C r = { .bus = 0, .esc = -1, .reg = -1, .n = -1 };
            g_auto(GStrv) c = g_strsplit(reglas[i], ":", -1);
            if (!strcmp(c[0], "nack")) { r.tipo = W580_FA_NACK; }
            else if (!strcmp(c[0], "ocupado")) { r.tipo = W580_FA_OCUPADO; }
            else { error_setg(errp, "i2c-fallo: «%s»: el tipo es nack u ocupado", c[0]); return; }
            for (unsigned j = 1; c[j]; j++) {
                char *ig = strchr(c[j], '='), *fin = NULL;
                if (!ig || !ig[1]) { error_setg(errp, "i2c-fallo: «%s»: se espera campo=valor", c[j]); return; }
                *ig++ = 0;
                if (!strcmp(c[j], "sent")) {
                    if (!strcmp(ig, "e")) { r.sent = W580_FS_E; } else if (!strcmp(ig, "l")) { r.sent = W580_FS_L; }
                    else { error_setg(errp, "i2c-fallo: sent=e (escritura) o sent=l (lectura)"); return; }
                    continue;
                }
                if (!strcmp(c[j], "fase")) {
                    if (!strcmp(ig, "paso")) { r.fase = W580_FF_PASO; } else if (!strcmp(ig, "inicio")) { r.fase = W580_FF_INICIO; }
                    else if (!strcmp(ig, "parada")) { r.fase = W580_FF_PARADA; }
                    else { error_setg(errp, "i2c-fallo: fase=inicio, paso o parada"); return; }
                    continue;
                }
                long long x = g_ascii_strtoll(ig, &fin, 0);
                if (*fin || x < 0) { error_setg(errp, "i2c-fallo: %s=«%s» no es un número", c[j], ig); return; }
                if (!strcmp(c[j], "bus") && x <= 1) { r.bus = x; }
                else if (!strcmp(c[j], "esc") && x <= 127) { r.esc = x; }
                else if (!strcmp(c[j], "reg") && x <= 255) { r.reg = x; }
                else if (!strcmp(c[j], "n") && x >= 1) { r.n = x; }
                else if (!strcmp(c[j], "ms") && x >= 1) { r.ms = x; r.hasta_ns = ahora + x * INT64_C(1000000); }
                else if (!strcmp(c[j], "cada") && x >= 1) { r.cada = x; }
                else if (!strcmp(c[j], "salta")) { r.salta = x; }
                else { error_setg(errp, "i2c-fallo: campo «%s» desconocido o fuera de rango (bus esc reg sent n ms cada salta fase)", c[j]); return; }
            }
            if (r.fase != W580_FF_PASO && r.tipo != W580_FA_OCUPADO) { error_setg(errp, "i2c-fallo: fase= solo vale con ocupado"); return; }
            if (r.fase == W580_FF_INICIO && (r.esc >= 0 || r.reg >= 0 || r.sent)) { error_setg(errp, "i2c-fallo: con fase=inicio solo se puede filtrar por bus (en el START aún no se sabe a quién se llama)"); return; }
            nr[n++] = r;
        }
    }
    memcpy(s->fa.r, nr, sizeof(nr[0]) * n); s->fa.nr = n;
    if (s->cpu) { for (unsigned i = 0; i < 2; i++) { w580_i2c_suelta(&s->i2c[i], "se cambia i2c-fallo"); } }      /* lo que estuviera colgado termina ahora */
    g_free(s->fa.i2c_txt); s->fa.i2c_txt = g_strdup(n ? value : "");
    if (s->cpu) {
        if (n) { s->i2c_log = 0; }               /* otras 3000 líneas W580_I2C desde aquí: para ver qué hace el firmware tras el fallo */
        w580_fa_log(s, "i2c-fallo=%s (%u reglas)", n ? value : "nada", n);
    }
}
/* número, «no»/«off»/«nada» (= quitar, -1) u «on» (= on_vale) desde -M (texto), qom-set con JSON o los guiones de la pista S */
static bool w580_fa_visita(Visitor *v, const char *name, int64_t *out, int64_t on_vale, Error **errp)
{
    QObject *q = NULL;
    if (!visit_type_any(v, name, &q, errp)) { return false; }
    QBool *qb = qobject_to(QBool, q); QNum *qn = qobject_to(QNum, q); QString *qs = qobject_to(QString, q);
    bool ok = true;
    if (qb) { *out = qbool_get_bool(qb) ? on_vale : -1; }
    else if (qn) { *out = (int64_t)qnum_get_double(qn); }
    else if (qs) {
        const char *t = qstring_get_str(qs); char *fin = NULL;
        if (!g_ascii_strcasecmp(t, "no") || !g_ascii_strcasecmp(t, "off") || !g_ascii_strcasecmp(t, "nada") || !*t) { *out = -1; }
        else if (!g_ascii_strcasecmp(t, "on") || !g_ascii_strcasecmp(t, "si")) { *out = on_vale; }
        else { *out = g_ascii_strtoll(t, &fin, 0); if (*fin) { error_setg(errp, "%s: «%s» no es un número, on ni no", name, t); ok = false; } }
    } else { error_setg(errp, "%s: se espera un número, on o no", name); ok = false; }
    qobject_unref(q);
    return ok;
}
enum { W580_FAP_77, W580_FAP_6A, W580_FAP_APAGADO };
static void w580_fa_get_n(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    struct W580Fallos *f = &W580_MACHINE(obj)->fa;
    int64_t x = (uintptr_t)opaque == W580_FAP_77 ? f->pmu77 : (uintptr_t)opaque == W580_FAP_6A ? f->pmu6a : f->apagado_falla;
    visit_type_int(v, name, &x, errp);
}
static void w580_fa_set_n(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580Fallos *f = &s->fa;
    int64_t x;
    if ((uintptr_t)opaque == W580_FAP_APAGADO) {
        if (!w580_fa_visita(v, name, &x, -2, errp)) { return; }
        if (x < -2) { error_setg(errp, "apagado-falla: on, off o cuántos intentos fallan"); return; }
        f->apagado_falla = x == -2 ? -1 : x == -1 ? 0 : x;        /* on = siempre (-1 por dentro); no/off = 0 */
    } else {
        if (!w580_fa_visita(v, name, &x, -3, errp)) { return; }
        if (x < -1 || x > 255) { error_setg(errp, "%s: de 0 a 255, o no (-1) para quitarlo", name); return; }
        if ((uintptr_t)opaque == W580_FAP_77) { f->pmu77 = x; } else { f->pmu6a = x; }
    }
    if (s->cpu) { w580_fa_log(s, "%s=%" PRId64, name, x); }
}
static char *w580_fa_get_kp(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->fa.kp_at_txt ?: ""); }
static void w580_fa_set_kp(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    if (!w580_fa_kp_pon(s, value, false, errp)) { return; }
    w580_fa_kp_pon(s, value, true, errp);
    g_free(s->fa.kp_at_txt); s->fa.kp_at_txt = g_strdup(!strcmp(value, "nada") || !strcmp(value, "no") ? "" : value);
}
static const char *const w580_fa_foto_nombre[] = { "nivel", "irq", "rearme", "clavada" };
static char *w580_fa_get_foto(Object *obj, Error **errp) { return g_strdup(w580_fa_foto_nombre[W580_MACHINE(obj)->fa.kp_foto_modo]); }
static void w580_fa_set_foto(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    for (unsigned i = 0; i < 4; i++) {
        if (strcmp(value, w580_fa_foto_nombre[i])) { continue; }
        if (s->cpu && s->fa.kp_foto_modo == W580_KF_NIVEL && i != W580_KF_NIVEL) { s->fa.kp_foto = w580_fa_kp_masa(s); s->fa.kp_foto_armada = true; }   /* en caliente: la foto de ahora */
        s->fa.kp_foto_modo = i;
        if (s->cpu) { w580_fa_log(s, "kp-foto=%s", value); }
        return;
    }
    error_setg(errp, "kp-foto: nivel | irq | rearme | clavada");
}
static char *w580_fa_get_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580Fallos *f = &s->fa;
    GString *g = g_string_new(NULL);
    int64_t t = w580_fa_ahora();
    g_string_append_printf(g, "t=%.2f s | i2c: %u reglas, %" PRIu64 " NACK y %" PRIu64 " cuelgues inyectados | pmu-77-fijo=%d pmu-6a-fijo=%d (%" PRIu64 " lecturas fijadas) | "
                           "apagado-falla=%" PRId64 " (%" PRIu64 " negados) | apaga-con-tecla=%s (%" PRIu64 ") | pmu-reset=%s (%" PRIu64 " calientes) | teclas-atascadas=%s | kp-foto=%s (foto=%02x ahora=%02x, %" PRIu64 " fotos, %s)\n",
                           t / 1e9, f->nr, f->n_nack, f->n_colgado, f->pmu77, f->pmu6a, f->n_fijo, f->apagado_falla, f->apagado_negados,
                           w580_fa_tecla_nombre[f->apaga_tecla], f->apaga_tecla_n, f->pmu_frio ? "frio" : "caliente", f->pmu_calientes,
                           f->kp_at_txt && *f->kp_at_txt ? f->kp_at_txt : "nada", w580_fa_foto_nombre[f->kp_foto_modo], w580_fa_kp_foto(s), w580_fa_kp_masa(s),
                           f->kp_fotos, f->kp_foto_armada ? "armada" : "sin rearmar");
    for (unsigned i = 0; i < f->nr; i++) {
        struct W580FalloI2C *r = &f->r[i];
        g_string_append_printf(g, "  regla %u: %s%s bus=%d", i, r->tipo == W580_FA_NACK ? "nack" : "ocupado",
                               r->fase == W580_FF_INICIO ? " fase=inicio" : r->fase == W580_FF_PARADA ? " fase=parada" : "", r->bus);
        if (r->esc >= 0) { g_string_append_printf(g, " esc=0x%02x", r->esc); }
        if (r->reg >= 0) { g_string_append_printf(g, " reg=0x%02x", r->reg); }
        if (r->sent) { g_string_append_printf(g, " sent=%s", r->sent == W580_FS_E ? "e" : "l"); }
        if (r->n >= 0) { g_string_append_printf(g, " n=%" PRId64, r->n); }
        if (r->ms) { g_string_append_printf(g, " ms=%" PRId64 " (%s)", r->ms, t > r->hasta_ns ? "VENCIDA" : "viva"); }
        if (r->cada > 1) { g_string_append_printf(g, " cada=%" PRId64, r->cada); }
        if (r->salta) { g_string_append_printf(g, " salta=%" PRId64, r->salta); }
        g_string_append_printf(g, " | han casado %" PRIu64 " transacciones, inyectados %" PRIu64 "%s\n", r->vistos, r->hechos,
                               r->n >= 0 && (int64_t)r->hechos >= r->n ? " (AGOTADA)" : "");
    }
    for (unsigned i = 0; i < 2; i++) {
        W580I2C *b = &s->i2c[i];
        if (b->fa_modo || b->fa_pegado) {
            g_string_append_printf(g, "  bus %u AHORA: %s%s\n", i, b->fa_modo == W580_FA_OCUPADO ? "COLGADO" : "esclavo mudo hasta el STOP",
                                   b->fa_pegado == 0x20 ? " (START sin salir)" : b->fa_pegado == 0x10 ? " (STOP sin salir)" : "");
        }
    }
    return g_string_free(g, false);
}
static char *w580_fa_get_tecla(Object *obj, Error **errp) { return g_strdup(w580_fa_tecla_nombre[W580_MACHINE(obj)->fa.apaga_tecla]); }
static void w580_fa_set_tecla(Object *obj, const char *value, Error **errp)
{
    for (unsigned i = 0; i < 3; i++) { if (!strcmp(value, w580_fa_tecla_nombre[i])) { W580_MACHINE(obj)->fa.apaga_tecla = i; return; } }
    error_setg(errp, "apaga-con-tecla: apaga | reinicia | nada");
}
static char *w580_fa_get_pmu_reset(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->fa.pmu_frio ? "frio" : "caliente"); }
static void w580_fa_set_pmu_reset(Object *obj, const char *value, Error **errp)
{
    if (!strcmp(value, "frio")) { W580_MACHINE(obj)->fa.pmu_frio = true; }
    else if (!strcmp(value, "caliente")) { W580_MACHINE(obj)->fa.pmu_frio = false; }
    else { error_setg(errp, "pmu-reset: frio | caliente"); }
}
static void w580_fa_instance_init(W580State *s) { s->fa.pmu77 = s->fa.pmu6a = -1; s->fa.pmu_frio = W580_PMU_RESET_FRIO_OMISION; }
static void w580_fa_class_init(ObjectClass *oc)
{
    object_class_property_add_str(oc, "i2c-fallo", w580_fa_get_i2c, w580_fa_set_i2c);
    object_class_property_set_description(oc, "i2c-fallo", "Pista E8: fallos de I2C inyectados: REGLA[/REGLA], REGLA = nack|ocupado[:bus=N][:esc=0x4a][:reg=0x61][:sent=e|l][:n=N][:ms=T][:cada=K][:salta=M][:fase=inicio|paso|parada]; nada los quita");
    object_class_property_add(oc, "pmu-77-fijo", "int", w580_fa_get_n, w580_fa_set_n, NULL, (void *)(uintptr_t)W580_FAP_77);
    object_class_property_set_description(oc, "pmu-77-fijo", "Pista E8: las lecturas de PMU 0x75..0x77 dan siempre este valor (0..255); no o -1 = sin fijar");
    object_class_property_add(oc, "pmu-6a-fijo", "int", w580_fa_get_n, w580_fa_set_n, NULL, (void *)(uintptr_t)W580_FAP_6A);
    object_class_property_set_description(oc, "pmu-6a-fijo", "Pista E8: la conversion con PMU 0x70 <- 0x6A da siempre este valor (0..255); no o -1 = sin fijar");
    object_class_property_add(oc, "apagado-falla", "int", w580_fa_get_n, w580_fa_set_n, NULL, (void *)(uintptr_t)W580_FAP_APAGADO);
    object_class_property_set_description(oc, "apagado-falla", "Pista E8: con apagado-real=on, PMU 0x41 <- 0 NO apaga tampoco sin cargador: on = nunca (se lee -1), N = los N primeros intentos, off = 0");
    object_class_property_add_str(oc, "teclas-atascadas", w580_fa_get_kp, w580_fa_set_kp);
    object_class_property_set_description(oc, "teclas-atascadas", "Pista E8: celdas pulsadas para siempre (CELDA[:CELDA]; 30 = encendido a masa, 35 = ON del PMU); nada las suelta");
    object_class_property_add_str(oc, "kp-foto", w580_fa_get_foto, w580_fa_set_foto);
    object_class_property_set_description(oc, "kp-foto", "Pista E8: bits 27..31 de FB010000: nivel (omision) | irq (foto en cada interrupcion del teclado) | rearme (ademas exige el bit 8) | clavada (la del reset)");
    object_class_property_add_str(oc, "apaga-con-tecla", w580_fa_get_tecla, w580_fa_set_tecla);
    object_class_property_set_description(oc, "apaga-con-tecla", "Pista E8: con apagado-real=on, que hace PMU 0x41 <- 0 con la tecla de encendido aun pulsada (23 bit 0): apaga (omision) | reinicia | nada");
    object_class_property_add_str(oc, "pmu-reset", w580_fa_get_pmu_reset, w580_fa_set_pmu_reset);
    object_class_property_set_description(oc, "pmu-reset", "Pista E8: en un reset de la maquina que no es el primero, el PMU conserva sus registros (caliente) o se reinicia entero como antes (frio)");
    object_class_property_add_str(oc, "fallos-estado", w580_fa_get_estado, NULL);
    object_class_property_set_description(oc, "fallos-estado", "Pista E8: reglas de fallo y contadores");
}
