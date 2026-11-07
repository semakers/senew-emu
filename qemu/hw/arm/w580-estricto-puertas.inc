/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Puertas de reloj de SYSCON por bloque (pista E4). { primera, última dirección, palabra de puerta, bit, palabra de
 * reset, bit, nombre, quién lo abre en el original }. Palabra de puerta: 0 = +16, 1 = +18, 2 = +1A, 3 = +1C, 4 = +3A
 * (bit a 1 = reloj PARADO); palabra de reset: 0 = +10, 1 = +12, 2 = +14 (bit a 0 = bloque EN RESET); -1 = sin esa mitad.
 * Solo filas con evidencia doble: la función del original que abre el reloj (docs/hal.md §2.1) y la estadística de la
 * calibración (tools/e4_tablas.py puertas): el original NUNCA toca el bloque con ese bit cerrado / en reset.
 * La fila del generador de tonos es solo estática (el original no lo toca en nada de lo ejecutado). */
    { 0xf7000000, 0xf7001fff,  1,  9,  1,  9, "XGAM: GAMCon, PDI, PDICON, MMIACC", "FUN_4422102c: puerta 9, bloque 7" },
    { 0xfb010000, 0xfb010007,  2,  0,  2,  0, "teclado", "FUN_441acc64: puerta 21, bloque 17" },
    { 0xf9040000, 0xf9040007,  0,  5,  0,  4, "I2C 0", "0x449CDCD0 y FUN_441aa330: puerta 13, bloque 11" },
    { 0xf9070000, 0xf9070007,  0,  6,  0,  5, "I2C 1", "FUN_441aa330: puerta 14, bloque 12" },
    { 0xfb060000, 0xfb06003f,  2,  2, -1,  0, "UART 0", "FUN_442200b0: puerta 33" },
    { 0xfb070000, 0xfb07003f,  2,  3, -1,  0, "UART 1", "FUN_442200b0: puerta 34" },
    { 0xfb080000, 0xfb08003f,  2,  4, -1,  0, "UART 3", "FUN_442200b0: puerta 35" },
    { 0xfb020100, 0xfb0203ff,  2,  1,  2,  1, "controlador de interrupciones", "FUN_4423a33c: puerta 15, bloque 13" },
    { 0xf9000000, 0xf90000ff,  0,  0,  0,  0, "GPIO", "FUN_441a9060: puerta 10, bloque 8" },
    { 0xf9010000, 0xf901000f,  0,  1, -1,  0, "temporizadores", "FUN_44236a14: puerta 30" },
    { 0xf901001c, 0xf9010023,  0,  1, -1,  0, "temporizadores", "FUN_44236a14: puerta 30" },
    { 0xf90b0000, 0xf90b00ff,  0,  2, -1,  0, "segundo temporizador", "FUN_44232808: puerta 31" },
    { 0xf9050000, 0xf905005f,  0,  7, -1,  0, "RTC", "FUN_441acb04: puerta 27" },
    { 0xf5000874, 0xf5000875,  0, 11, -1,  0, "pulso de conversión del ADC", "FUN_44233734: puerta 40" },
    /* USB FC030000: SIN fila (pista UEMU, 2026-10-14). La «puerta 26, bloque 20» era falsa: con VBUS el original toca el USB con
     * F9090018 bit 1 a 1 y F9090012 bit 1 a 0 [observado en el emulador]; docs/usb.md §11. */
    { 0xfc040000, 0xfc04ffff,  2, 12,  2, 12, "IrDA", "FUN_4415f7d8: puertas 17 y 18, bloque 15" },
    { 0xf9020000, 0xf90200ff,  0,  3,  0,  2, "generador de tonos [solo leído en su código]", "FUN_441ad020: puerta 32, bloque 22" },
