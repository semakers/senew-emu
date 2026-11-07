/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OJO (pista E9): dos valores más A MANO en w580_es_pmu_val (0x43 <- 0x07 y 0x44 <- 0x0E: el encendido de la tarjeta Memory Stick, work/emu/trazas/e9_m1_m.aprendido.tsv).
 * OJO (pista E7): tres entradas añadidas A MANO (0x7a R y los valores 0x90 y 0x94 de 0x60), de las primeras trazas del original CON
 * cargador (work/emu/trazas/e7_oc2.aprendido.tsv y e7_oc5.aprendido.tsv). Al regenerar con tools/e5_tablas.py hay que incluir ese fichero o volver a añadirlas.
 * GENERADO por tools/e4_tablas.py. Transacciones I2C que el original hace: bus << 24 | esclavo << 16 | registro << 8 | escribe. */
static const uint32_t w580_es_i2c_lista[] = {
    0x004a2000,   /* bus 0, 0x4a, reg 0x20 R: 15 veces; valores e0 */
    0x004a2100,   /* bus 0, 0x4a, reg 0x21 R: 30 veces; valores 00 01 */
    0x004a2200,   /* bus 0, 0x4a, reg 0x22 R: 30 veces; valores 00 */
    0x004a2300,   /* bus 0, 0x4a, reg 0x23 R: 30 veces; valores 00 */
    0x004a2400,   /* bus 0, 0x4a, reg 0x24 R: 15 veces; valores 00 */
    0x004a2401,   /* bus 0, 0x4a, reg 0x24 W: 30 veces; valores 00 ef */
    0x004a2500,   /* bus 0, 0x4a, reg 0x25 R: 15 veces; valores 00 */
    0x004a2501,   /* bus 0, 0x4a, reg 0x25 W: 30 veces; valores 00 7f */
    0x004a2600,   /* bus 0, 0x4a, reg 0x26 R: 15 veces; valores 08 */
    0x004a2601,   /* bus 0, 0x4a, reg 0x26 W: 15 veces; valores 08 */
    0x004a2700,   /* bus 0, 0x4a, reg 0x27 R: 15 veces; valores 72 */
    0x004a2701,   /* bus 0, 0x4a, reg 0x27 W: 15 veces; valores 72 */
    0x004a3600,   /* bus 0, 0x4a, reg 0x36 R: 15 veces; valores 00 */
    0x004a3601,   /* bus 0, 0x4a, reg 0x36 W: 15 veces; valores 00 */
    0x004a3700,   /* bus 0, 0x4a, reg 0x37 R: 15 veces; valores 00 */
    0x004a3800,   /* bus 0, 0x4a, reg 0x38 R: 15 veces; valores 1b */
    0x004a3801,   /* bus 0, 0x4a, reg 0x38 W: 15 veces; valores 1b */
    0x004a4000,   /* bus 0, 0x4a, reg 0x40 R: 15 veces; valores 28 */
    0x004a4001,   /* bus 0, 0x4a, reg 0x40 W: 15 veces; valores 28 */
    0x004a4100,   /* bus 0, 0x4a, reg 0x41 R: 15 veces; valores 26 */
    0x004a4101,   /* bus 0, 0x4a, reg 0x41 W: 15 veces; valores 26 */
    0x004a4200,   /* bus 0, 0x4a, reg 0x42 R: 15 veces; valores 42 */
    0x004a4201,   /* bus 0, 0x4a, reg 0x42 W: 15 veces; valores 42 */
    0x004a4300,   /* bus 0, 0x4a, reg 0x43 R: 30 veces; valores 00 04 */
    0x004a4301,   /* bus 0, 0x4a, reg 0x43 W: 45 veces; valores 04 80 */
    0x004a4400,   /* bus 0, 0x4a, reg 0x44 R: 15 veces; valores 0c */
    0x004a4401,   /* bus 0, 0x4a, reg 0x44 W: 15 veces; valores 0c */
    0x004a4500,   /* bus 0, 0x4a, reg 0x45 R: 15 veces; valores 50 */
    0x004a4501,   /* bus 0, 0x4a, reg 0x45 W: 15 veces; valores 50 */
    0x004a5000,   /* bus 0, 0x4a, reg 0x50 R: 15 veces; valores 80 */
    0x004a5001,   /* bus 0, 0x4a, reg 0x50 W: 217 veces; valores 80 83 85 b5 b8 */
    0x004a6000,   /* bus 0, 0x4a, reg 0x60 R: 15 veces; valores 88 */
    0x004a6001,   /* bus 0, 0x4a, reg 0x60 W: 62 veces; valores 88 */
    0x004a6100,   /* bus 0, 0x4a, reg 0x61 R: 15 veces; valores 13 */
    0x004a6101,   /* bus 0, 0x4a, reg 0x61 W: 60 veces; valores 10 13 */
    0x004a6200,   /* bus 0, 0x4a, reg 0x62 R: 15 veces; valores 00 */
    0x004a6201,   /* bus 0, 0x4a, reg 0x62 W: 45 veces; valores 00 01 */
    0x004a6300,   /* bus 0, 0x4a, reg 0x63 R: 75 veces; valores 00 */
    0x004a6301,   /* bus 0, 0x4a, reg 0x63 W: 75 veces; valores 00 */
    0x004a7000,   /* bus 0, 0x4a, reg 0x70 R: 15 veces; valores 6a */
    0x004a7001,   /* bus 0, 0x4a, reg 0x70 W: 34705 veces; valores 6a a1 a4 a8 ab ac ad */
    0x004a7100,   /* bus 0, 0x4a, reg 0x71 R: 34705 veces; valores 00 26 6c 7d 80 */
    0x004a7200,   /* bus 0, 0x4a, reg 0x72 R: 30 veces; valores f3 */
    0x004a7201,   /* bus 0, 0x4a, reg 0x72 W: 45 veces; valores f0 f3 */
    0x004a7300,   /* bus 0, 0x4a, reg 0x73 R: 30 veces; valores 00 */
    0x004a7301,   /* bus 0, 0x4a, reg 0x73 W: 30 veces; valores 00 30 */
    0x004a7500,   /* bus 0, 0x4a, reg 0x75 R: 15 veces; valores 6c */
    0x004a7600,   /* bus 0, 0x4a, reg 0x76 R: 15 veces; valores 6c */
    0x004a7700,   /* bus 0, 0x4a, reg 0x77 R: 147 veces; valores 6c */
    0x004a7a00,   /* bus 0, 0x4a, reg 0x7a R: A MANO, pista E7: el original lee la corriente del cargador cada segundo mientras hay cargador (80 veces en work/emu/trazas/e7_oc2.aprendido.tsv); valores 00 */
    0x004a8400,   /* bus 0, 0x4a, reg 0x84 R: 15 veces; valores 00 */
    0x004a8600,   /* bus 0, 0x4a, reg 0x86 R: 15 veces; valores 00 */
    0x004a8700,   /* bus 0, 0x4a, reg 0x87 R: 15 veces; valores 00 */
    0x004a8800,   /* bus 0, 0x4a, reg 0x88 R: 15 veces; valores 00 */
    0x004a8801,   /* bus 0, 0x4a, reg 0x88 W: 15 veces; valores 16 */
    0x004a8900,   /* bus 0, 0x4a, reg 0x89 R: 15 veces; valores 00 */
    0x004a8901,   /* bus 0, 0x4a, reg 0x89 W: 15 veces; valores 16 */
    0x004a9000,   /* bus 0, 0x4a, reg 0x90 R: 15 veces; valores 00 */
    0x004a9001,   /* bus 0, 0x4a, reg 0x90 W: 177 veces; valores f0 f4 f8 fc */
    0x004a9100,   /* bus 0, 0x4a, reg 0x91 R: 15 veces; valores 80 */
    0x004a9101,   /* bus 0, 0x4a, reg 0x91 W: 30 veces; valores 80 */
    0x004a9200,   /* bus 0, 0x4a, reg 0x92 R: 162 veces; valores 00 */
    0x004a9300,   /* bus 0, 0x4a, reg 0x93 R: 162 veces; valores 00 */
    0x004a9400,   /* bus 0, 0x4a, reg 0x94 R: 162 veces; valores 00 */
    0x004aa000,   /* bus 0, 0x4a, reg 0xa0 R: 15 veces; valores 00 */
    0x004aa001,   /* bus 0, 0x4a, reg 0xa0 W: 45 veces; valores 00 20 */
    0x004aa100,   /* bus 0, 0x4a, reg 0xa1 R: 15 veces; valores 0c */
    0x004aa101,   /* bus 0, 0x4a, reg 0xa1 W: 30 veces; valores 0c */
    0x004ab000,   /* bus 0, 0x4a, reg 0xb0 R: 15 veces; valores f4 */
    0x004ab001,   /* bus 0, 0x4a, reg 0xb0 W: 30 veces; valores 04 f4 */
    0x004ab101,   /* bus 0, 0x4a, reg 0xb1 W: 15 veces; valores 00 */
    0x004ab201,   /* bus 0, 0x4a, reg 0xb2 W: 15 veces; valores 00 */
    0x004ab301,   /* bus 0, 0x4a, reg 0xb3 W: 15 veces; valores 00 */
    0x004ab401,   /* bus 0, 0x4a, reg 0xb4 W: 15 veces; valores 00 */
    0x004ab501,   /* bus 0, 0x4a, reg 0xb5 W: 15 veces; valores 00 */
    0x004ab600,   /* bus 0, 0x4a, reg 0xb6 R: 15 veces; valores b2 */
    0x004ab601,   /* bus 0, 0x4a, reg 0xb6 W: 15 veces; valores b2 */
    0x004ab700,   /* bus 0, 0x4a, reg 0xb7 R: 15 veces; valores 00 */
    0x004ab800,   /* bus 0, 0x4a, reg 0xb8 R: 15 veces; valores 00 */
    0x004ab900,   /* bus 0, 0x4a, reg 0xb9 R: 15 veces; valores 00 */
    0x004ac000,   /* bus 0, 0x4a, reg 0xc0 R: 15 veces; valores 00 */
    0x004ac001,   /* bus 0, 0x4a, reg 0xc0 W: 240 veces; valores 00 01 03 13 17 43 47 57 */
    0x004ac100,   /* bus 0, 0x4a, reg 0xc1 R: 15 veces; valores 00 */
    0x004ac101,   /* bus 0, 0x4a, reg 0xc1 W: 20 veces; valores 00 40 */
    0x004ac200,   /* bus 0, 0x4a, reg 0xc2 R: 15 veces; valores 00 */
    0x004ac201,   /* bus 0, 0x4a, reg 0xc2 W: 8 veces; valores 00 04 44 */
    0x004ac300,   /* bus 0, 0x4a, reg 0xc3 R: 15 veces; valores 00 */
    0x004ac301,   /* bus 0, 0x4a, reg 0xc3 W: 8 veces; valores 60 66 */
    0x004ac400,   /* bus 0, 0x4a, reg 0xc4 R: 15 veces; valores 00 */
    0x004ac401,   /* bus 0, 0x4a, reg 0xc4 W: 8 veces; valores 60 66 */
    0x004ac500,   /* bus 0, 0x4a, reg 0xc5 R: 15 veces; valores 00 */
    0x004ac501,   /* bus 0, 0x4a, reg 0xc5 W: 27 veces; valores 00 04 18 20 */
    0x004ac600,   /* bus 0, 0x4a, reg 0xc6 R: 15 veces; valores 00 */
    0x004ac601,   /* bus 0, 0x4a, reg 0xc6 W: 16 veces; valores 00 80 */
    0x004ac700,   /* bus 0, 0x4a, reg 0xc7 R: 15 veces; valores 00 */
    0x004ac701,   /* bus 0, 0x4a, reg 0xc7 W: 54 veces; valores 40 */
    0x004ac800,   /* bus 0, 0x4a, reg 0xc8 R: 15 veces; valores 00 */
    0x004ac801,   /* bus 0, 0x4a, reg 0xc8 W: 22 veces; valores 08 0c */
    0x004ac900,   /* bus 0, 0x4a, reg 0xc9 R: 15 veces; valores 00 */
    0x004ac901,   /* bus 0, 0x4a, reg 0xc9 W: 20 veces; valores 00 04 10 14 */
    0x004aca00,   /* bus 0, 0x4a, reg 0xca R: 15 veces; valores 00 */
    0x004aca01,   /* bus 0, 0x4a, reg 0xca W: 19 veces; valores 10 */
    0x004acb00,   /* bus 0, 0x4a, reg 0xcb R: 15 veces; valores 00 */
    0x004acb01,   /* bus 0, 0x4a, reg 0xcb W: 47 veces; valores 00 40 50 */
    0x004acc00,   /* bus 0, 0x4a, reg 0xcc R: 15 veces; valores 00 */
    0x004acc01,   /* bus 0, 0x4a, reg 0xcc W: 77 veces; valores 40 50 */
    0x004acd00,   /* bus 0, 0x4a, reg 0xcd R: 15 veces; valores 00 */
    0x004acd01,   /* bus 0, 0x4a, reg 0xcd W: 47 veces; valores 00 40 50 57 */
    0x004ace00,   /* bus 0, 0x4a, reg 0xce R: 15 veces; valores 00 */
    0x004ace01,   /* bus 0, 0x4a, reg 0xce W: 12 veces; valores 20 30 32 */
    0x004acf00,   /* bus 0, 0x4a, reg 0xcf R: 15 veces; valores 00 */
    0x004acf01,   /* bus 0, 0x4a, reg 0xcf W: 12 veces; valores 20 22 30 */
    0x004ad000,   /* bus 0, 0x4a, reg 0xd0 R: 15 veces; valores 00 */
    0x004ad001,   /* bus 0, 0x4a, reg 0xd0 W: 24 veces; valores 00 20 30 34 35 */
    0x004af400,   /* bus 0, 0x4a, reg 0xf4 R: 15 veces; valores 00 */
    0x004af500,   /* bus 0, 0x4a, reg 0xf5 R: 15 veces; valores 00 */
    0x01400001,   /* bus 1, 0x40, reg 0x00 W: 16 veces; valores 00 08 0c */
    0x01400101,   /* bus 1, 0x40, reg 0x01 W: 20 veces; valores 00 01 05 */
    0x01400201,   /* bus 1, 0x40, reg 0x02 W: 16 veces; valores 00 40 6a */
    0x01400301,   /* bus 1, 0x40, reg 0x03 W: 4 veces; valores 00 */
    0x01400401,   /* bus 1, 0x40, reg 0x04 W: 12 veces; valores 00 2a */
    0x01400501,   /* bus 1, 0x40, reg 0x05 W: 4 veces; valores 00 */
    0x01400801,   /* bus 1, 0x40, reg 0x08 W: 4 veces; valores 00 */
    0x01400901,   /* bus 1, 0x40, reg 0x09 W: 34 veces; valores 00 01 02 03 04 05 */
    0x01400a01,   /* bus 1, 0x40, reg 0x0a W: 34 veces; valores 00 01 02 03 04 05 */
    0x01400b01,   /* bus 1, 0x40, reg 0x0b W: 12 veces; valores 00 ff */
    0x01400c01,   /* bus 1, 0x40, reg 0x0c W: 16 veces; valores 00 7f ff */
    0x01400d01,   /* bus 1, 0x40, reg 0x0d W: 16 veces; valores 00 ff */
    0x01400e01,   /* bus 1, 0x40, reg 0x0e W: 4 veces; valores 00 */
    0x01400f01,   /* bus 1, 0x40, reg 0x0f W: 4 veces; valores 00 */
    0x01401001,   /* bus 1, 0x40, reg 0x10 W: 4 veces; valores 00 */
    0x01401101,   /* bus 1, 0x40, reg 0x11 W: 4 veces; valores 00 */
    0x01401201,   /* bus 1, 0x40, reg 0x12 W: 4 veces; valores 00 */
    0x01401301,   /* bus 1, 0x40, reg 0x13 W: 12 veces; valores 00 ff */
    0x01401401,   /* bus 1, 0x40, reg 0x14 W: 16 veces; valores 00 7f ff */
    0x01401501,   /* bus 1, 0x40, reg 0x15 W: 16 veces; valores 00 ff */
    0x01401601,   /* bus 1, 0x40, reg 0x16 W: 40 veces; valores 01 11 13 */
    0x01401701,   /* bus 1, 0x40, reg 0x17 W: 22 veces; valores 00 */
    0x01401801,   /* bus 1, 0x40, reg 0x18 W: 4 veces; valores 00 */
    0x01401901,   /* bus 1, 0x40, reg 0x19 W: 4 veces; valores 00 */
    0x01401a01,   /* bus 1, 0x40, reg 0x1a W: 4 veces; valores 00 */
    0x01401b01,   /* bus 1, 0x40, reg 0x1b W: 4 veces; valores 00 */
    0x01401c01,   /* bus 1, 0x40, reg 0x1c W: 4 veces; valores 00 */
    0x01401d01,   /* bus 1, 0x40, reg 0x1d W: 4 veces; valores 00 */
    0x01401f01,   /* bus 1, 0x40, reg 0x1f W: 4 veces; valores 0c */
    0x01402001,   /* bus 1, 0x40, reg 0x20 W: 4 veces; valores 00 */
    0x01402101,   /* bus 1, 0x40, reg 0x21 W: 8 veces; valores 00 9a */
    0x01402201,   /* bus 1, 0x40, reg 0x22 W: 8 veces; valores 04 84 */
    0x01402301,   /* bus 1, 0x40, reg 0x23 W: 8 veces; valores 00 09 */
    0x01402401,   /* bus 1, 0x40, reg 0x24 W: 8 veces; valores 00 7f */
    0x01402501,   /* bus 1, 0x40, reg 0x25 W: 8 veces; valores 00 7c */
    0x01402601,   /* bus 1, 0x40, reg 0x26 W: 8 veces; valores 00 94 */
    0x01402700,   /* bus 1, 0x40, reg 0x27 R: 4 veces; valores 00 */
    0x01402800,   /* bus 1, 0x40, reg 0x28 R: 4 veces; valores 00 */
    0x01402901,   /* bus 1, 0x40, reg 0x29 W: 4 veces; valores 01 */
    0x01402c01,   /* bus 1, 0x40, reg 0x2c W: 4 veces; valores 00 */
    0x01402d01,   /* bus 1, 0x40, reg 0x2d W: 4 veces; valores 00 */
    0x01402e01,   /* bus 1, 0x40, reg 0x2e W: 4 veces; valores 00 */
    0x01402f01,   /* bus 1, 0x40, reg 0x2f W: 77 veces; valores 00 2f 34 44 54 64 74 84 */
    0x01403001,   /* bus 1, 0x40, reg 0x30 W: 4 veces; valores 00 */
    0x01403101,   /* bus 1, 0x40, reg 0x31 W: 4 veces; valores 00 */
    0x01403201,   /* bus 1, 0x40, reg 0x32 W: 4 veces; valores 00 */
    0x01403301,   /* bus 1, 0x40, reg 0x33 W: 4 veces; valores 00 */
    0x01403401,   /* bus 1, 0x40, reg 0x34 W: 4 veces; valores 00 */
    0x01403501,   /* bus 1, 0x40, reg 0x35 W: 4 veces; valores 00 */
    0x01403700,   /* bus 1, 0x40, reg 0x37 R: 4 veces; valores 00 */
    0x01403800,   /* bus 1, 0x40, reg 0x38 R: 4 veces; valores 00 */
    0 };
/* valores que el original escribe en los registros DELICADOS del PMU (reguladores 0x38, 0x40..0x44 y cargador 0x60..0x63):
 * { registro, n, valores }. Un valor fuera de la lista es un aviso [pmu]. */
static const struct { uint8_t reg, n, val[8]; } w580_es_pmu_val[] = {
    { 0x38, 3, { 0x00, 0x1b, 0x38 } },
    { 0x40, 1, { 0x28 } },
    { 0x41, 2, { 0x00, 0x26 } },
    { 0x42, 1, { 0x42 } },
    { 0x43, 4, { 0x04, 0x80, 0x84, 0x07 } },       /* 0x07 A MANO, pista E9: el original enciende la tarjeta Memory Stick con SetMode(6,3) = 43 <- (43 & FC) | 3 (work/emu/trazas/e9_m1_m.aprendido.tsv:
                                                    * «I2C 0 4a 43 W … 80,04,07»); al apagarla vuelve a 04 */
    { 0x44, 2, { 0x0c, 0x0e } },                   /* 0x0E A MANO, pista E9: SetVoltage(6,4) = 44 <- (44 & FC) | 2, justo antes (misma traza: «I2C 0 4a 44 W … 0c,0e») */
    { 0x60, 5, { 0x00, 0x80, 0x88, 0x90, 0x94 } }, /* 0x90 y 0x94 A MANO, pista E7: con cargador el original escribe 60 <- 90 cada segundo (e7_oc2: 83 veces) y,
                                                    * si arranca con él puesto, un 60 <- 94 a los 20 s (e7_oc5) */
    { 0x61, 3, { 0x00, 0x10, 0x13 } },
    { 0x62, 2, { 0x00, 0x01 } },
    { 0x63, 1, { 0x00 } },
    { 0, 0, { 0 } } };
/* canales del ADC del PMU que el original convierte (0x70 <- 0xA0 | canal) */
static const uint16_t w580_es_adc_canales = 0x3912;   /* 1 4 8 11 12 13 */
/* esclavos que el original llama y NO contestan en el modelo: bus << 8 | direccion */
static const uint16_t w580_es_i2c_nack[] = { 0x0000, 0x0100, 0x0110, 0x0140,  0xffff };
