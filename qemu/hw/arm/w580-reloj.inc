/* Pista E6: RELOJ DE CPU SEGÚN SYSCON, sobre -icount. SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Con -icount el tiempo virtual es «instrucciones × 2^shift ns», a paso fijo. Con `reloj-cpu=on` (apagada por omisión; solo
 * tiene efecto si QEMU se lanza con -icount) el paso SIGUE al reloj que programa el firmware en F909001C (docs/hal.md §2.2:
 * bit 0 = usar el PLL, bit 4 = dividir por 2; lo que midió la pista E4/E5 en el original):
 *     13 MHz (reset, y siempre que el bit 0 esté a 0)   shift 6 = 64 ns por instrucción (15,6 MHz)
 *    104 MHz (bit 0 y bit 4)                            shift 3 =  8 ns (125 MHz)
 *    208 MHz (bit 0, sin bit 4)                         shift 2 =  4 ns (250 MHz)
 * Es decir, la escala entre los tres estados es la buena (8 y 16 veces) y el valor absoluto va un 20 % deprisa: el shift
 * es una potencia de dos. Además cuenta INSTRUCCIONES, no ciclos (un ARM9 gasta ~1,5–2 ciclos por instrucción [inferido]):
 * una espera por bucle dura en el emulador MENOS que en el aparato. Para una regla de «espera mínima» eso es el lado
 * prudente (si aquí llega, allí sobra). `reloj-cpu-mas=N` suma N al shift (N = 1: la mitad de velocidad, ~CPI 1,7).
 * El cambio de paso se hace como lo hace QEMU en su modo adaptativo (icount_adjust): se ajusta el sesgo para que el
 * tiempo virtual no salte. [sin saber]: si el núcleo va de verdad a esas frecuencias; el coste de un acceso de E/S.
 */
#include "qemu/seqlock.h"
#include "exec/icount.h"
#include "system/cpu-timers-internal.h"

static unsigned w580_reloj_mhz_de(unsigned v1c) { return !(v1c & 1) ? 13 : (v1c & 0x10) ? 104 : 208; }
static void w580_reloj_pon(W580State *s, unsigned mhz)
{
    int64_t ahora = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned i = s->cpu_mhz == 13 ? 0 : s->cpu_mhz == 104 ? 1 : 2;
    s->reloj_ns_en[i] += ahora - s->reloj_t_cambio; s->reloj_t_cambio = ahora;
    if (mhz != s->cpu_mhz) { s->reloj_cambios++; }
    s->cpu_mhz = mhz;
    if (!s->reloj_cpu || !icount_enabled()) { return; }
    int nuevo = (mhz >= 208 ? 2 : mhz >= 104 ? 3 : 6) + (int)s->reloj_cpu_mas;
    (void)icount_get_raw();             /* pone al día las instrucciones ya ejecutadas (estamos en un acceso de E/S o con la CPU parada) */
    seqlock_write_lock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
    int viejo = timers_state.icount_time_shift;
    if (nuevo != viejo) {
        int64_t ic = timers_state.qemu_icount;
        qatomic_set_i64(&timers_state.qemu_icount_bias, timers_state.qemu_icount_bias + (ic << viejo) - (ic << nuevo));
        qatomic_set(&timers_state.icount_time_shift, nuevo);
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock, &timers_state.vm_clock_lock);
    if (nuevo != viejo) {
        s->reloj_shift = nuevo;
        if (current_cpu) { cpu_exit(current_cpu); }       /* el presupuesto de instrucciones hasta el próximo plazo se calculó con el paso viejo */
        qemu_log_mask(LOG_UNIMP, "W580_RELOJ CPU a %u MHz: %d ns por instruccion (shift %d -> %d) t=%" PRId64 " ns pc=0x%08x\n", mhz, 1 << nuevo, viejo, nuevo,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->cpu->env.regs[15]);
    }
}
static void w580_reloj_syscon(W580State *s) { w580_reloj_pon(s, w580_reloj_mhz_de(w580_latch_get(s, 0xf909001c, 2))); }
static void w580_reloj_reset(W580State *s) { s->reloj_t_cambio = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL); w580_reloj_pon(s, 13); }
static bool w580_reloj_activo(W580State *s) { return s->reloj_cpu && icount_enabled(); }
static char *w580_get_reloj_cpu_estado(Object *obj, Error **errp)
{
    W580State *s = W580_MACHINE(obj);
    int64_t ahora = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned i = s->cpu_mhz == 13 ? 0 : s->cpu_mhz == 104 ? 1 : 2;
    int64_t en[3] = { s->reloj_ns_en[0], s->reloj_ns_en[1], s->reloj_ns_en[2] }; en[i] += ahora - s->reloj_t_cambio;
    char paso[64] = "";
    if (w580_reloj_activo(s)) { snprintf(paso, sizeof(paso), ", %d ns por instruccion (shift %d)", 1 << timers_state.icount_time_shift, timers_state.icount_time_shift); }
    return g_strdup_printf("reloj-cpu=%s icount=%s | F909001C=0x%04x -> CPU a %u MHz%s | cambios=%" PRIu64 " | tiempo virtual a 13/104/208 MHz: %.3f / %.3f / %.3f s\n",
                           s->reloj_cpu ? "on" : "off", icount_enabled() ? "si" : "no (el tiempo virtual es el del anfitrion: la velocidad de la CPU NO se modela)",
                           (unsigned)w580_latch_get(s, 0xf909001c, 2), s->cpu_mhz,
                           paso,
                           s->reloj_cambios, en[0] / 1e9, en[1] / 1e9, en[2] / 1e9);
}
static bool w580_get_reloj_cpu(Object *obj, Error **errp) { return W580_MACHINE(obj)->reloj_cpu; }
static void w580_set_reloj_cpu(Object *obj, bool value, Error **errp) { W580_MACHINE(obj)->reloj_cpu = value; }
static void w580_reloj_class_init(ObjectClass *oc)
{
    object_class_property_add_bool(oc, "reloj-cpu", w580_get_reloj_cpu, w580_set_reloj_cpu);
    object_class_property_set_description(oc, "reloj-cpu", "Pista E6: con -icount, el paso del reloj virtual sigue a F909001C (13/104/208 MHz). Solo al lanzar");
    object_class_property_add(oc, "reloj-cpu-mas", "uint32", w580_get_u32, w580_set_u32, NULL, (void *)offsetof(W580State, reloj_cpu_mas));
    object_class_property_set_description(oc, "reloj-cpu-mas", "Pista E6: se suma al shift de reloj-cpu (1 = CPU a la mitad de velocidad)");
    object_class_property_add_str(oc, "reloj-cpu-estado", w580_get_reloj_cpu_estado, NULL);
}
