/* SPDX-License-Identifier: GPL-2.0-or-later */
    /* estado de los periféricos modelados (w580-perif.inc); se rellena según lo pide la traza */
    uint32_t irq_mask[3], irq_latch[3], irq_level[3], irq_vector[3], irq_config[3][32];
    uint64_t irq_cuenta[3][32];
    QEMUTimer *timer1, *timer2;
    int64_t timer_epoch[2];
    uint64_t timer_cuentas[2], timer_atrasos[2], timer_perdones[2], timer_tics[2], timer_fundidos[2], timer_aplazados[2]; bool timer_reancla[2]; unsigned timer_aplaza[2];   /* pista E6: época = base + cuentas de 32 768 Hz */
    uint32_t timer_control, timer_compare[2];
    bool timer_expired[2];
    uint32_t cs_log;
    MemoryRegion rtc_port;
    uint8_t rtc_regs[0x60];
    int64_t rtc_seconds, rtc_epoch;
    int64_t rtc_fraction_ns, rtc_alarm_when[2];
    QEMUTimer *rtc_alarm_timer;
    bool rtc_date_pending;
    struct W580I2C {
        struct W580State *s;
        unsigned idx;
        MemoryRegion port;
        QEMUTimer *timer;
        bool pending, reading;
        uint16_t control, divider;
        uint8_t data, status, phase, slave, reg;
        uint8_t fa_modo, fa_pegado, fa_vistas; bool fa_parada, fa_primera, fa_exento; int64_t fa_t, fa_hasta; uint16_t fa_valor; QEMUTimer *fa_timer;   /* pista E8: fallo inyectado en la transacción en curso (w580-fallos.inc) */
        uint8_t devs[16], regs[128][256];
    } i2c[2];
    uint32_t i2c_log;
    char *i2c_lista;
    uint32_t *dsp_memory;
    uint64_t dsp_reads, dsp_writes;
    uint16_t uart_pending[3];
    uint8_t uart_linea[3][40];
    unsigned uart_n[3];
    uint32_t uart_log;
    uint64_t uart_tx_total[3];
    uint8_t pmu_adc[16], pmu_vbat, pmu_id;
    uint32_t chip_id;
    uint32_t kp_control, kp_log;
    uint8_t kp_cols[6], kp_masa;
    QEMUTimer *kp_timer;
    uint32_t panel_id;
    uint32_t panel_r28;                /* pista E5 (petición 3 de S3): lo que contesta el registro 0x28 del panel; 0xFFFFFFFF = lo último escrito */
    bool reloj_cpu; uint32_t reloj_cpu_mas, cpu_mhz; int reloj_shift; int64_t reloj_ns_en[3], reloj_t_cambio; uint64_t reloj_cambios;   /* pista E6: w580-reloj.inc */
    bool panel_real, panel_real_puesto, panel_variante_puesta;   /* pista E6: w580-panel.inc */
    char *lcd_traza_ruta;              /* pista E3: propiedades pantalla-registro y panel-dormido-negro (w580-lcd.inc) */
    bool lcd_dormido_visible;
    /* acelerómetro (w580-acel.inc, pista E2) */
    struct W580AcelPunto { int64_t t_ns; float x, y; } *acel_tabla;
    unsigned acel_n, acel_i;
    int64_t acel_t0, acel_t_enc, acel_min_asentado, acel_ns_encendido, acel_ult_t;
    char *acel_senal, *acel_registro;
    FILE *acel_f;
    uint64_t acel_conv[2], acel_conv_apagado[2], acel_encendidos;
    uint8_t acel_ult[2], acel_apagado;
    bool acel_puerta;
    uint32_t acel_log;
    /* pista E2: lo pedido por la pista S y la persistencia (w580-extra.inc) */
    uint8_t gpio_in_mask[5], gpio_in_val[5];
    bool reset_real, apagado_real, wdt_30_cero, pmu_41_cero;
    bool wdt_armado; QEMUTimer *wdt_reset_timer;      /* pista E6: reset-real espera a F9010014 <- 1 */
    CharFrontend uart0_chr;
    bool uart0_hay;
    uint8_t uart0_rx[256];
    unsigned uart0_rx_head, uart0_rx_n;
    FILE *tono_f;
    char *tono_ruta;
    uint64_t tono_muestras;
    int64_t tono_t0;
    uint32_t tono_log;
    /* pista W: cola de sucesos del generador de tonos (propiedad tono-sucesos, w580-extra.inc) */
    struct { int64_t t; uint16_t ctl, per, anc; } tsuc[512];
    uint32_t tsuc_ini, tsuc_n;
    uint64_t tsuc_seq, tsuc_perdidos;
    bool tsuc_on;
    char *nor_persistente;
    Notifier salida;
    /* pista E7: batería y cargador (w580-bateria.inc), nor-id y teclas-arranque. Sin sus propiedades, nada cambia */
    struct W580Bat {
        bool puesto;                    /* se ha fijado pmu-vbat-mv (o una descarga): las cuentas salen de mv; si no, las de siempre */
        double mv; int64_t t_mv;        /* tensión de la batería en el instante t_mv (tiempo virtual) */
        double descarga, sube;          /* mV/s: descarga-mv-por-s y carga-mv-por-s */
        bool cargador, usb, ausente, apaga_cc, negado;
        uint32_t cargador_mv;
        uint8_t b7_modo; uint32_t b7_s; bool b7_caducado; int64_t b7_vence; QEMUTimer *b7_timer; uint64_t b7_rearmes, b7_caducas;
        int64_t t61; uint32_t pc61; QEMUTimer *t61_timer; double s61_max; uint64_t n61_avisos;
        int64_t h61_ini[16], h61_fin[16]; unsigned h61_n; double a61_arranque;      /* pista E8 (AUD4): tramos cerrados de 0x61 con código != 0, para el ACUMULADO de la regla [carga] */
        unsigned ma; int64_t t_ma; double s_ma, mah; uint64_t n_cambios, n_sucesos;
        uint32_t log;
    } bat;
    uint32_t nor_id; char *nor_cfi_pon;
    char *kp_arranque; uint32_t kp_arranque_ms; QEMUTimer *kp_arranque_timer;
    bool pmu_on_baja;               /* (sis28, 28-oct) PMU 0x23 bit 0 ACTIVO A 0 como lo lee el firmware (w580-i2c.inc, w580_i2c_dev_read) */
    /* pista E8: inyección de fallos (w580-fallos.inc). Sin sus propiedades, nada cambia */
    struct W580Fallos {
        struct W580FalloI2C {
            uint8_t tipo, fase, sent; int16_t bus, esc, reg;
            int64_t n, cada, salta, ms, hasta_ns;
            uint64_t vistos, hechos;
        } r[8];
        unsigned nr; char *i2c_txt;
        int32_t pmu77, pmu6a;                      /* -1 = sin fijar */
        int64_t apagado_falla; uint64_t apagado_negados;
        uint8_t kp_at_cols[6], kp_at_masa; bool kp_at_on; char *kp_at_txt;
        uint8_t kp_foto_modo, kp_foto; bool kp_foto_armada; uint64_t kp_fotos;
        uint64_t n_nack, n_colgado, n_fijo; uint32_t log;
        bool pmu_frio, pmu_arrancado; uint64_t pmu_calientes;      /* pmu-reset: el PMU conserva o no sus registros en un reset de la CPU */
        uint8_t apaga_tecla; uint64_t apaga_tecla_n;
    } fa;
    /* pista E9: Memory Stick (w580-ms.inc): anfitrión FC010000 + tarjeta PRO sobre un fichero imagen. Sin sus propiedades, nada cambia */
    struct W580Ms {
        bool modelo, iniciado, puesta, puesta_dicha, solo_lectura, protegida, sondeo_vale, sin_sondeo, sin_msint_serie, por_pin; char *hip;
        char *ruta; int fd; uint64_t sectores; uint32_t bloque;      /* capacidad en sectores de 512; sectores por bloque de los atributos */
        GHashTable *capa; uint32_t capa_n;                           /* ms-solo-lectura: los sectores escritos viven aquí, no en el fichero */
        /* anfitrión */
        uint32_t sys, sta; uint8_t tpc, dir, res; uint16_t n; bool activo, reseteado;
        uint8_t fifo[1032]; uint32_t fifo_n, fifo_i;
        QEMUTimer *t_fin, *t_int;
        /* tarjeta */
        bool viva, paralelo, rw_puesto; uint8_t reg[0x20], rw[4], intr, intr_sig, fase, orden;
        uint32_t lba, quedan, aplazos; int64_t t_tension;
        /* cuentas */
        uint64_t n_tpc[16], n_toe, n_msint, n_rdy, n_leidos, n_escritos, n_ordenes, n_metidas, n_sacadas, n_reset, n_sondeo, n_irq_tpc;
        uint32_t log; GString *escritos; uint32_t e_ini, e_n;
    } ms;
    /* pista UEMU: USB de dispositivo (w580-usb.inc): UDC tipo OMAP1 en FC030000 + anfitrión por chardev. Sin udc/usb-host, nada cambia */
    struct W580Usb {
        bool modelo, hay_host; char *host; CharFrontend chr; QEMUTimer *t_reset;
        uint16_t ep_num, syscon1, irq_en, irq_src, dma_irq_en, devstat, ep0, r108, ep_rx[16], ep_tx[16];
        uint16_t pend_tx, pend_rx, ant[3];
        uint8_t setup[8], setup_i; bool setup_otro, stall0, puerta_dicha;
        struct W580UsbEp { uint8_t buf[W580_USB_FIFO_TAM]; uint32_t n, i; bool armado, ack, stall, halt; } ep[16][2];
        uint8_t direccion, reset_fase; int dir_pend; uint32_t reset_seq;
        uint8_t rx[2048]; uint32_t rx_n;
        uint64_t n_msg, n_resets, n_setup, n_armados, bytes_in, bytes_out, n_ds, n_pullup, n_cfg; uint32_t log;
    } usb;
    /* pista BT: la UART 3 del DB2020 con un STLC2500 falso al otro lado (w580-bt.inc; bt/prueba_emu/LEEME.md). Sin bt-uart,
     * nada cambia: la UART 3 sigue sin nadie al otro lado. Las reglas [uart3] del estricto valen con y sin el modelo. */
    struct W580Bt {
        bool modelo, hay_ctl; char *uart_id, *ctl_id; CharFrontend chr, ctl; QEMUTimer *t;
        int64_t t_ant, resto_ns;
        uint8_t tx[512]; uint32_t tx_i, tx_n;           /* FIFO de transmisión (bt-fifo bytes) */
        uint8_t rx[512]; uint32_t rx_i, rx_n;           /* FIFO de recepción */
        uint8_t et[4096]; uint32_t et_i, et_n;          /* lo que el chip ya ha soltado y aún no ha cruzado el cable */
        uint32_t fifo, baud; uint16_t fmt, flujo;       /* la línea del teléfono tal como la dejan +0A/+0C/+16/+20 */
        uint32_t baud_dicho; uint16_t fmt_dicho, flujo_dicho;
        int reset_nivel; int64_t reset_bajo_ns; bool vivo;
        int64_t t_rx_ult; bool to_dado;                 /* el último byte que entró en la FIFO de RX; el «tiempo de espera» ya avisado */
        char cbuf[160]; uint32_t cbuf_n;
        uint64_t n_tx, n_rx, n_perdidos, n_retenidos, n_tx_lleno, n_tx_en_reset, n_resets, n_lineas, n_vacia;
        uint32_t log;
    } bt;
