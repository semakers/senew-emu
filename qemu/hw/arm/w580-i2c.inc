/* Maestros I2C del DB2020 — DOS buses: I2C0 en 0xF9040000 e I2C1 en 0xF9070000 («I2C0/I2C1 SW IRQ issued»
 * en el registro; el main espera en 0x441AA48A a que el bit 4 de F9070002 se borre solo) —, portado del modelo del DB2010 (SEW800iEmu, w800-i2c.inc).
 * SPDX-License-Identifier: GPL-2.0-or-later
 * [observado en e06] mismo protocolo: +0 dato, +2 control (0x20 inicio, 0x10 parada, 0x04 ACK, bit 3 = paso
 * terminado; el main escribe 4, 0, 0x24 y sondea el bit 3 desde 0x449CDD06), +4 divisor (0x7E), +6 estado
 * (códigos clásicos 0x08/0x10/0x18/0x20/0x28/0x30/0x40/0x48/0x50/0x58).
 * Los esclavos son bancos de registros: los que NO están en w580_i2c_presente() no contestan (NACK), y cada
 * transacción se registra (W580_I2C) para saber a quién llama el main. */
static uint8_t w580_i2c_dev_read(W580I2C *b, uint8_t slave, uint8_t reg);
static void w580_i2c_dev_write(W580I2C *b, uint8_t slave, uint8_t reg, uint8_t value);
static bool w580_i2c_presente(W580I2C *b, uint8_t slave);
#include "w580-fallos.inc"         /* pista E8: fallos inyectados; sin sus propiedades, lo de siempre */
static void w580_i2c_log(W580I2C *b, const char *que, unsigned a, unsigned v)
{
    W580State *s = b->s;
    if (s->i2c_log < 3000) { s->i2c_log++;
        qemu_log_mask(LOG_UNIMP, "W580_I2C%u %s esclavo=0x%02x reg=0x%02x valor=0x%02x pc=0x%08x lr=0x%08x\n", b->idx, que, b->slave, a, v,
                      s->cpu->env.regs[15], s->cpu->env.regs[14]);
    }
}
static void w580_i2c_complete(void *opaque)
{
    W580I2C *b = opaque;
    b->pending = true;
    w580_irq_update(b->s);
}
static void w580_i2c_reset1(W580I2C *b)
{
    timer_del(b->timer);
    b->pending = false;
    b->control = b->data = b->divider = 0;
    b->status = 0xf8;
    b->phase = b->slave = b->reg = 0;
    b->reading = false;
    b->fa_modo = b->fa_pegado = b->fa_vistas = 0; b->fa_parada = false; b->fa_hasta = 0;
    if (b->fa_timer) { timer_del(b->fa_timer); }
}
/* Pista E8: el paso en curso se cuelga (fallo «ocupado»): no termina, no hay IRQ, y el bit de START o de STOP que se
 * estuviera esperando se queda a 1. Se sale con otro START (transacción nueva) o con un STOP. */
static void w580_i2c_cuelga(W580I2C *b, uint8_t pegado)
{
    b->fa_modo = W580_FA_OCUPADO; b->fa_pegado = pegado; b->fa_t = w580_fa_ahora();
    if (b->fa_hasta > b->fa_t) { timer_mod(b->fa_timer, b->fa_hasta); }      /* regla con ms: se suelta sola */
    w580_irq_update(b->s);
}
static void w580_i2c_control(W580I2C *b, uint16_t value);
/* El cuelgue se suelta sin que el firmware haga nada (vence la ventana ms de la regla, o se cambia i2c-fallo): el paso que estaba
 * a medias se ejecuta AHORA, tarde, sin mirar las reglas; si lo colgado era el STOP, el bit 4 baja. */
static void w580_i2c_suelta(W580I2C *b, const char *por)
{
    if (b->fa_modo != W580_FA_OCUPADO) { return; }
    w580_fa_log(b->s, "I2C%u el cuelgue se SUELTA solo a los %.3f ms (%s): el paso termina tarde", b->idx, (w580_fa_ahora() - b->fa_t) / 1e6, por);
    timer_del(b->fa_timer);
    b->fa_modo = 0;
    if (b->fa_pegado == 0x10) { b->fa_pegado = 0; w580_irq_update(b->s); return; }
    b->fa_exento = true; w580_i2c_control(b, b->fa_valor); b->fa_exento = false;
}
static void w580_i2c_suelta_t(void *opaque) { w580_i2c_suelta(opaque, "vence el ms de la regla"); }
static void w580_i2c_control(W580I2C *b, uint16_t value)
{
    b->fa_valor = value;
    W580State *s = b->s;
    bool completion = false;
    timer_del(b->timer);
    b->pending = false;
    b->control = value & ~0x38;
    b->fa_pegado = 0;
    if (b->fa_modo == W580_FA_OCUPADO) {           /* E8: colgado. Solo se sale con START (transacción nueva) o STOP */
        timer_del(b->fa_timer);
        if (value & 0x30) {
            w580_fa_log(s, "I2C%u sale del cuelgue con un %s a los %.3f ms (lo que esperó el firmware)", b->idx, value & 0x20 ? "START" : "STOP", (w580_fa_ahora() - b->fa_t) / 1e6);
        }
        if (value & 0x20) { b->phase = 0; b->fa_modo = 0; }
        else {
            if (value & 0x10) { b->fa_modo = 0; b->phase = 0; b->status = 0xf8; }
            w580_irq_update(s);
            return;
        }
    }
    if (value & 0x20) {
        if (!b->phase) {
            w580_fa_i2c_empieza(b);
            if (s->fa.nr && w580_fa_i2c(b, W580_FP_INICIO)) { w580_i2c_cuelga(b, 0x20); return; }
        }
        b->status = b->phase ? 0x10 : 8;
        b->phase = 1;
        completion = true;
    } else if (value & 0x10) {
        b->phase = 0;
        b->status = 0xf8;
        if (b->fa_parada) {
            b->fa_parada = false; s->fa.n_colgado++;
            w580_fa_log(s, "I2C%u OCUPADO en el STOP: esclavo=0x%02x reg=0x%02x (la transaccion fue bien; el bit 4 de +2 se queda a 1)", b->idx, b->slave, b->reg);
            w580_i2c_cuelga(b, 0x10);
            return;
        }
        b->fa_modo = 0;
    } else if (b->phase == 1) {
        b->slave = b->data >> 1;
        b->reading = b->data & 1;
        bool era_primera = b->fa_primera;
        int fa = s->fa.nr && b->fa_modo != W580_FA_NACK ? w580_fa_i2c(b, b->reading ? W580_FP_DIR_L : W580_FP_DIR) : 0;
        b->fa_primera = false;
        if (fa == W580_FA_OCUPADO) { b->fa_primera = era_primera; w580_i2c_cuelga(b, 0); return; }
        if (fa == W580_FA_NACK) { b->fa_modo = W580_FA_NACK; }
        bool acknowledged = w580_i2c_presente(b, b->slave) && b->fa_modo != W580_FA_NACK;
        b->status = acknowledged ? (b->reading ? 0x40 : 0x18) : (b->reading ? 0x48 : 0x20);
        if (!acknowledged && b->fa_modo != W580_FA_NACK) { w580_i2c_log(b, "SIN-RESPUESTA", 0, b->reading); w580_es_i2c_nadie(s, b->idx, b->slave); }
        b->phase = 2;
        completion = true;
    } else if (b->phase >= 2) {
        if (b->reading) {
            if (b->fa_modo == W580_FA_NACK) {      /* E8: el esclavo no contestó a la dirección: la línea de datos está suelta */
                b->data = 0xff;
                w580_i2c_log(b, "lee-MUDO", b->reg, b->data);
                b->status = 0x58;
            } else {
            if (b->idx == 0 && b->slave == 0x4a && b->reg == 0x71) { w580_es_adc_lee(s); }
            b->data = w580_i2c_dev_read(b, b->slave, b->reg);
            w580_i2c_log(b, "lee", b->reg, b->data);
            w580_es_i2c(s, b->idx, b->slave, b->reg, false, b->data);
            b->reg++;
            b->status = value & 4 ? 0x50 : 0x58;
            }
        } else {
            bool acknowledged = w580_i2c_presente(b, b->slave) && b->fa_modo != W580_FA_NACK, recien = false;
            if (acknowledged) {
                if (b->phase == 2) { b->reg = b->data; }
                else {
                    int fa = s->fa.nr ? w580_fa_i2c(b, W580_FP_DATO) : 0;
                    if (fa == W580_FA_OCUPADO) { w580_i2c_log(b, "escribe-COLGADO", b->reg, b->data); w580_i2c_cuelga(b, 0); return; }
                    if (fa == W580_FA_NACK) { b->fa_modo = W580_FA_NACK; acknowledged = false; recien = true; w580_i2c_log(b, "escribe-NO-LLEGA", b->reg, b->data); }
                    else {
                    w580_i2c_log(b, "escribe", b->reg, b->data);
                    w580_es_i2c(s, b->idx, b->slave, b->reg, true, b->data);
                    w580_i2c_dev_write(b, b->slave, b->reg++, b->data);
                    }
                }
            }
            if (b->fa_modo == W580_FA_NACK && !recien) { w580_i2c_log(b, "byte-al-MUDO", b->reg, b->data); }      /* E8: lo que el firmware sigue mandando a un esclavo que no contestó */
            b->status = acknowledged ? 0x28 : 0x30;
        }
        b->phase = 3;
        completion = true;
    }
    w580_irq_update(s);
    if (completion) { timer_mod(b->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000); }
}
static uint64_t w580_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    W580I2C *b = opaque;
    uint64_t es = 0;
    w580_es_acceso(b->s, (b->idx ? 0xf9070000 : 0xf9040000) + offset, size, false, &es);
    switch (offset) {
    case 0: return b->data;
    case 2: return b->control | (b->pending ? 8 : 0) | b->fa_pegado;      /* fa_pegado: E8, START o STOP que no sale */
    case 4: return b->divider;
    case 6: return b->status;
    default: return 0;
    }
}
static void w580_i2c_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    W580I2C *b = opaque;
    w580_es_acceso(b->s, (b->idx ? 0xf9070000 : 0xf9040000) + offset, size, true, &value);
    if (offset == 0) { b->data = value; }
    if (offset == 2) { w580_i2c_control(b, value); }
    if (offset == 4) { b->divider = value; }
}
static const MemoryRegionOps w580_i2c_ops = {
    .read = w580_i2c_read, .write = w580_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 2 },
};
/* ---- esclavos: bancos de registros con lo medido
 * Bus 0, 0x4A = compañero analógico/de potencia. [observado en e12] el main le habla con el MISMO mapa que el
 * «Vincenne» del DB2010 (w800-power.inc de SEW800iEmu): 0x20 identificador, 0x21/0x22 sucesos (se borran al leer),
 * 0x23 estado, 0x24..0x27 máscaras, 0x70 control del ADC (0x6A en frío; 0xA0|canal después: canales 1, 8 y 11
 * vistos), 0x71 dato del ADC, 0x75..0x77 medidas de VBAT. El entorno eléctrico es una batería puesta y quieta,
 * sin cargador, y la tecla de encendido como causa del arranque: son ENTRADAS del aparato, no parches. */
#define W580_PMU 0x4a
#include "w580-bateria.inc"        /* pista E7: batería y cargador; sin sus propiedades, lo de siempre */
static uint8_t w580_pmu_adc(W580State *s, unsigned canal)
{
    canal &= 15;
    if (canal == 4 || canal == 12) { return w580_acel_convertir(s, canal); }     /* acelerómetro: w580-acel.inc */
    if (canal == 10) { return w580_bat_cuenta(s, false, s->pmu_adc[10]); }       /* E7: VBAT por pulso */
    if (canal == 8 && s->bat.cargador) { return MIN(255u, s->bat.cargador_mv / 25); }   /* E7: «Chrg V», mV = 25 x cuenta */
    return s->pmu_adc[canal];
}
static bool w580_i2c_presente(W580I2C *b, uint8_t slave)
{
    return b->devs[slave >> 3] >> (slave & 7) & 1;
}
static uint8_t w580_i2c_dev_read(W580I2C *b, uint8_t slave, uint8_t reg)
{
    uint8_t v = b->regs[slave & 127][reg];
    if (b->idx == 0 && slave == W580_PMU) {
        if (reg >= 0x75 && reg <= 0x77) {
            if (b->s->fa.pmu77 >= 0) { b->s->fa.n_fijo++; return b->s->fa.pmu77; }       /* E8: lectura clavada */
            return w580_bat_cuenta(b->s, false, b->s->pmu_vbat);
        }
        if (reg == 0x7a && b->s->bat.ma) { return MIN(255u, (unsigned)((b->s->bat.ma * 10 + 22) / 66.42)); }   /* E7: medida automática 6 = corriente del cargador [inferido, pista B] */
        if (reg == 0x21 || reg == 0x22) { b->regs[W580_PMU][reg] = 0; w580_irq_update(b->s); }   /* sucesos: leer los borra */
        /* (sis28, 28-oct) La línea ON del PMU, como la MIDIÓ senew en el W580 bueno el 2026-10-20 (plat/hw_w580.c estado23(),
         * api/enc.h): 0x23 bit 0 = 1 SUELTA, 0 PULSADA. Dentro de la máquina el bit sigue valiendo 1 = pulsada (w580_pmu_tecla,
         * teclas-arranque, teclas-atascadas, apaga-con-tecla y los registros lo usan así): solo se invierte lo que LEE el firmware.
         * pmu-on-activa-baja=off devuelve la polaridad de antes (la de la pista E, 1 = pulsada). */
        if (reg == 0x23 && b->s->pmu_on_baja) { return v ^ 1; }
    }
    return v;
}
static void w580_i2c_dev_write(W580I2C *b, uint8_t slave, uint8_t reg, uint8_t value)
{
    if (b->idx == 0 && slave == W580_PMU) {
        if ((reg >= 0x20 && reg <= 0x23) || reg == 0x71 || (reg >= 0x75 && reg <= 0x77)) { return; }   /* solo lectura */
        uint8_t antes = b->regs[W580_PMU][reg];
        if (reg >= 0x60 && reg <= 0x63) { w580_bat_avanza(b->s); }
        b->regs[W580_PMU][reg] = value;
        if (reg >= 0x60 && reg <= 0x63) { w580_bat_escribe(b->s, reg, antes, value); }      /* pista E7 */
        if (reg == 0x41) { b->s->pmu_41_cero = value == 0; w580_apagado_mirar(b->s); }     /* apagado-real (pista S) */
        if (reg == 0x43 || reg == 0x44) { w580_ms_pmu(b->s, reg, antes, value); }          /* pista E9: regulador 6 = la tarjeta */
        if (reg == 0x26 || reg == 0x27 || reg == 0x24) { w580_irq_update(b->s); }      /* E7: 0x24 tapa los sucesos de cargador y USB */
        if (reg == 0x70 && ((value & 0xe0) == 0xa0 || value == 0x6a) && !w580_es_adc_elige(b->s, value)) {
            b->regs[W580_PMU][0x71] = value == 0x6a ? w580_bat_cuenta(b->s, true, b->s->pmu_vbat) : w580_pmu_adc(b->s, value & 15);
            if (value == 0x6a && b->s->fa.pmu6a >= 0) { b->s->fa.n_fijo++; b->regs[W580_PMU][0x71] = b->s->fa.pmu6a; }      /* E8: lectura clavada */
        }
        if (reg != 0x70) { w580_luz_muestra(b->s); }
        return;
    }
    b->regs[slave & 127][reg] = value;
    if (b->idx == 1 && (slave & 127) == 0x40) { w580_luz_chip_w(b->s, reg, value); }      /* pista S5: la rampa del PWM del chip de LED */
    w580_luz_muestra(b->s);
}
static void w580_pmu_tecla(W580State *s, bool abajo)
{
    uint8_t *r = s->i2c[0].regs[W580_PMU];
    abajo = abajo || s->fa.kp_at_on;             /* E8: la línea ON atascada no se suelta */
    if ((r[0x23] & 1) == abajo) { return; }
    r[0x23] = (r[0x23] & ~1) | abajo;
    r[0x21] |= 1;
    w580_irq_update(s);
}
static void w580_pmu_reset(W580State *s)
{
    uint8_t *r = s->i2c[0].regs[W580_PMU];
    w580_bat_avanza(s);
    if (!s->fa.pmu_frio && s->fa.pmu_arrancado) {      /* pista E8 (P1-M1 de AUD4): un reset de la CPU NO es un corte de alimentación del PMU */
        s->fa.pmu_calientes++;
        r[0x23] &= ~1;                                 /* la línea ON la reponen teclas-arranque / teclas-atascadas (w580_perif_reset) */
        qemu_log_mask(LOG_UNIMP, "W580_PMU reset CALIENTE n.o %" PRIu64 ": el PMU conserva sus registros (21=%02x 41=%02x 60=%02x 61=%02x 63=%02x) t=%.3f s\n",
                      s->fa.pmu_calientes, r[0x21], r[0x41], r[0x60], r[0x61], r[0x63], w580_bat_ahora() / 1e9);
        w580_bat_reset_caliente(s);
        return;
    }
    s->fa.pmu_arrancado = true;
    memset(r, 0, 256);
    r[0x20] = s->pmu_id;
    r[0x21] = 1;                 /* causa del encendido: tecla ON (suceso con cerrojo) */
    r[0x24] = r[0x25] = r[0x26] = r[0x27] = 0xff;
    w580_bat_reset(s);           /* pista E7: 0x23 según cargador, USB y batería */
}
static void w580_i2c_reset(W580State *s)
{
    for (unsigned i = 0; i < 2; i++) { w580_i2c_reset1(&s->i2c[i]); }
    w580_pmu_reset(s);
}
static void w580_i2c_init(W580State *s, MemoryRegion *sysmem)
{
    static const hwaddr base[2] = { 0xf9040000, 0xf9070000 };
    for (unsigned i = 0; i < 2; i++) {
        W580I2C *b = &s->i2c[i];
        b->s = s; b->idx = i;
        b->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_i2c_complete, b);
        b->fa_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_i2c_suelta_t, b);
        memory_region_init_io(&b->port, OBJECT(s), &w580_i2c_ops, b, i ? "w580.i2c1" : "w580.i2c0", 8);
        memory_region_add_subregion(sysmem, base[i], &b->port);
    }
    w580_bat_init(s);
}
