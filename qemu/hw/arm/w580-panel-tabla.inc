/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Panel variant table -- PUBLIC TREE VERSION (stub).
 *
 * The development tree generates this file from the display-driver configuration file of a real handset (the four
 * factory initialisation scripts of the LCD controller, as register writes). That data comes from the manufacturer's
 * firmware, so it is NOT published here. What remains is the part that is hardware identification only:
 * the four variant names and the value each one answers on panel register R0028.
 *
 * Consequence: every script below is EMPTY. With panel-real=off (the default, and what the browser emulator uses)
 * nothing changes: the picture is drawn exactly as before. With estricto=on / panel-real=on the panel still runs its
 * PDI microcode interpreter and its R61505 register/GRAM model, but the "does the firmware follow the factory
 * script" cross-checks ([panel] warnings about out-of-script writes) are skipped (w580_pr_guion_ev returns at once).
 *
 * Registro 0xFFFF = "el que estuviera elegido" (datos sin índice delante). */
typedef struct { uint16_t reg, val, espera; } W580PrPaso;
typedef struct { const char *nombre; const W580PrPaso *p; unsigned n; } W580PrGuion;
enum { W580_PR_G_INIT, W580_PR_G_INIT2, W580_PR_G_STANDBY_FROM_NORMAL, W580_PR_G_NORMAL_FROM_STANDBY, W580_PR_G_SHUTDOWN, W580_PR_G_SET_BPP_MODE, W580_PR_G_SET_BPP_MODE_HIGH, W580_PR_G_PRE_UPDATE, W580_PR_NGUIONES };
/* sm_cristal: bit SM de R0001 con el que esta variante de cristal se inicia (en el árbol de desarrollo se sacaba del guion init) */
typedef struct { const char *corto, *nombre; int id; bool sm_cristal; W580PrGuion g[W580_PR_NGUIONES]; } W580PrVariante;
#define W580_PR_GUIONES_VACIOS \
    { { "init", NULL, 0 }, { "init2", NULL, 0 }, { "standby_from_normal", NULL, 0 }, { "normal_from_standby", NULL, 0 }, \
      { "shutdown", NULL, 0 }, { "set_bpp_mode", NULL, 0 }, { "set_bpp_mode_high", NULL, 0 }, { "pre_update", NULL, 0 } }
static const W580PrVariante w580_pr_variantes[4] = {
    { "hitachi", "LCD variant R0028 = 0x00", 0, true, W580_PR_GUIONES_VACIOS },
    { "lpg", "LCD variant R0028 = 0x0A", 10, false, W580_PR_GUIONES_VACIOS },
    { "sharp", "LCD variant R0028 = 0x0E", 14, false, W580_PR_GUIONES_VACIOS },
    { "r61504", "LCD variant R61504 (fallback)", -1, true, W580_PR_GUIONES_VACIOS },
};
#define W580_PR_REG_ID   0x0028     /* el registro que distingue las variantes y su máscara (campo `reg` de los descriptores) */
#define W580_PR_REG_MASC 0x000F
#define W580_PR_VEL_GC 0x1
#define W580_PR_VEL_WRITE 0x1
#define W580_PR_VEL_READ 0x15
static const uint32_t w580_pr_colores16[6] = { 0x130F0E0D, 0x17161514, 0x06050403, 0x0C0B0A07, 0x00000000, 0x00000000 };     /* pdidisplaycolour1..6: PDICON+E4..+F8 para 16 bpp */
