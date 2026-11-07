/* Pista E7: una batería y un cargador para el PMU emulado, y la regla [carga] del modo estricto.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Es SEGURIDAD (una batería de litio y un cargador): sirve para ENSAYAR en el simulador la política «no cargar
 * nunca» del sistema propio (docs/bateria.md §8.A) antes de tocar un aparato. NO es el chip: del PMU de verdad no hay
 * hoja; lo que se modela es CÓMO LO USA el firmware original (docs/bateria.md §2, pista B), y donde no se sabe qué
 * hace el chip hay un PARÁMETRO en vez de una respuesta (el bit 7 de 0x60, apagar con cargador).
 * Sin tocar ninguna propiedad de aquí la máquina hace EXACTAMENTE lo de antes (batería quieta de 108 cuentas, sin
 * cargador, 0x23 = 0 salvo la tecla).
 *
 * Entradas del aparato, movibles en caliente (qom-set) y al lanzar (-M):
 *   pmu-vbat-mv=N        tensión de la batería en mV. 0x75/0x76/0x77 y el ADC 10: cuenta = (mV − 2755)/10,843 (recta por
 *                        defecto del original en marcha); la lectura con 0x70 <- 0x6A: (mV − 2796)/10,784 (recta fija del
 *                        arranque temprano). 0 = sin poner: las cuentas de siempre (pmu-vbat, pmu-adc10 = 108)
 *   descarga-mv-por-s=X  la batería baja X mV por segundo VIRTUAL (admite decimales) mientras no le entre corriente
 *   carga-mv-por-s=X     y sube X mV/s mientras el modelo ve corriente entrando, hasta el techo del código de 0x60
 *   cargador=on|off      0x23 bit 3; al cambiar, suceso 0x21 bit 3 e IRQ 37 (según la máscara 0x26). cargador-mv=N (5000):
 *                        el ADC 8 da N/25 mientras hay cargador
 *   usb-vbus=on|off      0x23 bit 7; al cambiar, suceso 0x21 bit 6 («usb» a secas ya es una propiedad de toda máquina de QEMU)
 *   bateria-presente=on|off   0x23 bit 6 (a 1 = quitada). [sin saber] si hay suceso: no se levanta ninguno
 *   apaga-con-cargador=on|off (off) con apagado-real=on: ¿0x41 <- 0 apaga con cargador o USB puestos? off = NO apaga
 *                        (lo [inferido]: el original no lo intenta nunca, reinicia) y se queda pendiente: apaga al quitarlo
 *   carga-bit7=nada|corta:N|apaga:N   qué hace el bit 7 de 0x60 (el original lo reescribe cada 20,5 s): nada (por omisión);
 *                        a los N s de la última escritura con el bit a 1 el chip lo baja y deja de cargar; o apaga la máquina
 * Salida: carga-estado (lectura): una línea con todo, y si el modelo ve «corriente entrando a la batería».
 *
 * «Corriente entrando» en este modelo = batería presente Y ((cargador Y 0x61 bits 0..3 ≠ 0) O (USB Y 0x63 bits 4..5 ≠ 0))
 * Y, solo en el modo corta:N, el bit 7 de 0x60 vivo. Es el caso PESIMISTA de docs/bateria.md §4.2 (a): que con consigna
 * ≠ 0 el chip cargue sin más software. [sin saber] si el aparato lo hace. */
enum { W580_B7_NADA, W580_B7_CORTA, W580_B7_APAGA };
#define W580_CARGA_MAX_S 20           /* el original deja 0x61 = 0x13 durante 16,9 s en cada arranque sin cargador (1,33 a 18,20 s) */
static int64_t w580_bat_ahora(void) { return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); }
/* techo de tensión del código de 0x60 (tabla 0x44288F28 del original, docs/bateria.md §1): mV */
static unsigned w580_bat_techo_mv(unsigned cod)
{
    static const uint16_t t[32] = { 3300, 3400, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 3925, 3950, 3975, 4000, 4025, 4050, 4075,
                                    4100, 4125, 4150, 4175, 4200, 4225, 4250, 4275, 4300, 4350, 4400, 4500, 4600, 4800, 5000, 5200 };
    return t[cod & 31];
}
static unsigned w580_bat_techo(W580State *s)
{
    static const uint16_t tu[4] = { 3900, 4000, 4100, 4500 };     /* tabla 0x4427FB68: 0x63 bits 0..1 */
    uint8_t *r = s->i2c[0].regs[0x4a];
    unsigned t = 0;
    if (s->bat.cargador && (r[0x61] & 15)) { t = w580_bat_techo_mv(r[0x60]); }
    if (s->bat.usb && (r[0x63] & 0x30)) { t = MAX(t, tu[r[0x63] & 3]); }
    return t;
}
/* Corriente (mA) que el MODELO supone entrando a la batería con el estado de AHORA de los registros y las entradas. */
static unsigned w580_bat_ma_ahora(W580State *s)
{
    static const uint16_t iu[4] = { 0, 75, 175, 275 };            /* [inferido de los umbrales 750/1750/2750 (0,1 mA) de 0x4427FB58] */
    uint8_t *r = s->i2c[0].regs[0x4a];
    unsigned ma = 0;
    if (s->bat.ausente) { return 0; }
    if (s->bat.b7_modo == W580_B7_CORTA && !(r[0x60] & 0x80)) { return 0; }
    if (s->bat.cargador) { ma = (r[0x61] & 15) * 100; }           /* tabla 0x44288EE8: código N = N x 100 mA */
    if (s->bat.usb) { ma = MAX(ma, iu[(r[0x63] >> 4) & 3]); }
    return ma;
}
static double w580_bat_mv_reposo(W580State *s) { return 2755.0 + 10.843 * s->pmu_vbat; }
/* Integra la batería hasta ahora con el estado que había (se llama ANTES de cambiar nada y al leer). */
static void w580_bat_avanza(W580State *s)
{
    struct W580Bat *b = &s->bat;
    int64_t t = w580_bat_ahora();
    double dt = (t - b->t_mv) / 1e9;
    b->t_mv = t;
    if (dt <= 0) { return; }
    if (b->ma) {
        b->s_ma += dt; b->mah += b->ma * dt / 3600.0;
        if (b->puesto && b->sube > 0) {
            double techo = w580_bat_techo(s);
            if (b->mv < techo) { b->mv = MIN(techo, b->mv + b->sube * dt); }
        }
    } else if (b->puesto && b->descarga > 0) {
        b->mv = MAX(2500.0, b->mv - b->descarga * dt);
    }
}
static double w580_bat_mv(W580State *s)
{
    w580_bat_avanza(s);
    return s->bat.puesto ? s->bat.mv : w580_bat_mv_reposo(s);
}
/* Cuenta de 8 bits de la batería: temprana = lectura con 0x70 <- 0x6A; si no, 0x75..0x77 y el ADC 10. */
static uint8_t w580_bat_cuenta(W580State *s, bool temprana, uint8_t de_siempre)
{
    if (!s->bat.puesto) { return de_siempre; }
    double mv = w580_bat_mv(s);
    double c = temprana ? (mv - 2796.0) / 10.784 : (mv - 2755.0) / 10.843;
    return c < 0 ? 0 : c > 255 ? 255 : (uint8_t)(c + 0.5);
}
/* Tras cualquier cambio (registros 0x60..0x63, entradas, bit 7): ¿ha cambiado la corriente que ve el modelo? */
static void w580_bat_tras(W580State *s, const char *por)
{
    struct W580Bat *b = &s->bat;
    unsigned ma = w580_bat_ma_ahora(s);
    if (ma == b->ma) { return; }
    uint8_t *r = s->i2c[0].regs[0x4a];
    b->n_cambios++;
    if (ma && !b->ma) { b->t_ma = w580_bat_ahora(); }
    if (b->log < 400) { b->log++;
        qemu_log_mask(LOG_UNIMP, "W580_CARGA corriente del modelo %u -> %u mA (%s) t=%.3f s: cargador=%d usb=%d bateria=%s 60=%02x 61=%02x 62=%02x 63=%02x%s\n",
                      b->ma, ma, por, w580_bat_ahora() / 1e9, b->cargador, b->usb, b->ausente ? "QUITADA" : "puesta", r[0x60], r[0x61], r[0x62], r[0x63],
                      ma ? "  <-- CORRIENTE ENTRANDO A LA BATERIA [modelo pesimista, docs/bateria.md §4.2 (a)]" : "");
    }
    b->ma = ma;
}
static void w580_bat_b7_arma(W580State *s)
{
    struct W580Bat *b = &s->bat;
    if (!b->b7_timer) { return; }
    if (b->b7_modo != W580_B7_NADA && (s->i2c[0].regs[0x4a][0x60] & 0x80)) {
        b->b7_vence = w580_bat_ahora() + (int64_t)b->b7_s * 1000000000;
        timer_mod(b->b7_timer, b->b7_vence);
    } else { timer_del(b->b7_timer); b->b7_vence = 0; }
}
static void w580_bat_b7_caduca(void *opaque)
{
    W580State *s = opaque;
    struct W580Bat *b = &s->bat;
    uint8_t *r = s->i2c[0].regs[0x4a];
    if (b->b7_modo == W580_B7_NADA || !(r[0x60] & 0x80)) { return; }
    w580_bat_avanza(s);
    b->b7_caducas++; b->b7_vence = 0;
    if (b->b7_modo == W580_B7_CORTA) {
        r[0x60] &= ~0x80; b->b7_caducado = true;
        qemu_log_mask(LOG_UNIMP, "W580_CARGA el bit 7 de 0x60 CADUCA a los %u s sin reescribirlo (carga-bit7=corta): el chip lo baja y deja de cargar; 60=%02x t=%.3f s\n",
                      b->b7_s, r[0x60], w580_bat_ahora() / 1e9);
        w580_bat_tras(s, "caduca el bit 7 de 0x60");
    } else {
        qemu_log_mask(LOG_UNIMP, "W580_APAGADO por caducar el bit 7 de 0x60 a los %u s sin reescribirlo (carga-bit7=apaga) t=%.3f s\n", b->b7_s, w580_bat_ahora() / 1e9);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}
/* Regla [carga] a: cuánto tiempo lleva 0x61 un código de corriente ≠ 0. Dos cuentas (pista E8, hallazgo P1-A1 de la auditoría AUD4;
 * antes eran 20 s SEGUIDOS desde la transición 0 -> ≠0, y alternar 13 … 10 … 13 cada 19 s no avisaba nunca):
 *   a1 (clave 0x6100): más de W580_CARGA_MAX_S (20) segundos ACUMULADOS DESDE EL RESET. El original: un solo tramo por arranque, de
 *       16,9 s (17,8 s si arranca con cargador). Un tramo que el reset pilla ABIERTO (pmu-reset=caliente: el PMU conserva 0x61) sigue
 *       contando entero a través del reset.
 *   a2 (clave 0x6101): más de W580_CARGA_VENTANA_MAX_S (60) segundos acumulados en los últimos W580_CARGA_VENTANA_S (300), sin
 *       borrarse en el reset: un bucle de reinicios que vuelve a poner corriente en cada arranque. El original reiniciándose suma
 *       16,9 s por arranque (dos arranques: 33,8 s; tres: 50,7 s): no llega con hasta tres arranques en 5 minutos.
 * El historial de la ventana guarda 16 tramos; al llenarse FUNDE los dos más viejos (suma sus duraciones; no pierde tiempo).
 * El aviso lleva el PC de la ESCRITURA que abrió el tramo en curso. Y OJO: que [carga] no salte NO demuestra que no se cargue
 * (docs/auditoria_emulador_y_banco.md P1-A1): la prueba fuerte es cargador=on desde el arranque y carga-estado «acumulado 0.0 s». */
#define W580_CARGA_VENTANA_S 300
#define W580_CARGA_VENTANA_MAX_S 60
static double w580_bat_61_acum(W580State *s, int64_t t)        /* ventana de 300 s, a través de resets */
{
    struct W580Bat *b = &s->bat;
    int64_t desde = t - W580_CARGA_VENTANA_S * INT64_C(1000000000), suma = 0;
    for (unsigned i = 0; i < b->h61_n; i++) {
        int64_t a = MAX(b->h61_ini[i], desde), f = MIN(b->h61_fin[i], t);
        if (f > a) { suma += f - a; }
    }
    if (b->t61) { suma += t - MAX(b->t61, desde); }
    return suma / 1e9;
}
static double w580_bat_61_arranque(W580State *s, int64_t t)    /* desde el reset (más el tramo abierto, entero) */
{
    return s->bat.a61_arranque + (s->bat.t61 ? (t - s->bat.t61) / 1e9 : 0);
}
static void w580_bat_61_arma(W580State *s)       /* con un tramo abierto: el instante más temprano en que alguna cuenta llega a su tope */
{
    struct W580Bat *b = &s->bat;
    int64_t t = w580_bat_ahora();
    double falta = MIN(W580_CARGA_MAX_S - w580_bat_61_arranque(s, t), W580_CARGA_VENTANA_MAX_S - w580_bat_61_acum(s, t));
    timer_mod(b->t61_timer, t + (falta > 0.001 ? (int64_t)(falta * 1e9) + 1000 : 1000000));
}
static void w580_bat_61_vence(void *opaque)
{
    W580State *s = opaque;
    struct W580Bat *b = &s->bat;
    uint8_t *r = s->i2c[0].regs[0x4a];
    if (!(r[0x61] & 15) || !b->t61) { return; }
    int64_t t = w580_bat_ahora();
    double arr = w580_bat_61_arranque(s, t), ven = w580_bat_61_acum(s, t), seguidos = (t - b->t61) / 1e9;
    const char *corr = b->ma ? "Y HAY CORRIENTE ENTRANDO A LA BATERÍA según el modelo (cargador o USB puestos)" : "Sin cargador el modelo no ve corriente; con él, la vería";
    if (arr < W580_CARGA_MAX_S && ven < W580_CARGA_VENTANA_MAX_S) { w580_bat_61_arma(s); return; }
    b->n61_avisos++;
    w580_es.pc_forzado = b->pc61;
    if (arr >= W580_CARGA_MAX_S) {
        w580_es_aviso(s, "carga", 0x6100, "PMU 0x61 lleva un código de corriente de carga ≠ 0 (0x%02x = %u mA) más de %d s acumulados desde el reset (%.1f s; "
                      "%.1f s seguidos ahora): el original lo deja como mucho 16,9 s por arranque (la lista escribe 0x13 a 1,33 s y BATTMGR 0x10 a 18,20 s) y lo pone a 0 "
                      "siempre que no quiere cargar. %s [el PC es el de la escritura; el LR no vale]", r[0x61], (r[0x61] & 15) * 100, W580_CARGA_MAX_S, arr, seguidos, corr);
    } else {
        w580_es_aviso(s, "carga", 0x6101, "PMU 0x61 lleva un código de corriente de carga ≠ 0 (0x%02x = %u mA) más de %d s acumulados en los últimos %d s, a través de "
                      "resets (%.1f s; %.1f s en este arranque): más que tres arranques seguidos del original (16,9 s cada uno). %s [el PC es el de la escritura; el LR no vale]",
                      r[0x61], (r[0x61] & 15) * 100, W580_CARGA_VENTANA_MAX_S, W580_CARGA_VENTANA_S, ven, arr, corr);
    }
    w580_es.pc_forzado = 0;
    timer_mod(b->t61_timer, t + W580_CARGA_MAX_S * INT64_C(1000000000));
}
static void w580_bat_61_cierra(W580State *s)
{
    struct W580Bat *b = &s->bat;
    if (b->t61) {
        int64_t t = w580_bat_ahora();
        double d = (t - b->t61) / 1e9; if (d > b->s61_max) { b->s61_max = d; }
        b->a61_arranque += d;
        if (b->h61_n == G_N_ELEMENTS(b->h61_ini)) {      /* lleno: los dos más viejos se FUNDEN en uno que dura la suma y acaba donde el segundo */
            b->h61_ini[1] -= b->h61_fin[0] - b->h61_ini[0];
            memmove(b->h61_ini, b->h61_ini + 1, sizeof(b->h61_ini) - sizeof(b->h61_ini[0]));
            memmove(b->h61_fin, b->h61_fin + 1, sizeof(b->h61_fin) - sizeof(b->h61_fin[0]));
            b->h61_n--;
        }
        b->h61_ini[b->h61_n] = b->t61; b->h61_fin[b->h61_n] = t; b->h61_n++;
    }
    b->t61 = 0;
    if (b->t61_timer) { timer_del(b->t61_timer); }
}
/* Una escritura del firmware en 0x60..0x63 (ya guardada en el banco; antes = lo que había). */
static void w580_bat_escribe(W580State *s, uint8_t reg, uint8_t antes, uint8_t value)
{
    struct W580Bat *b = &s->bat;
    if (reg == 0x60) {
        if (value & 0x80) { b->b7_caducado = false; b->b7_rearmes++; }
        w580_bat_b7_arma(s);
        if ((value & 0x1f) > 0x14) {
            w580_es_aviso(s, "carga", 0x6000 | value, "PMU 0x60 <- 0x%02x: código de tensión de carga 0x%02x = %u mV, por encima de 0x14 = 4200 mV (el original: 0x08 = 3900 mV "
                          "en la lista; el techo de una celda de litio es 4200 mV)", value, value & 0x1f, w580_bat_techo_mv(value));
        }
    } else if (reg == 0x61) {
        if ((value & 15) && !(antes & 15)) {
            b->t61 = w580_bat_ahora(); b->pc61 = w580_es_pc(s);
            w580_bat_61_arma(s);
        } else if (!(value & 15)) { w580_bat_61_cierra(s); }
    } else if (reg == 0x62 && value > 1) {
        w580_es_aviso(s, "carga", 0x6200 | value, "PMU 0x62 <- 0x%02x: el original solo escribe ahí 0x00 y 0x01 (HAL_Battery; qué es: [sin saber], «PWM» según una cadena)", value);
    } else if (reg == 0x63 && value) {
        w580_es_aviso(s, "carga", 0x6300 | value, "PMU 0x63 <- 0x%02x: carga por USB (bits 4..5 corriente, bits 0..1 tensión); el original solo escribe 0x00 en lo ejecutado", value);
    }
    w580_bat_tras(s, "escritura del firmware");
}
/* 0x23 según las entradas; con suceso = además levanta 0x21 (cambio en caliente). */
static void w580_bat_estado23(W580State *s, uint8_t suceso)
{
    uint8_t *r = s->i2c[0].regs[0x4a];
    r[0x23] = (r[0x23] & ~0xc8) | (s->bat.cargador ? 0x08 : 0) | (s->bat.ausente ? 0x40 : 0) | (s->bat.usb ? 0x80 : 0);
    if (suceso) { r[0x21] |= suceso; s->bat.n_sucesos++; w580_irq_update(s); }
}
/* Pista E8 (P1-M1 de AUD4): reset de la CPU con el PMU alimentado (pmu-reset=caliente): el banco se conserva. Un tramo abierto de
 * 0x61 sigue abierto; la cuenta «desde el reset» de los tramos cerrados empieza de cero; el bit 7 de 0x60 sigue con su plazo. */
static void w580_bat_reset_caliente(W580State *s)
{
    struct W580Bat *b = &s->bat;
    b->negado = false; b->a61_arranque = 0;
    w580_bat_estado23(s, 0);
    if (b->t61) { w580_bat_61_arma(s); }
    w580_bat_tras(s, "reset caliente");
}
static void w580_bat_reset(W580State *s)       /* desde w580_pmu_reset, con el banco ya a cero */
{
    struct W580Bat *b = &s->bat;
    b->b7_caducado = false; b->b7_vence = 0; b->negado = false;
    if (b->b7_timer) { timer_del(b->b7_timer); }
    w580_bat_61_cierra(s); b->a61_arranque = 0;
    w580_bat_estado23(s, 0);       /* [sin saber] la causa del encendido con cargador: se deja la de siempre (tecla, 0x21 = 01) */
    w580_bat_tras(s, "reset");
}
static void w580_bat_init(W580State *s)
{
    s->bat.b7_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_bat_b7_caduca, s);
    s->bat.t61_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_bat_61_vence, s);
}
/* ¿Puede apagar 0x41 <- 0? (lo pregunta w580_apagado_mirar, w580-extra.inc) */
static bool w580_bat_deja_apagar(W580State *s)
{
    struct W580Bat *b = &s->bat;
    if ((b->cargador || b->usb) && !b->apaga_cc) {
        if (!b->negado) {
            qemu_log_mask(LOG_UNIMP, "W580_APAGADO_NEGADO PMU 0x41 <- 0 con %s puesto: NO apaga (apaga-con-cargador=off; [sin saber] qué hace el aparato). "
                          "Queda pendiente: apagará al quitar la alimentación externa. pc=0x%08x t=%.3f s\n", b->cargador ? "el cargador" : "el USB",
                          s->cpu->env.regs[15], w580_bat_ahora() / 1e9);
        }
        b->negado = true;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- propiedades
 * Los valores llegan de tres sitios: -M (texto), qom-set con JSON (booleano o número) y los guiones de la pista S, que
 * mandan entero si el valor es un número y cadena si no («prop cargador on», «prop descarga-mv-por-s 2.5»): se acepta todo. */
static bool w580_bat_visita(Visitor *v, const char *name, double *out, Error **errp)
{
    QObject *q = NULL;
    if (!visit_type_any(v, name, &q, errp)) { return false; }      /* un solo intento: el visitante de -M no deja visitar dos veces el mismo nombre */
    QBool *qb = qobject_to(QBool, q); QNum *qn = qobject_to(QNum, q); QString *qs = qobject_to(QString, q);
    bool ok = true;
    if (qb) { *out = qbool_get_bool(qb); }
    else if (qn) { *out = qnum_get_double(qn); }
    else if (qs) {
        const char *t = qstring_get_str(qs); char *fin = NULL;
        if (!g_ascii_strcasecmp(t, "on") || !g_ascii_strcasecmp(t, "si") || !g_ascii_strcasecmp(t, "true") || !g_ascii_strcasecmp(t, "yes")) { *out = 1; }
        else if (!g_ascii_strcasecmp(t, "off") || !g_ascii_strcasecmp(t, "no") || !g_ascii_strcasecmp(t, "false")) { *out = 0; }
        else {
            *out = g_ascii_strtod(t, &fin);
            if (fin == t || *fin || !(*out == *out) || *out > 1e12 || *out < -1e12) { error_setg(errp, "%s: «%s» no es on, off ni un número", name, t); ok = false; }   /* P1-B6 de AUD4: «nan», «inf» */
        }
    } else { error_setg(errp, "%s: se espera on, off o un número", name); ok = false; }
    qobject_unref(q);
    return ok;
}
enum { W580_BP_MV, W580_BP_DESCARGA, W580_BP_SUBE, W580_BP_CARGADOR, W580_BP_CARGADOR_MV, W580_BP_USB, W580_BP_PRESENTE, W580_BP_APAGA_CC };
static void w580_bat_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580Bat *b = &s->bat;
    bool o = false; double d = 0; uint32_t u = 0;
    switch ((uintptr_t)opaque) {
    case W580_BP_MV: u = b->puesto ? (uint32_t)(w580_bat_mv(s) + 0.5) : 0; visit_type_uint32(v, name, &u, errp); return;
    case W580_BP_CARGADOR_MV: u = b->cargador_mv; visit_type_uint32(v, name, &u, errp); return;
    case W580_BP_DESCARGA: d = b->descarga; visit_type_number(v, name, &d, errp); return;
    case W580_BP_SUBE: d = b->sube; visit_type_number(v, name, &d, errp); return;
    case W580_BP_CARGADOR: o = b->cargador; break;
    case W580_BP_USB: o = b->usb; break;
    case W580_BP_PRESENTE: o = !b->ausente; break;
    case W580_BP_APAGA_CC: o = b->apaga_cc; break;
    }
    visit_type_bool(v, name, &o, errp);
}
static void w580_bat_set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580Bat *b = &s->bat;
    bool viva = s->cpu != NULL;          /* false = al lanzar (-M): solo se guarda; el reset lo aplica */
    double x;
    if (!w580_bat_visita(v, name, &x, errp)) { return; }
    if (viva) { w580_bat_avanza(s); }
    switch ((uintptr_t)opaque) {
    case W580_BP_MV:
        if (x != 0 && (x < 2000 || x > 5000)) { error_setg(errp, "pmu-vbat-mv: de 2000 a 5000 mV (0 = sin poner: las cuentas de siempre)"); return; }
        b->puesto = x != 0; b->mv = x;
        break;
    case W580_BP_DESCARGA: case W580_BP_SUBE:
        if (x < 0 || x > 1000) { error_setg(errp, "%s: de 0 a 1000 mV/s", name); return; }
        if (!b->puesto && x > 0) { b->puesto = true; b->mv = w580_bat_mv_reposo(s); }      /* parte de la tensión «en marcha» de las cuentas de siempre (3926 mV) */
        if ((uintptr_t)opaque == W580_BP_DESCARGA) { b->descarga = x; } else { b->sube = x; }
        break;
    case W580_BP_CARGADOR_MV:
        if (x < 0 || x > 20000) { error_setg(errp, "cargador-mv: de 0 a 20000 mV"); return; }
        b->cargador_mv = x;
        break;
    case W580_BP_CARGADOR:
        if (b->cargador == (x != 0)) { return; }
        b->cargador = x != 0;
        if (viva) { w580_bat_estado23(s, 0x08); }
        break;
    case W580_BP_USB:
        if (b->usb == (x != 0)) { return; }
        b->usb = x != 0;
        if (viva) { w580_bat_estado23(s, 0x40); }
        w580_usb_vbus(s, !b->usb);                       /* pista UEMU: DEVSTAT.ATT del controlador USB */
        break;
    case W580_BP_PRESENTE:
        b->ausente = x == 0;
        if (viva) { w580_bat_estado23(s, 0); }
        break;
    case W580_BP_APAGA_CC: b->apaga_cc = x != 0; break;
    }
    if (!viva) { return; }
    qemu_log_mask(LOG_UNIMP, "W580_BATERIA %s <- %g t=%.3f s (23=%02x 21=%02x)\n", name, x, w580_bat_ahora() / 1e9, s->i2c[0].regs[0x4a][0x23], s->i2c[0].regs[0x4a][0x21]);
    w580_es_suceso("bateria", "%s=%g", name, x);
    w580_bat_tras(s, name);
    if (!(b->cargador || b->usb) || b->apaga_cc) { b->negado = false; w580_apagado_mirar(s); }     /* el apagado que quedó pendiente */
}
static char *w580_bat_get_b7(Object *obj, Error **errp)
{
    struct W580Bat *b = &W580_MACHINE(obj)->bat;
    return b->b7_modo == W580_B7_NADA ? g_strdup("nada") : g_strdup_printf("%s:%u", b->b7_modo == W580_B7_CORTA ? "corta" : "apaga", b->b7_s);
}
static void w580_bat_set_b7(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580Bat *b = &s->bat;
    unsigned n = 0; uint8_t modo;
    if (!strcmp(value, "nada")) { modo = W580_B7_NADA; }
    else if (sscanf(value, "corta:%u", &n) == 1 && n >= 1 && n <= 86400) { modo = W580_B7_CORTA; }
    else if (sscanf(value, "apaga:%u", &n) == 1 && n >= 1 && n <= 86400) { modo = W580_B7_APAGA; }
    else { error_setg(errp, "carga-bit7: nada | corta:N | apaga:N (N segundos, 1..86400)"); return; }
    if (s->cpu) { w580_bat_avanza(s); }
    b->b7_modo = modo; b->b7_s = n;
    if (s->cpu) { w580_bat_b7_arma(s); w580_bat_tras(s, "carga-bit7"); }
}
static char *w580_bat_get_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    struct W580Bat *b = &s->bat;
    uint8_t *r = s->i2c[0].regs[0x4a];
    int64_t t = w580_bat_ahora();
    double mv = w580_bat_mv(s);
    GString *g = g_string_new(NULL);
    g_string_append_printf(g, "t=%.2f s | bateria %s %.0f mV%s (77=%u cuentas, 6A=%u) descarga=%g mV/s sube=%g mV/s | cargador=%s (%u mV) usb=%s | "
                           "23=%02x 21=%02x | 60=%02x (techo %u mV, bit7=%d) 61=%02x (%u mA) 62=%02x 63=%02x 41=%02x | ",
                           t / 1e9, b->ausente ? "QUITADA" : "puesta", mv, b->puesto ? "" : " [las cuentas de siempre]",
                           w580_bat_cuenta(s, false, s->pmu_vbat), w580_bat_cuenta(s, true, s->pmu_vbat), b->descarga, b->sube,
                           b->cargador ? "on" : "off", b->cargador_mv, b->usb ? "on" : "off", r[0x23], r[0x21],
                           r[0x60], w580_bat_techo_mv(r[0x60]), r[0x60] >> 7, r[0x61], (r[0x61] & 15) * 100, r[0x62], r[0x63], r[0x41]);
    if (b->b7_modo == W580_B7_NADA) { g_string_append(g, "carga-bit7=nada | "); }
    else {
        g_string_append_printf(g, "carga-bit7=%s:%u ", b->b7_modo == W580_B7_CORTA ? "corta" : "apaga", b->b7_s);
        if (b->b7_vence) { g_string_append_printf(g, "(caduca en %.1f s) | ", (b->b7_vence - t) / 1e9); }
        else { g_string_append_printf(g, "(%s) | ", b->b7_caducado ? "CADUCADO: carga cortada por el chip" : "sin armar: nadie ha escrito el bit 7"); }
    }
    double s61 = b->t61 ? (t - b->t61) / 1e9 : 0;
    g_string_append_printf(g, "61 con corriente != 0: %s%.1f s seguidos (maximo %.1f s; el original 16.9), %.1f s desde el reset (regla [carga] a los %d) y %.1f s en los ultimos %d s "
                           "(a los %d): %" PRIu64 " avisos | ", b->t61 ? "AHORA, " : "no; ultimo tramo cerrado; ahora ", s61, MAX(s61, b->s61_max),
                           w580_bat_61_arranque(s, t), W580_CARGA_MAX_S, w580_bat_61_acum(s, t), W580_CARGA_VENTANA_S, W580_CARGA_VENTANA_MAX_S, b->n61_avisos);
    if (b->ma) {
        g_string_append_printf(g, "CORRIENTE ENTRANDO A LA BATERIA: %u mA desde t=%.2f s%s", b->ma, b->t_ma / 1e9,
                               b->puesto && mv >= w580_bat_techo(s) ? " (en el techo: tension constante)" : "");
    } else {
        g_string_append(g, "sin corriente a la bateria");
    }
    g_string_append_printf(g, " | acumulado %.1f s con corriente, %.3f mAh, %" PRIu64 " cambios | bit 7 rearmado %" PRIu64 " veces, caducado %" PRIu64 "%s\n",
                           b->s_ma, b->mah, b->n_cambios, b->b7_rearmes, b->b7_caducas,
                           b->negado ? " | APAGADO PEDIDO (41 <- 0) Y NEGADO por la alimentacion externa: pendiente" : "");
    return g_string_free(g, false);
}
static void w580_bat_class_init(ObjectClass *oc)
{
    static const struct { const char *n, *tipo, *d; uintptr_t k; } p[] = {
        { "pmu-vbat-mv", "uint32", "Pista E7: tension de la bateria en mV, movible en caliente (75/76/77, lectura de 6A y ADC 10). 0 = sin poner: las cuentas de siempre", W580_BP_MV },
        { "descarga-mv-por-s", "number", "Pista E7: la bateria baja X mV por segundo virtual mientras no le entra corriente (0 = quieta)", W580_BP_DESCARGA },
        { "carga-mv-por-s", "number", "Pista E7: la bateria sube X mV por segundo virtual mientras el modelo ve corriente entrando, hasta el techo de 0x60", W580_BP_SUBE },
        { "cargador", "bool", "Pista E7: cargador puesto (PMU 0x23 bit 3; al cambiar, suceso 0x21 bit 3 e IRQ 37). En caliente", W580_BP_CARGADOR },
        { "cargador-mv", "uint32", "Pista E7: tension del cargador en mV (ADC 8 = mV/25 mientras hay cargador). Por omision 5000", W580_BP_CARGADOR_MV },
        { "usb-vbus", "bool", "Pista E7: USB puesto (el nombre «usb» ya es de QEMU) (PMU 0x23 bit 7; al cambiar, suceso 0x21 bit 6 e IRQ 37). En caliente", W580_BP_USB },
        { "bateria-presente", "bool", "Pista E7: off = bateria quitada (PMU 0x23 bit 6 a 1). En caliente", W580_BP_PRESENTE },
        { "apaga-con-cargador", "bool", "Pista E7: con apagado-real=on, PMU 0x41 <- 0 apaga aunque haya cargador o USB. Por omision off: no apaga y queda pendiente", W580_BP_APAGA_CC },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(p); i++) {
        object_class_property_add(oc, p[i].n, p[i].tipo, w580_bat_get, w580_bat_set, NULL, (void *)p[i].k);
        object_class_property_set_description(oc, p[i].n, p[i].d);
    }
    object_class_property_add_str(oc, "carga-bit7", w580_bat_get_b7, w580_bat_set_b7);
    object_class_property_set_description(oc, "carga-bit7", "Pista E7: que hace el bit 7 de PMU 0x60: nada | corta:N (a los N s sin reescribirlo el chip lo baja y deja de cargar) | apaga:N (apaga la maquina)");
    object_class_property_add_str(oc, "carga-estado", w580_bat_get_estado, NULL);
    object_class_property_set_description(oc, "carga-estado", "Pista E7: bateria, cargador, registros 60..63 y si el modelo ve corriente entrando a la bateria");
}
