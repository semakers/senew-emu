/* Pista E9: MEMORY STICK. El anfitrión 0xFC010000 (IRQ 76) y una tarjeta Memory Stick PRO sobre un fichero imagen.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * De dónde sale cada cosa (docs/memory_stick.md; docs/emulador_estricto.md §15):
 *  - ANFITRIÓN: de cómo lo usa el main ORIGINAL (memstick_pd.c, 0x4422C6F8..0x4422D128): +0 orden (TPC << 12 | tamaño),
 *    +4 FIFO de 8 en 8 bytes tras ver DRQ, +8 estado (DRQ 0x4000, MSINT 0x2000, RDY 0x1000, CRC 0x200, TOE 0x100 y, en
 *    paralelo, bits 3..0 = CED, ERR, BREQ, CMDNK de la tarjeta), +C sistema (RST 0x8000 que se quita solo, SRAC 0x4000 =
 *    serie, INTEN 0x2000, INTCLR 0x0800 acuse). [de memoria] el mismo cuarteto y los mismos bits que el anfitrión
 *    «tifm_ms» de Linux (TI FlashMedia): es el bloque MSHC de Sony. Lo que el original no usa NO se modela (lee 0).
 *  - TARJETA: del estándar Memory Stick PRO [de memoria: mspro_block.c de Linux] y de lo que el original le pide:
 *    registros 0x00..0x1F (0x01 INT, 0x02 STATUS0 bit 0 = protegida, 0x04 tipo = 1, 0x06 categoría = 0, 0x07 clase = 0,
 *    0x10 parámetro de sistema: 0x80 serie, 0x00 paralelo de 4 bits), TPC 2/4/7/8/9/B/D/E, órdenes 0x20 leer, 0x21 escribir,
 *    0x24 atributos, 0x25 parar; 0x10 (formato) se RECHAZA siempre. Sector de 512, dirección = LBA.
 *  - HIPÓTESIS del modelo (lo que el original no distingue y solo dirá el aparato), todas [inferido]:
 *      H1 RDY/CRC/TOE/MSINT se pueden LEER con INTEN a 0 (sondeo) y se borran escribiendo INTCLR; INTCLR se lee a 0.
 *      H2 arrancar un TPC borra RDY/CRC/TOE (no MSINT).
 *      H3 MSINT es el flanco de subida de la INT de la tarjeta, en serie y en paralelo (también el CED del encendido);
 *         pasar a paralelo con la INT ya alta NO da flanco.
 *      H4 la INT de la tarjeta nunca coincide con el RDY del TPC que la provoca: llega después (W580_MS_T_*), y con
 *         INTEN a 1 el modelo la APLAZA hasta que el firmware haya acusado el RDY (el proceso de interrupción del
 *         original mira MSINT antes que RDY: si coincidieran perdería el «terminada»; en el aparato lo impide que la
 *         tarjeta tarda mucho más que la interrupción, aquí el reloj virtual no lo garantiza).
 *      H5 sin tarjeta, sin tensión, o con anfitrión y tarjeta en distinto modo (serie/paralelo): TOE.
 *      H6 el valor de +C tras RST: [sin saber]; el modelo conserva los demás bits (el original siempre lee-modifica-escribe).
 *      H7 los tiempos (W580_MS_T_*) son mínimos razonables, no medidas.
 *  - LA IMAGEN: un fichero crudo (MBR + FAT; tools/e9_ms_imagen.py). Se lee y se escribe POR SECTORES con pread/pwrite:
 *    nunca se carga en memoria. Con ms-solo-lectura=on el fichero se abre solo para leer y lo escrito vive en una capa
 *    en memoria (hasta W580_MS_CAPA_MAX sectores) que se pierde al salir: la tarjeta, para el firmware, SÍ se deja escribir.
 *
 * Propiedades (todas al lanzar; las marcadas (c) también en caliente):
 *   ms=on                  el anfitrión se modela aunque no haya tarjeta (sin ella todo TPC da TOE). Lo pone ms-imagen.
 *   ms-imagen=RUTA   (c)   fichero imagen. Al lanzar: la tarjeta ARRANCA PUESTA (salvo ms-puesta=off). En caliente solo
 *                          con la tarjeta fuera (cambia de tarjeta; la capa de solo lectura se tira).
 *   ms-puesta=on|off (c)   meter / sacar la tarjeta: mueve la entrada de GPIO P3.4 (0 = puesta; es gpio-in=3.4=V, con su
 *                          IRQ de GPIO) y conecta o desconecta la tarjeta del anfitrión.
 *   ms-solo-lectura=on|off el FICHERO no se toca (capa temporal). Solo con la tarjeta fuera.
 *   ms-protegida=on|off    la TARJETA se declara protegida (STATUS0 bit 0 y tipo de dispositivo 1) y rechaza 0x21.
 *   ms-sondeo-vale=on|off  estricto: no avisar de los TPC con INTEN a 0 (el sondeo es la hipótesis H1, no lo que hace el original).
 *   ms-hipotesis=LISTA (c) da la vuelta a hipótesis que el original no distingue, para probar un controlador contra las dos caras:
 *                          sin-sondeo (lo contrario de H1: con INTEN a 0 el registro +8 no enseña RDY/CRC/TOE/MSINT; DRQ y los bits
 *                          3..0 sí) y sin-msint-serie (lo contrario de H3: en serie la INT de la tarjeta no da MSINT, ni la del CED
 *                          del encendido); separadas por ':'; «nada» las quita. El original monta la tarjeta con cualquiera de ellas.
 *   gpio-in=3.4=V    (c)   con el modelo puesto, forzar P3.4 por la propiedad de siempre TAMBIÉN mete (0) o saca (1) la tarjeta: es
 *                          lo mismo que ms-puesta (sirve a los guiones que solo saben poner propiedades de texto).
 *   ms-estado (lectura)    todo lo anterior, las cuentas y los tramos escritos.
 * Sin ms ni ms-imagen la máquina hace EXACTAMENTE lo de antes: FC01000x sigue en el cerrojo y P3.4 sin forzar. */
#define W580_MS_DRQ    0x4000u
#define W580_MS_MSINT  0x2000u
#define W580_MS_RDY    0x1000u
#define W580_MS_CRC    0x0200u
#define W580_MS_TOE    0x0100u
#define W580_MS_RST    0x8000u
#define W580_MS_SRAC   0x4000u
#define W580_MS_INTEN  0x2000u
#define W580_MS_INTCLR 0x0800u
#define W580_MS_CED 0x80
#define W580_MS_ERR 0x40
#define W580_MS_BREQ 0x20
#define W580_MS_CMDNK 0x01
/* tiempos [inferido: mínimos razonables; H7] */
#define W580_MS_T_CED     INT64_C(2000000)   /* de dar tensión al CED (el original espera 14 ms antes del primer TPC) */
#define W580_MS_T_ORDEN   INT64_C(300000)    /* de una orden (EX_SET_CMD) a su primera INT */
#define W580_MS_T_SECTOR  INT64_C(150000)    /* entre sectores al leer */
#define W580_MS_T_ESCRIBE INT64_C(500000)    /* entre sectores al escribir */
#define W580_MS_T_TOE     INT64_C(20000)     /* lo que tarda el anfitrión en rendirse si nadie contesta */
#define W580_MS_T_APLAZO  INT64_C(50000)     /* H4: reintento de la INT aplazada */
#define W580_MS_APLAZOS   400                /* H4: tope (20 ms) */
#define W580_MS_T_TENSION INT64_C(10000000)  /* estricto: el original espera 14 ms; se exigen 10 */
#define W580_MS_CAPA_MAX  65536u             /* 32 MiB de capa de solo lectura */
#define W580_MS_ATRB_SECT 64u                /* área de atributos: 0x8000 bytes */
enum { W580_MS_F_NADA, W580_MS_F_LEE, W580_MS_F_ESCRIBE, W580_MS_F_ATRB };
typedef struct W580Ms W580Ms;

static int64_t w580_ms_ahora(void) { return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); }
static void G_GNUC_PRINTF(2, 3) w580_ms_log(W580State *s, const char *fmt, ...)
{
    if (s->ms.log >= 3000) { return; }
    s->ms.log++;
    va_list ap; va_start(ap, fmt);
    g_autofree char *m = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_UNIMP, "W580_MS %s t=%.6f s\n", m, w580_ms_ahora() / 1e9);
}
static uint8_t *w580_ms_pmu_regs(W580State *s) { return s->i2c[0].regs[W580_PMU]; }
static bool w580_ms_tension(W580State *s) { return (w580_ms_pmu_regs(s)[0x43] & 3) != 0; }
static bool w580_ms_hay(W580State *s) { return s->ms.puesta && s->ms.fd >= 0; }

/* ---------------------------------------------------------------- la imagen, por sectores */
static void w580_ms_sector_lee(W580State *s, uint64_t lba, uint8_t *b)
{
    W580Ms *m = &s->ms;
    memset(b, 0, 512);
    if (m->capa) {
        uint8_t *p = g_hash_table_lookup(m->capa, GUINT_TO_POINTER((guint)lba + 1));
        if (p) { memcpy(b, p, 512); return; }
    }
    for (size_t h = 0; h < 512; ) {
        ssize_t r = pread(m->fd, b + h, 512 - h, (off_t)lba * 512 + h);
        if (r <= 0) { if (r < 0 && errno == EINTR) { continue; } break; }
        h += r;
    }
}
static bool w580_ms_sector_escribe(W580State *s, uint64_t lba, const uint8_t *b)
{
    W580Ms *m = &s->ms;
    if (m->solo_lectura) {
        if (!m->capa) { m->capa = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free); }
        uint8_t *p = g_hash_table_lookup(m->capa, GUINT_TO_POINTER((guint)lba + 1));
        if (!p) {
            if (m->capa_n >= W580_MS_CAPA_MAX) { return false; }
            p = g_malloc(512); m->capa_n++;
            g_hash_table_insert(m->capa, GUINT_TO_POINTER((guint)lba + 1), p);
        }
        memcpy(p, b, 512);
        return true;
    }
    for (size_t h = 0; h < 512; ) {
        ssize_t r = pwrite(m->fd, b + h, 512 - h, (off_t)lba * 512 + h);
        if (r <= 0) { if (r < 0 && errno == EINTR) { continue; } return false; }
        h += r;
    }
    return true;
}
/* Área de atributos, generada del tamaño de la imagen. Solo el sector 0 lleva algo: firma A5C3, una entrada (id 0x10,
 * información del sistema, 0x60 bytes en 0x1A0). Campos de la información del sistema: los que lee el original
 * (docs/memory_stick.md §2.5) y, [de memoria] de struct mspro_sys_info, tamaño de página (+8), tipo de interfaz (+0x33). */
static void w580_ms_atributos(W580State *s, uint32_t sector, uint8_t *b)
{
    W580Ms *m = &s->ms;
    memset(b, 0, 512);
    if (sector) { return; }
    uint32_t bloques = m->sectores / m->bloque;
    b[0] = 0xa5; b[1] = 0xc3; b[4] = 1;
    uint8_t *e = b + 0x10, *i = b + 0x1a0;
    e[2] = 0x01; e[3] = 0xa0; e[7] = 0x60; e[8] = 0x10;
    i[0] = 2;                                           /* clase: PRO */
    i[2] = m->bloque >> 8; i[3] = m->bloque;            /* tamaño de bloque, en unidades */
    i[4] = bloques >> 8; i[5] = bloques;                /* bloques totales */
    i[6] = bloques >> 8; i[7] = bloques;                /* bloques de usuario */
    i[8] = 0x02;                                        /* tamaño de página 0x200 [de memoria] */
    i[0x2c] = 0x02;                                     /* tamaño de unidad 0x200 */
    i[0x33] = 1;                                        /* tipo de interfaz: admite paralelo [de memoria] */
    i[0x38] = m->protegida ? 1 : 0;                     /* tipo de dispositivo: 0 = lectura y escritura */
}

/* ---------------------------------------------------------------- IRQ 76 y la INT de la tarjeta */
static bool w580_ms_irq(W580State *s)
{
    return (s->ms.sys & W580_MS_INTEN) && (s->ms.sta & (W580_MS_RDY | W580_MS_MSINT));
}
/* IRQ 76 = banco 2 bit 16. El original la configura POR NIVEL («ti_intcon … level:9,12,57,75,76,77», ILR = 0x7E): entonces
 * lo pendiente es la línea y nada más. w580_irq_linea() guarda además el flanco en el cerrojo del controlador, que para una
 * fuente de nivel nadie borra [observado en la primera pasada: 13 241 interrupciones seguidas por un solo MSINT]. */
static void w580_ms_linea(W580State *s)
{
    if (s->irq_config[2][16] & 2) {
        if (w580_ms_irq(s)) { s->irq_level[2] |= 1u << 16; } else { s->irq_level[2] &= ~(1u << 16); }
    } else {
        w580_irq_linea(s, 2, 16, w580_ms_irq(s));
    }
}
static void w580_ms_int_sube(void *opaque)
{
    W580State *s = opaque; W580Ms *m = &s->ms;
    if (!w580_ms_hay(s) || !m->viva) { return; }
    /* H4: no antes de que el TPC haya terminado ni, con INTEN, de que el firmware haya acusado su RDY */
    if ((m->activo || ((m->sys & W580_MS_INTEN) && (m->sta & W580_MS_RDY))) && m->aplazos < W580_MS_APLAZOS) {
        m->aplazos++; timer_mod(m->t_int, w580_ms_ahora() + W580_MS_T_APLAZO); return;
    }
    bool antes = m->intr != 0;
    m->intr = m->intr_sig; m->reg[1] = m->intr;
    if (!antes && m->intr && !(m->sin_msint_serie && (m->sys & W580_MS_SRAC))) { m->sta |= W580_MS_MSINT; m->n_msint++; }      /* H3: flanco de subida */
    w580_irq_update(s);
}
/* la tarjeta se pone a trabajar: INT a 0 y, al cabo de ns, vale `luego` */
static void w580_ms_int_luego(W580State *s, uint8_t luego, int64_t ns)
{
    W580Ms *m = &s->ms;
    m->intr = 0; m->reg[1] = 0; m->intr_sig = luego; m->aplazos = 0;
    timer_mod(m->t_int, w580_ms_ahora() + ns);
}
static void w580_ms_tarjeta_reset(W580State *s)
{
    W580Ms *m = &s->ms;
    timer_del(m->t_int);
    memset(m->reg, 0, sizeof(m->reg)); memset(m->rw, 0, sizeof(m->rw));
    m->rw_puesto = false; m->paralelo = false; m->intr = m->intr_sig = 0; m->fase = W580_MS_F_NADA; m->quedan = 0; m->orden = 0;
    m->reg[2] = m->protegida ? 1 : 0;       /* STATUS0: bit 0 = protegida contra escritura */
    m->reg[4] = 1;                          /* tipo: PRO (lo que exige el original: tipo 1, categoría 0, clase < 4) */
    m->reg[0x10] = 0x80;                    /* parámetro de sistema: serie */
}
/* la tensión del regulador 6 (PMU 0x43 bits 0..1) o la presencia han cambiado */
static void w580_ms_alimenta(W580State *s)
{
    W580Ms *m = &s->ms;
    bool viva = w580_ms_hay(s) && w580_ms_tension(s);
    if (viva == m->viva) { return; }
    m->viva = viva;
    w580_ms_tarjeta_reset(s);
    if (viva) { w580_ms_int_luego(s, W580_MS_CED, W580_MS_T_CED); }
    w580_ms_log(s, "tarjeta %s (PMU 43=%02x 44=%02x)", viva ? "CON tension: CED en 2 ms" : "sin tension", w580_ms_pmu_regs(s)[0x43], w580_ms_pmu_regs(s)[0x44]);
}

/* ---------------------------------------------------------------- la tarjeta: órdenes y TPC */
static void w580_ms_orden(W580State *s, uint8_t cmd, uint16_t cuenta, uint32_t dir)
{
    W580Ms *m = &s->ms;
    m->n_ordenes++; m->orden = cmd;
    w580_ms_log(s, "orden 0x%02x cuenta=%u dir=0x%x%s", cmd, cuenta, dir, m->fase != W580_MS_F_NADA ? " (con otra a medias)" : "");
    m->fase = W580_MS_F_NADA; m->quedan = 0;
    switch (cmd) {
    case 0x20: case 0x21:
        if (cmd == 0x21 && m->protegida) { w580_ms_int_luego(s, W580_MS_CED | W580_MS_CMDNK, W580_MS_T_ORDEN); return; }
        if (!cuenta || (uint64_t)dir + cuenta > m->sectores) { w580_ms_int_luego(s, W580_MS_CED | W580_MS_ERR, W580_MS_T_ORDEN); return; }
        m->fase = cmd == 0x20 ? W580_MS_F_LEE : W580_MS_F_ESCRIBE; m->lba = dir; m->quedan = cuenta;
        if (cmd == 0x21) {
            if (!m->escritos) { m->escritos = g_string_new(NULL); }
            if (m->escritos->len < 8000) { g_string_append_printf(m->escritos, " %u+%u", dir, cuenta); }
            qemu_log_mask(LOG_UNIMP, "W580_MS ESCRIBE lba=%u cuenta=%u t=%.6f s\n", dir, cuenta, w580_ms_ahora() / 1e9);
        }
        w580_ms_int_luego(s, W580_MS_BREQ, W580_MS_T_ORDEN);
        return;
    case 0x24: {
        /* [observado] el original da la dirección EN BYTES (sector << 9, FUN_44779460); [de memoria] Linux, en sectores.
         * [sin saber]. Solo el sector 0 lleva algo, y 0 es 0 en las dos. Un múltiplo de 512 se toma como bytes. */
        uint32_t sec = (dir & 0x1ff) ? dir : dir >> 9;
        if (!cuenta || sec + cuenta > W580_MS_ATRB_SECT) { w580_ms_int_luego(s, W580_MS_CED | W580_MS_ERR, W580_MS_T_ORDEN); return; }
        m->fase = W580_MS_F_ATRB; m->lba = sec; m->quedan = cuenta;
        w580_ms_int_luego(s, W580_MS_BREQ, W580_MS_T_ORDEN);
        return;
    }
    case 0x25:      /* parar [de memoria]; el original no la manda nunca */
        w580_ms_int_luego(s, W580_MS_CED, W580_MS_T_ORDEN);
        return;
    case 0x10:
        qemu_log_mask(LOG_GUEST_ERROR, "W580_MS FORMATO (orden 0x10) RECHAZADO: el modelo no formatea nunca la imagen (pc=0x%08x)\n", s->cpu->env.regs[15]);
        /* fallthrough */
    default:
        w580_ms_int_luego(s, W580_MS_CED | W580_MS_CMDNK, W580_MS_T_ORDEN);
    }
}
/* Un TPC llega a la tarjeta. Lectura: rellena b[0..n). false = la tarjeta no contesta (TOE). */
static bool w580_ms_tarjeta_tpc(W580State *s, unsigned tpc, uint8_t *b, unsigned n)
{
    W580Ms *m = &s->ms;
    if (!w580_ms_hay(s) || !m->viva) { return false; }
    if (m->paralelo != !(m->sys & W580_MS_SRAC)) { return false; }       /* H5: cada uno en un modo */
    switch (tpc) {
    case 8:         /* SET_RW_REG_ADRS: dirección y tamaño de lectura, dirección y tamaño de escritura */
        if (n < 4) { return false; }
        memcpy(m->rw, b, 4); m->rw_puesto = true;
        return true;
    case 4:         /* READ_REG */
        for (unsigned i = 0; i < n; i++) { unsigned a = m->rw[0] + i; b[i] = (i < m->rw[1] && a < sizeof(m->reg)) ? m->reg[a] : 0xff; }
        return true;
    case 0xb:       /* WRITE_REG */
        for (unsigned i = 0; i < n && i < m->rw[3]; i++) {
            unsigned a = m->rw[2] + i;
            if (a < 0x10 || a >= sizeof(m->reg)) { continue; }           /* 0x00..0x0F: de solo lectura */
            m->reg[a] = b[i];
            if (a == 0x10) {
                m->paralelo = (b[i] & 0xc0) == 0;
                w580_ms_log(s, "parametro de sistema (reg 0x10) <- 0x%02x: tarjeta en %s", b[i], m->paralelo ? "PARALELO de 4 bits" : (b[i] & 0xc0) == 0x80 ? "serie" : "un modo que el anfitrion no tiene");
                if ((b[i] & 0xc0) == 0x40) { m->paralelo = false; }
            }
        }
        return true;
    case 7:         /* GET_INT */
        if (n) { b[0] = m->intr; }
        for (unsigned i = 1; i < n; i++) { b[i] = 0xff; }
        return true;
    case 9:         /* EX_SET_CMD: orden, cuenta (BE16), dirección (BE32) */
        if (n < 7) { return false; }
        memcpy(m->reg + 0x11, b + 1, 6);
        w580_ms_orden(s, b[0], b[1] << 8 | b[2], (uint32_t)b[3] << 24 | b[4] << 16 | b[5] << 8 | b[6]);
        return true;
    case 0xe:       /* SET_CMD: la cuenta y la dirección, de los registros 0x11..0x16 */
        if (n < 1) { return false; }
        w580_ms_orden(s, b[0], m->reg[0x11] << 8 | m->reg[0x12], (uint32_t)m->reg[0x13] << 24 | m->reg[0x14] << 16 | m->reg[0x15] << 8 | m->reg[0x16]);
        return true;
    case 2:         /* READ_LONG_DATA: un sector */
        if (!(m->intr & W580_MS_BREQ) || !m->quedan || (m->fase != W580_MS_F_LEE && m->fase != W580_MS_F_ATRB)) { return false; }
        {
            uint8_t sec[512];
            if (m->fase == W580_MS_F_ATRB) { w580_ms_atributos(s, m->lba, sec); } else { w580_ms_sector_lee(s, m->lba, sec); m->n_leidos++; }
            memcpy(b, sec, MIN(n, 512u));
            if (n > 512) { memset(b + 512, 0xff, n - 512); }
        }
        m->lba++; m->quedan--;
        if (!m->quedan) { m->fase = W580_MS_F_NADA; }
        w580_ms_int_luego(s, m->quedan ? W580_MS_BREQ : W580_MS_CED, W580_MS_T_SECTOR);
        return true;
    case 0xd:       /* WRITE_LONG_DATA: un sector */
        if (!(m->intr & W580_MS_BREQ) || !m->quedan || m->fase != W580_MS_F_ESCRIBE || n < 512) { return false; }
        {
            bool bien = w580_ms_sector_escribe(s, m->lba, b);
            if (m->n_escritos < 600) {       /* qué escribe el firmware: el sector y sus 8 primeros bytes (en el primero de una FAT, FAT[0] y FAT[1]) */
                qemu_log_mask(LOG_UNIMP, "W580_MS sector escrito lba=%u primeros=%02x%02x%02x%02x%02x%02x%02x%02x\n", m->lba, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            }
            m->n_escritos++; m->lba++; m->quedan--;
            if (!bien) {
                qemu_log_mask(LOG_UNIMP, "W580_MS no se pudo escribir el sector %u (capa llena o fallo del fichero)\n", m->lba - 1);
                m->fase = W580_MS_F_NADA; m->quedan = 0;
                w580_ms_int_luego(s, W580_MS_CED | W580_MS_ERR, W580_MS_T_ESCRIBE);
                return true;
            }
        }
        if (!m->quedan) { m->fase = W580_MS_F_NADA; }
        w580_ms_int_luego(s, m->quedan ? W580_MS_BREQ : W580_MS_CED, W580_MS_T_ESCRIBE);
        return true;
    default:        /* 3 y 0xC (datos cortos: solo los usa el formato) y lo demás: la tarjeta del modelo no contesta */
        return false;
    }
}

/* ---------------------------------------------------------------- reglas [ms] del modo estricto (docs/emulador_estricto.md §15) */
static bool w580_ms_es_bloque_vivo(W580State *s, bool *p22, bool *p23, bool *rst)
{
    unsigned c = w580_es_l16(s, 0xf909001a), h = w580_es_l16(s, 0xf9090014);
    bool a = !(c & 0x0100), b = !(c & 0x0080), r = (h & 0x0080) != 0;
    if (p22) { *p22 = a; } if (p23) { *p23 = b; } if (rst) { *rst = r; }
    return a && b && r;
}
static void w580_ms_es_acceso(W580State *s, hwaddr addr, unsigned size, bool write)
{
    if (!w580_es.on || !current_cpu) { return; }
    bool p22, p23, vivo;
    if (!w580_ms_es_bloque_vivo(s, &p22, &p23, &vivo)) {
        w580_es_aviso(s, "ms", 0x100 | (p22 ? 0 : 1) | (p23 ? 0 : 2) | (vivo ? 0 : 4) | (write ? 8 : 0),
                      "%s de 0x%08" HWADDR_PRIx " (anfitrión del Memory Stick) con%s%s%s: el original abre las puertas 22 y 23 (F909001A bits 8 y 7 a 0) y saca el "
                      "bloque 18 de reset (F9090014 bit 7 a 1) antes de tocarlo (MemStickPD_Init, 0x4422C71E..C738)", write ? "escritura" : "lectura", addr,
                      p22 ? "" : " la puerta 22 cerrada", p23 ? "" : " la puerta 23 cerrada", vivo ? "" : " el bloque 18 en reset");
    }
    if (!(w580_es_l16(s, 0xf9090040) & 2)) {
        w580_es_aviso(s, "ms", 0x110, "acceso al anfitrión del Memory Stick sin F9090040 bit 1: el original lo pone lo primero (0x4422C712); qué hace: [sin saber]");
    }
    if (size != 4 || (addr & 3)) {
        w580_es_aviso(s, "ms", 0x120, "acceso de %u bytes a 0x%08" HWADDR_PRIx ": el original solo accede a los cuatro registros con palabras de 32 bits", size, addr);
    }
}
/* tamaño que el original fuerza para cada TPC (docs/memory_stick.md §1.2): 0 = el que pida; -1 = no lo manda nunca */
static int w580_ms_es_tam(unsigned tpc)
{
    switch (tpc) {
    case 2: case 0xd: return 0x200;
    case 7: case 0xe: return 1;
    case 8: return 4;
    case 9: return 7;
    case 3: case 4: case 0xb: case 0xc: return 0;
    default: return -1;
    }
}
static void w580_ms_es_tpc(W580State *s, unsigned tpc, unsigned n)
{
    W580Ms *m = &s->ms;
    if (!w580_es.on) { return; }
    uint8_t *r = w580_ms_pmu_regs(s);
    int tam = w580_ms_es_tam(tpc);
    if (tam < 0) {
        w580_es_aviso(s, "ms", 0x200 | tpc, "TPC 0x%X: el original solo manda 2, 3, 4, 7, 8, 9, 0xB, 0xC, 0xD y 0xE (FUN_4422cf80)", tpc);
    } else if ((tam && n != (unsigned)tam) || (tpc == 3 && (n < 0x20 || n > 0x100)) || !n) {
        w580_es_aviso(s, "ms", 0x210 | tpc, "TPC 0x%X con tamaño %u: el original lo manda siempre con %s", tpc, n,
                      tpc == 2 || tpc == 0xd ? "0x200" : tpc == 7 || tpc == 0xe ? "1" : tpc == 8 ? "4" : tpc == 9 ? "7" : tpc == 3 ? "0x20..0x100" : "un tamaño distinto de 0");
    }
    if (!(r[0x43] & 3)) {
        w580_es_aviso(s, "ms", 0x220, "TPC 0x%X sin tensión en la tarjeta (PMU 0x43 bits 0..1 = 0): el original da antes SetVoltage(6,4) y SetMode(6,3) "
                      "(0x44 <- xE, 0x43 <- x7; 0x4422C77A..C7A4)", tpc);
    } else {
        if ((r[0x43] & 3) != 3 || (r[0x44] & 3) != 2) {
            w580_es_aviso(s, "ms", 0x230, "TPC 0x%X con el regulador 6 en un valor que el original no usa (PMU 0x43 bits 0..1 = %u, 0x44 bits 0..1 = %u; el original: 3 y 2). "
                          "Qué voltios es cada código: [sin saber]", tpc, r[0x43] & 3, r[0x44] & 3);
        }
        int64_t d = w580_ms_ahora() - m->t_tension;
        if (d < W580_MS_T_TENSION) {
            w580_es_aviso(s, "ms", 0x240, "TPC 0x%X a %.2f ms de dar tensión a la tarjeta: el original espera 14 ms (delay(0xe), MemStickLD_BuildStickInfo) y aquí se exigen %d",
                          tpc, d / 1e6, (int)(W580_MS_T_TENSION / 1000000));
        }
    }
    if (w580_es.mhz != 208) {
        w580_es_aviso(s, "ms", 0x2e0, "TPC 0x%X con la CPU a %u MHz: el original tiene pedidos 208 MHz mientras la tarjeta está encendida (puerta 23 en modo 4, 0x4422C730; "
                      "en la calibración, los 1126 TPC con F909001C = 0x0401). [sin saber] de qué reloj sale el bus del Memory Stick ni si anda a otra frecuencia", tpc, w580_es.mhz);
    }
    if (!m->reseteado) {
        w580_es_aviso(s, "ms", 0x250, "TPC 0x%X sin haber reiniciado el anfitrión (+C bit 15) desde que el bloque salió de reset: el original lo hace dos veces "
                      "nada más abrirlo (0x4422C74E..C76C)", tpc);
    }
    if ((m->sys & 7) != 5) {
        w580_es_aviso(s, "ms", 0x260, "TPC 0x%X con +C bits 2..0 = %u: el original pone siempre 5 tras el reset (0x4422C76E); qué es: [inferido] el tope de espera", tpc, m->sys & 7);
    }
    if (m->activo) {
        w580_es_aviso(s, "ms", 0x270, "TPC 0x%X arrancado con el TPC 0x%X todavía a medias (sin RDY): el original espera el fin de cada transacción (WaitDone)", tpc, m->tpc);
    } else if (m->dir == 1 && m->fifo_i < m->fifo_n) {
        w580_es_aviso(s, "ms", 0x271, "TPC 0x%X arrancado con %u bytes del TPC 0x%X sin leer del FIFO: el original lee siempre ceil(n/8) pares de palabras", tpc, m->fifo_n - m->fifo_i, m->tpc);
    } else if (m->dir == 2 && m->fifo_i < m->fifo_n) {
        w580_es_aviso(s, "ms", 0x272, "TPC 0x%X arrancado con el TPC 0x%X esperando aún %u bytes por el FIFO", tpc, m->tpc, m->fifo_n - m->fifo_i);
    }
    if (!(m->sys & W580_MS_INTEN)) {
        m->n_sondeo++;
        if (!m->sondeo_vale) {
            w580_es_aviso(s, "ms", 0x280, "TPC 0x%X con INTEN (+C bit 13) a 0: el original atiende SIEMPRE el fin de transacción y la INT de la tarjeta por la IRQ 76. "
                          "Que RDY/MSINT se puedan sondear es la hipótesis H1 del modelo, no una medida (ms-sondeo-vale=on calla este aviso)", tpc);
        }
    } else { m->n_irq_tpc++; }
    bool par = !(m->sys & W580_MS_SRAC);
    if (w580_ms_hay(s) && m->viva && par != m->paralelo) {
        w580_es_aviso(s, "ms", 0x290 | par, "TPC 0x%X con el anfitrión en %s y la tarjeta en %s: el original escribe 0x00 en el registro 0x10 de la tarjeta y ACTO SEGUIDO "
                      "pasa el anfitrión a paralelo (MemStickProLD_SetupStick)", tpc, par ? "paralelo" : "serie", m->paralelo ? "paralelo" : "serie");
    }
    if (par && tpc == 7) {
        w580_es_aviso(s, "ms", 0x2a0, "GET_INT (TPC 7) en paralelo: el original no lo manda; lee CED/ERR/BREQ/CMDNK de los bits 3..0 de +8 (0x4422C984)");
    }
    if ((tpc == 4 && (!m->rw_puesto || n != m->rw[1])) || (tpc == 0xb && (!m->rw_puesto || n != m->rw[3]))) {
        w580_es_aviso(s, "ms", 0x2b0 | tpc, "%s de %u bytes y el último SET_RW_REG_ADRS dice %u: el original exige que coincidan («Invalid size for buffer»)",
                      tpc == 4 ? "READ_REG" : "WRITE_REG", n, m->rw_puesto ? m->rw[tpc == 4 ? 1 : 3] : 0);
    }
    if ((tpc == 2 || tpc == 0xd) && w580_ms_hay(s) && m->viva) {
        if (!(m->intr & W580_MS_BREQ)) {
            w580_es_aviso(s, "ms", 0x2c0 | tpc, "%s sin BREQ pendiente (INT de la tarjeta = 0x%02x): el original espera la INT y mira BREQ antes de cada sector",
                          tpc == 2 ? "READ_LONG_DATA" : "WRITE_LONG_DATA", m->intr);
        } else if ((tpc == 2) != (m->fase != W580_MS_F_ESCRIBE)) {
            w580_es_aviso(s, "ms", 0x2d0 | tpc, "%s durante una orden de %s", tpc == 2 ? "READ_LONG_DATA" : "WRITE_LONG_DATA", tpc == 2 ? "escritura (0x21)" : "lectura");
        }
    }
}
static void w580_ms_es_orden(W580State *s, uint8_t cmd, uint16_t cuenta, uint32_t dir)
{
    W580Ms *m = &s->ms;
    if (!w580_es.on || !w580_ms_hay(s) || !m->viva) { return; }
    if (cmd == 0x10) {
        w580_es_aviso(s, "ms", 0x310, "ORDEN 0x10 = FORMATO INTERNO DE LA TARJETA: en el aparato BORRA la tarjeta. El modelo la rechaza. No mandarla nunca");
    } else if (cmd != 0x20 && cmd != 0x21 && cmd != 0x24) {
        w580_es_aviso(s, "ms", 0x3000 | cmd, "orden 0x%02x a la tarjeta: el original solo manda 0x20 (leer), 0x21 (escribir), 0x24 (atributos) y 0x10 (formato)", cmd);
    }
    if (cmd == 0x21 && m->protegida) {
        w580_es_aviso(s, "ms", 0x320, "orden de escritura (0x21) a una tarjeta protegida: el original no escribe si STATUS0 bit 0, la clase o el tipo de dispositivo dicen solo lectura");
    }
    if ((cmd == 0x20 || cmd == 0x21) && (!cuenta || (uint64_t)dir + cuenta > m->sectores)) {
        w580_es_aviso(s, "ms", 0x330, "orden 0x%02x de %u sectores desde el %u: se sale de la tarjeta (%" PRIu64 " sectores)", cmd, cuenta, dir, m->sectores);
    }
    if (cmd == 0x24 && (dir || cuenta != 1)) {
        w580_es_aviso(s, "ms", 0x340, "atributos (0x24) con dirección 0x%x y cuenta %u: la unidad de la dirección es dudosa (el original da BYTES; Linux, de memoria, sectores) "
                      "y el modelo solo tiene contenido en el sector 0", dir, cuenta);
    }
    if (!(m->intr & W580_MS_CED) || m->fase != W580_MS_F_NADA) {
        w580_es_aviso(s, "ms", 0x350, "orden 0x%02x sin haber llegado al CED de la anterior (INT = 0x%02x, quedan %u sectores de la orden 0x%02x): el original espera siempre al CED "
                      "y no manda nunca la orden de parar", cmd, m->intr, m->quedan, m->orden);
    }
}
/* estricto: escrituras de SYSCON (desde w580_es_syscon_escribe, ANTES de que el valor llegue al cerrojo) */
static void w580_ms_es_syscon(W580State *s, uint32_t addr, uint32_t value)
{
    W580Ms *m = &s->ms;
    if (!m->modelo) { return; }
    unsigned antes = w580_es_l16(s, addr);
    if (addr == 0xf9090014 && !(value & 0x80) && (antes & 0x80)) { m->reseteado = false; }
    if (!w580_es.on || !w580_ms_tension(s)) { return; }
    if (addr == 0xf909001a && (value & ~antes & 0x0180)) {
        w580_es_aviso(s, "ms", 0x400, "se cierra la puerta %s del Memory Stick (F909001A = 0x%04x) con la tarjeta en tensión: el original quita antes la tensión "
                      "(SetMode(6,0), MemStickPD_Shutdown 0x4422C818)", (value & ~antes & 0x100) ? "22" : "23", value);
    }
    if (addr == 0xf9090014 && !(value & 0x80) && (antes & 0x80)) {
        w580_es_aviso(s, "ms", 0x410, "el bloque 18 (Memory Stick) a reset (F9090014 = 0x%04x) con la tarjeta en tensión: el original quita antes la tensión", value);
    }
}
/* escrituras del PMU 0x43 / 0x44 (regulador 6), tras guardarse */
static void w580_ms_pmu(W580State *s, uint8_t reg, uint8_t antes, uint8_t value)
{
    W580Ms *m = &s->ms;
    if (!m->modelo || (reg != 0x43 && reg != 0x44)) { return; }
    if (reg == 0x43 && !(antes & 3) && (value & 3)) {
        m->t_tension = w580_ms_ahora();
        if (w580_es.on) {
            uint8_t v44 = w580_ms_pmu_regs(s)[0x44];
            if ((value & 3) != 3 || (v44 & 3) != 2) {
                w580_es_aviso(s, "ms", 0x500, "se da tensión a la tarjeta con PMU 0x43 bits 0..1 = %u y 0x44 bits 0..1 = %u: el original solo usa 3 y 2 (SetMode(6,3) tras "
                              "SetVoltage(6,4)). Otro código puede ser OTRA TENSIÓN: [sin saber] cuántos voltios es cada uno", value & 3, v44 & 3);
            }
            if (!w580_ms_es_bloque_vivo(s, NULL, NULL, NULL) || !m->reseteado) {
                w580_es_aviso(s, "ms", 0x510, "se da tensión a la tarjeta sin el anfitrión listo (puertas 22 y 23, bloque 18 fuera de reset y +C bit 15 dos veces): "
                              "el original prepara el bloque ANTES de la tensión (0x4422C712..C778)");
            }
        }
    }
    if (reg == 0x44 && w580_es.on && (w580_ms_pmu_regs(s)[0x43] & 3) && ((antes ^ value) & 3)) {
        w580_es_aviso(s, "ms", 0x520, "se cambia el código de tensión del regulador 6 (PMU 0x44 bits 0..1: %u -> %u) con la tarjeta en tensión: el original lo fija antes de encender",
                      antes & 3, value & 3);
    }
    w580_ms_alimenta(s);
}

/* ---------------------------------------------------------------- el anfitrión */
static void w580_ms_fin(void *opaque)
{
    W580State *s = opaque; W580Ms *m = &s->ms;
    if (!m->activo) { return; }
    m->activo = false;
    m->sta |= W580_MS_RDY | m->res; m->n_rdy++;
    if (m->res & W580_MS_TOE) { m->n_toe++; w580_ms_log(s, "TPC 0x%X n=%u: nadie contesta (TOE)", m->tpc, m->n); }
    w580_irq_update(s);
}
static int64_t w580_ms_dura(W580State *s, unsigned n)
{
    /* H7: 20 MHz y 1 bit en serie (400 ns por byte), 40 MHz y 4 bits en paralelo (50 ns por byte) [de memoria los topes del estándar] */
    return 2000 + (int64_t)n * ((s->ms.sys & W580_MS_SRAC) ? 400 : 50);
}
static void w580_ms_host_reset(W580State *s)
{
    W580Ms *m = &s->ms;
    timer_del(m->t_fin);
    m->activo = false; m->dir = 0; m->fifo_n = m->fifo_i = 0; m->res = 0;
    m->sta = 0;
}
static void w580_ms_tpc_entrega(W580State *s)      /* TPC de escritura: ya están todos los bytes */
{
    W580Ms *m = &s->ms;
    m->dir = 0;
    if (m->tpc == 9) { w580_ms_es_orden(s, m->fifo[0], m->fifo[1] << 8 | m->fifo[2], (uint32_t)m->fifo[3] << 24 | m->fifo[4] << 16 | m->fifo[5] << 8 | m->fifo[6]); }
    bool bien = w580_ms_tarjeta_tpc(s, m->tpc, m->fifo, m->n);
    m->res = bien ? 0 : W580_MS_TOE;
    timer_mod(m->t_fin, w580_ms_ahora() + (bien ? w580_ms_dura(s, m->n) : W580_MS_T_TOE));
}
static void w580_ms_tpc(W580State *s, uint32_t v)
{
    W580Ms *m = &s->ms;
    unsigned tpc = (v >> 12) & 15, n = v & 0x3ff;
    w580_ms_es_tpc(s, tpc, n);
    timer_del(m->t_fin);
    m->sta &= ~(W580_MS_RDY | W580_MS_CRC | W580_MS_TOE);          /* H2 */
    m->tpc = tpc; m->n = n; m->activo = true; m->res = 0; m->fifo_i = 0; m->fifo_n = (n + 7) & ~7u; m->n_tpc[tpc]++;
    memset(m->fifo, 0, sizeof(m->fifo));
    bool escribe = tpc & 8;                                        /* 8, 9, B, C, D, E escriben; 2, 3, 4, 7 leen (el bit 3 del TPC) */
    if (escribe) {
        m->dir = 2;
        if (!n) { w580_ms_tpc_entrega(s); }
    } else {
        bool bien = w580_ms_tarjeta_tpc(s, tpc, m->fifo, n);
        m->res = bien ? 0 : W580_MS_TOE;
        m->dir = bien && n ? 1 : 0;
        if (!bien) { m->fifo_n = 0; }
        timer_mod(m->t_fin, w580_ms_ahora() + (bien ? w580_ms_dura(s, n) : W580_MS_T_TOE));
    }
    w580_irq_update(s);
}
static uint32_t w580_ms_lee(W580State *s, hwaddr addr, unsigned size)
{
    W580Ms *m = &s->ms;
    w580_ms_es_acceso(s, addr, size, false);
    switch (addr & 0xc) {
    case 4:
        if (m->dir != 1 || m->fifo_i >= m->fifo_n) {
            if (w580_es.on) { w580_es_aviso(s, "ms", 0x600, "lectura del FIFO (+4) sin DRQ: no hay dato (el original sondea +8 bit 14 antes de cada par de palabras). Lee 0"); }
            return 0;
        }
        {
            uint32_t v = ldl_le_p(m->fifo + m->fifo_i);
            m->fifo_i += 4;
            if (m->fifo_i >= m->fifo_n) { m->dir = 0; }
            return v;
        }
    case 8: {
        uint32_t v = m->sta;
        if (m->sin_sondeo && !(m->sys & W580_MS_INTEN)) { v = 0; }       /* ms-hipotesis=sin-sondeo: sin INTEN no se ve RDY/CRC/TOE/MSINT (lo contrario de H1) */
        if (m->dir && m->fifo_i < m->fifo_n) { v |= W580_MS_DRQ; }
        if (!(m->sys & W580_MS_SRAC) && w580_ms_hay(s) && m->viva && m->paralelo) {       /* en paralelo: la INT de la tarjeta, viva */
            v |= (m->intr & W580_MS_CED ? 8 : 0) | (m->intr & W580_MS_ERR ? 4 : 0) | (m->intr & W580_MS_BREQ ? 2 : 0) | (m->intr & W580_MS_CMDNK ? 1 : 0);
        }
        return v;
    }
    case 0xc: return m->sys;
    default: return 0;       /* +0 es de solo escritura */
    }
}
static void w580_ms_escribe(W580State *s, hwaddr addr, unsigned size, uint32_t value)
{
    W580Ms *m = &s->ms;
    w580_ms_es_acceso(s, addr, size, true);
    switch (addr & 0xc) {
    case 0: w580_ms_tpc(s, value); break;
    case 4:
        if (m->dir != 2 || m->fifo_i >= m->fifo_n) {
            if (w580_es.on) { w580_es_aviso(s, "ms", 0x610, "escritura del FIFO (+4) sin DRQ: nadie la espera (el original sondea +8 bit 14 antes de cada par de palabras). Se pierde"); }
            break;
        }
        stl_le_p(m->fifo + m->fifo_i, value);
        m->fifo_i += 4;
        if (m->fifo_i >= m->fifo_n) { w580_ms_tpc_entrega(s); }
        break;
    case 0xc:
        if (value & W580_MS_RST) {          /* se quita solo; el anfitrión queda en reposo (H6: los demás bits se conservan) */
            w580_ms_host_reset(s); m->n_reset++;
            if (w580_ms_es_bloque_vivo(s, NULL, NULL, NULL) || !w580_es_activo()) { m->reseteado = true; }
        }
        if (value & W580_MS_INTCLR) { m->sta &= ~(W580_MS_RDY | W580_MS_CRC | W580_MS_TOE | W580_MS_MSINT); }      /* H1: acuse */
        {
            uint32_t antes = m->sys;
            m->sys = value & ~(W580_MS_RST | W580_MS_INTCLR);
            if ((antes ^ m->sys) & W580_MS_SRAC) { w580_ms_log(s, "anfitrion en %s (+C = 0x%04x)", m->sys & W580_MS_SRAC ? "serie" : "PARALELO", m->sys); }
        }
        w580_irq_update(s);
        break;
    default: break;      /* +8 es de solo lectura */
    }
}

/* ---------------------------------------------------------------- meter y sacar la tarjeta */
static void w580_ms_pin(W580State *s)
{
    /* P3.4: 0 = hay tarjeta [observado en el original, 44A1.c:5538]. Es la misma entrada forzada de gpio-in=3.4=V */
    s->gpio_in_mask[3] |= 0x10;
    s->gpio_in_val[3] = (s->gpio_in_val[3] & ~0x10) | (s->ms.puesta ? 0 : 0x10);
    if (s->cpu) { w580_irq_update(s); }
}
static bool w580_ms_abre(W580State *s, Error **errp)
{
    W580Ms *m = &s->ms;
    if (m->fd >= 0) { return true; }
    if (!m->ruta || !*m->ruta) { error_setg(errp, "ms-puesta: no hay imagen (ms-imagen=RUTA)"); return false; }
    int fd = open(m->ruta, (m->solo_lectura ? O_RDONLY : O_RDWR) | O_CLOEXEC);
    if (fd < 0) { error_setg_errno(errp, errno, "ms-imagen: no se puede abrir %s%s", m->ruta, m->solo_lectura ? "" : " para leer y escribir (ms-solo-lectura=on la abre solo para leer)"); return false; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 1024 * 1024 || (uint64_t)st.st_size > UINT64_C(32) << 30) {
        close(fd); error_setg(errp, "ms-imagen: %s debe medir entre 1 MiB y 32 GiB", m->ruta); return false;
    }
    m->fd = fd;
    uint64_t sec = st.st_size / 512;
    m->bloque = 0x80;       /* 64 KiB por bloque [inferido: docs/memory_stick.md §6.1]; más si no caben en 16 bits */
    while (sec / m->bloque > 0xffff) { m->bloque <<= 1; }
    m->sectores = sec / m->bloque * m->bloque;
    qemu_log_mask(LOG_UNIMP, "W580_MS imagen %s: %" PRIu64 " sectores de 512 (%" PRIu64 " MiB), %u sectores por bloque, %" PRIu64 " bloques%s%s\n", m->ruta, m->sectores,
                  m->sectores >> 11, m->bloque, m->sectores / m->bloque, sec != m->sectores ? " (el final que no llena un bloque queda fuera)" : "",
                  m->solo_lectura ? "; SOLO LECTURA: lo escrito va a una capa temporal" : "");
    return true;
}
static bool w580_ms_pon_puesta(W580State *s, bool puesta, Error **errp)
{
    W580Ms *m = &s->ms;
    if (puesta && !w580_ms_abre(s, errp)) { return false; }
    if (puesta != m->puesta) {
        m->puesta = puesta;
        if (puesta) { m->n_metidas++; } else { m->n_sacadas++; }
        qemu_log_mask(LOG_UNIMP, "W580_MS tarjeta %s (P3.4 = %d) t=%.6f s\n", puesta ? "METIDA" : "SACADA", puesta ? 0 : 1, w580_ms_ahora() / 1e9);
    }
    if (!puesta && m->fd >= 0) { close(m->fd); m->fd = -1; }
    w580_ms_alimenta(s);
    w580_ms_pin(s);
    return true;
}
/* gpio-in=3.4=V con el modelo puesto: es meter (0) o sacar (1) la tarjeta. Sin imagen solo se mueve el pin (nadie contesta). */
static bool w580_ms_gpio(W580State *s, bool puesta, Error **errp)
{
    W580Ms *m = &s->ms;
    if (!m->iniciado) { m->puesta = puesta; m->puesta_dicha = true; m->por_pin = true; return true; }      /* al lanzar: el orden de las propiedades no importa */
    if (!m->modelo) { return true; }
    if (puesta && (!m->ruta || !*m->ruta)) { m->puesta = true; return true; }       /* el pin dice «hay tarjeta» y nadie contesta */
    return w580_ms_pon_puesta(s, puesta, errp);
}
static void w580_ms_reset(W580State *s)
{
    W580Ms *m = &s->ms;
    if (!m->modelo) { return; }
    w580_ms_host_reset(s);
    m->sys = 0; m->reseteado = false; m->t_tension = 0;
    m->viva = false; w580_ms_tarjeta_reset(s);
    w580_ms_alimenta(s);        /* con pmu-reset caliente el PMU conserva 0x43: la tarjeta sigue con tensión (se reinicia igual) */
    w580_ms_pin(s);
}
static void w580_ms_init(W580State *s)
{
    W580Ms *m = &s->ms;
    m->t_fin = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_ms_fin, s);
    m->t_int = timer_new_ns(QEMU_CLOCK_VIRTUAL, w580_ms_int_sube, s);
    m->iniciado = true;
    if (!m->modelo) { return; }
    if (m->ruta && *m->ruta && (!m->puesta_dicha || m->puesta)) {
        m->puesta = false;
        w580_ms_pon_puesta(s, true, &error_fatal);
    } else {
        if (m->puesta && !m->por_pin) { error_report("w580: ms-puesta=on sin ms-imagen"); exit(1); }
        w580_ms_pin(s);
    }
}
static void w580_ms_instance_init(W580State *s) { s->ms.fd = -1; }

/* ---------------------------------------------------------------- propiedades */
static bool w580_ms_get_modelo(Object *obj, Error **errp) { return W580_MACHINE(obj)->ms.modelo; }
static void w580_ms_set_modelo(Object *obj, bool value, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    if (s->ms.iniciado && value != s->ms.modelo) { error_setg(errp, "ms: solo al lanzar"); return; }
    s->ms.modelo = value;
}
static char *w580_ms_get_imagen(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->ms.ruta); }
static void w580_ms_set_imagen(Object *obj, const char *value, Error **errp)
{
    W580State *s = W580_MACHINE(obj); W580Ms *m = &s->ms;
    if (m->iniciado && !m->modelo) { error_setg(errp, "ms-imagen: la maquina se lanzo sin Memory Stick (ms=on o ms-imagen=RUTA al lanzar)"); return; }
    if (m->iniciado && m->puesta) { error_setg(errp, "ms-imagen: saca antes la tarjeta (ms-puesta=off)"); return; }
    g_free(m->ruta); m->ruta = g_strdup(value);
    if (m->capa) { g_hash_table_destroy(m->capa); m->capa = NULL; m->capa_n = 0; }
    if (!m->iniciado && value && *value) { m->modelo = true; }
}
static bool w580_ms_get_puesta(Object *obj, Error **errp) { return W580_MACHINE(obj)->ms.puesta; }
static void w580_ms_set_puesta(Object *obj, bool value, Error **errp)
{
    W580State *s = W580_MACHINE(obj); W580Ms *m = &s->ms;
    if (!m->iniciado) { m->puesta = value; m->puesta_dicha = true; if (value) { m->modelo = true; } return; }
    if (!m->modelo) { error_setg(errp, "ms-puesta: la maquina se lanzo sin Memory Stick (ms=on o ms-imagen=RUTA al lanzar)"); return; }
    w580_ms_pon_puesta(s, value, errp);
}
static bool w580_ms_get_sl(Object *obj, Error **errp) { return W580_MACHINE(obj)->ms.solo_lectura; }
static void w580_ms_set_sl(Object *obj, bool value, Error **errp)
{
    W580Ms *m = &W580_MACHINE(obj)->ms;
    if (m->iniciado && m->puesta) { error_setg(errp, "ms-solo-lectura: saca antes la tarjeta (ms-puesta=off)"); return; }
    if (m->capa && !value) { g_hash_table_destroy(m->capa); m->capa = NULL; m->capa_n = 0; }
    m->solo_lectura = value;
}
static bool w580_ms_get_prot(Object *obj, Error **errp) { return W580_MACHINE(obj)->ms.protegida; }
static void w580_ms_set_prot(Object *obj, bool value, Error **errp)
{
    W580Ms *m = &W580_MACHINE(obj)->ms;
    if (m->iniciado && m->puesta) { error_setg(errp, "ms-protegida: saca antes la tarjeta (ms-puesta=off)"); return; }
    m->protegida = value;
}
static char *w580_ms_get_hip(Object *obj, Error **errp) { return g_strdup(W580_MACHINE(obj)->ms.hip ? W580_MACHINE(obj)->ms.hip : ""); }
static void w580_ms_set_hip(Object *obj, const char *value, Error **errp)
{
    W580Ms *m = &W580_MACHINE(obj)->ms;
    bool a = false, b = false;
    g_auto(GStrv) v = g_strsplit(value, ":", -1);
    for (unsigned i = 0; v[i]; i++) {
        if (!strcmp(v[i], "sin-sondeo")) { a = true; }
        else if (!strcmp(v[i], "sin-msint-serie")) { b = true; }
        else if (*v[i] && strcmp(v[i], "nada")) { error_setg(errp, "ms-hipotesis: «%s» no es sin-sondeo, sin-msint-serie ni nada (separadas por ':')", v[i]); return; }
    }
    m->sin_sondeo = a; m->sin_msint_serie = b;
    g_free(m->hip); m->hip = g_strdup(value);
}
static bool w580_ms_get_sondeo(Object *obj, Error **errp) { return W580_MACHINE(obj)->ms.sondeo_vale; }
static void w580_ms_set_sondeo(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->ms.sondeo_vale = value; }
static char *w580_ms_get_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj); W580Ms *m = &s->ms;
    GString *g = g_string_new(NULL);
    if (!m->modelo) { g_string_append(g, "Memory Stick: sin modelo (FC01000x en el cerrojo; ms=on o ms-imagen=RUTA al lanzar)\n"); return g_string_free(g, false); }
    g_string_append_printf(g, "tarjeta: %s imagen=%s sectores=%" PRIu64 " (%" PRIu64 " MiB) bloque=%u solo-lectura=%s capa=%u sectores protegida=%s P3.4=%d\n",
                           m->puesta ? "PUESTA" : "fuera", m->ruta ? m->ruta : "(ninguna)", m->sectores, m->sectores >> 11, m->bloque, m->solo_lectura ? "on" : "off",
                           m->capa_n, m->protegida ? "on" : "off", s->gpio_in_val[3] >> 4 & 1);
    g_string_append_printf(g, "tension=%s (PMU 43=%02x 44=%02x) tarjeta en %s INT=0x%02x orden=0x%02x fase=%u quedan=%u\n", m->viva ? "si" : "no",
                           w580_ms_pmu_regs(s)[0x43], w580_ms_pmu_regs(s)[0x44], m->paralelo ? "paralelo" : "serie", m->intr, m->orden, m->fase, m->quedan);
    g_string_append_printf(g, "anfitrion: +C=0x%04x (%s%s) +8=0x%04x activo=%d IRQ76=%d resets=%" PRIu64 " hipotesis=%s\n", m->sys, m->sys & W580_MS_SRAC ? "serie" : "paralelo",
                           m->sys & W580_MS_INTEN ? ", INTEN" : "", m->sta, m->activo, w580_ms_irq(s), m->n_reset, m->hip && *m->hip ? m->hip : "las del modelo");
    g_string_append(g, "TPC:");
    for (unsigned i = 0; i < 16; i++) { if (m->n_tpc[i]) { g_string_append_printf(g, " %X=%" PRIu64, i, m->n_tpc[i]); } }
    g_string_append_printf(g, "\ncuentas: RDY=%" PRIu64 " TOE=%" PRIu64 " MSINT=%" PRIu64 " ordenes=%" PRIu64 " sectores_leidos=%" PRIu64 " sectores_escritos=%" PRIu64
                           " metidas=%" PRIu64 " sacadas=%" PRIu64 " TPC_con_INTEN=%" PRIu64 " TPC_por_sondeo=%" PRIu64 "\n", m->n_rdy, m->n_toe, m->n_msint, m->n_ordenes,
                           m->n_leidos, m->n_escritos, m->n_metidas, m->n_sacadas, m->n_irq_tpc, m->n_sondeo);
    g_string_append_printf(g, "escritos (lba+cuenta):%s\n", m->escritos && m->escritos->len ? m->escritos->str : " (nada)");
    return g_string_free(g, false);
}
static void w580_ms_class_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "ms", w580_ms_get_modelo, w580_ms_set_modelo);
    object_class_property_set_description(oc, "ms", "Pista E9: on = se modela el anfitrion del Memory Stick (FC010000, IRQ 76) aunque no haya tarjeta. Lo pone ms-imagen. Solo al lanzar");
    object_class_property_add_str(oc, "ms-imagen", w580_ms_get_imagen, w580_ms_set_imagen);
    object_class_property_set_description(oc, "ms-imagen", "Pista E9: fichero imagen de la tarjeta Memory Stick PRO (MBR + FAT; tools/e9_ms_imagen.py). Al lanzar: la tarjeta arranca puesta. En caliente, solo con la tarjeta fuera");
    object_class_property_add_bool(oc, "ms-puesta", w580_ms_get_puesta, w580_ms_set_puesta);
    object_class_property_set_description(oc, "ms-puesta", "Pista E9: meter (on) o sacar (off) la tarjeta, tambien en caliente: mueve la entrada P3.4 (0 = puesta) y la conecta al anfitrion");
    object_class_property_add_bool(oc, "ms-solo-lectura", w580_ms_get_sl, w580_ms_set_sl);
    object_class_property_set_description(oc, "ms-solo-lectura", "Pista E9: on = el FICHERO imagen no se toca; lo que el firmware escribe va a una capa temporal en memoria");
    object_class_property_add_bool(oc, "ms-protegida", w580_ms_get_prot, w580_ms_set_prot);
    object_class_property_set_description(oc, "ms-protegida", "Pista E9: on = la TARJETA se declara protegida contra escritura (STATUS0 bit 0, tipo de dispositivo 1) y rechaza la orden 0x21");
    object_class_property_add_bool(oc, "ms-sondeo-vale", w580_ms_get_sondeo, w580_ms_set_sondeo);
    object_class_property_set_description(oc, "ms-sondeo-vale", "Pista E9, estricto: on = no avisar de los TPC con INTEN a 0 (sondear RDY/MSINT es una hipotesis del modelo, no lo que hace el original)");
    object_class_property_add_str(oc, "ms-hipotesis", w580_ms_get_hip, w580_ms_set_hip);
    object_class_property_set_description(oc, "ms-hipotesis", "Pista E9: da la vuelta a hipotesis del modelo que el original no distingue, para probar un controlador contra las dos: sin-sondeo (con INTEN a 0 no se ven RDY/CRC/TOE/MSINT en +8) y/o sin-msint-serie (en serie la INT de la tarjeta no da MSINT: ni el CED del encendido), separadas por ':'; nada las quita");
    object_class_property_add_str(oc, "ms-estado", w580_ms_get_estado, NULL);
    object_class_property_set_description(oc, "ms-estado", "Pista E9: estado de la tarjeta y del anfitrion del Memory Stick, cuentas de TPC y tramos escritos");
}
