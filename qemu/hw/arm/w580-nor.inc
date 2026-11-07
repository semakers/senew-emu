/* NOR del W580 (pista E): modelo propio, no el pflash_cfi01 de QEMU.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * [inferido] chip tipo Intel/Numonyx StrataFlash M18 («Sibley» en las cadenas del main): 64 MiB,
 * bus de 16 bits, bloques de borrado uniformes de 256 KiB (docs/fs.md: eraseSize 0x40000),
 * 8 particiones de 8 MiB con estado de lectura propio. Cada partición es un rom_device aparte:
 * mientras una está en modo «estado/ID/CFI» las demás siguen siendo memoria ejecutable.
 * No modela tiempos, suspensión, ni las restricciones de «región de programación» del M18.
 */
#define W580_NOR_BASE   0x44000000u
#define W580_NOR_TAM    0x04000000u
#define W580_NOR_PART   0x00800000u
#define W580_NOR_NPART  (W580_NOR_TAM / W580_NOR_PART)
#define W580_NOR_BLOQUE 0x00040000u

typedef struct W580NorPart {
    struct W580Nor *nor;
    MemoryRegion mem, alias;
    uint8_t *p;
    unsigned idx;
    uint8_t modo;          /* 0xFF matriz, 0x70 estado, 0x90 identificador, 0x98 CFI */
} W580NorPart;

typedef struct W580Nor {
    W580State *s;
    W580NorPart part[W580_NOR_NPART];
    uint8_t status;
    uint8_t pend;          /* orden de varios ciclos en curso: 0, 0x20, 0x60, 0x40, 0xE8, 0xBC */
    uint8_t fase;          /* 0xE8: 1 = espera cuenta, 2 = datos, 3 = espera confirmación */
    uint32_t pend_dir;     /* dirección absoluta (desplazamiento en la NOR) de la orden en curso */
    uint32_t buf_rest, buf_ini, buf_fin;
    uint64_t n_abortos;
    uint16_t rcr, ecr;
    uint8_t bloqueo[W580_NOR_TAM / W580_NOR_BLOQUE];   /* bit0 bloqueado, bit1 «lock-down» */
    uint16_t cfi[0x200];
    uint16_t id_fab, id_disp;
    uint8_t otp[0x200];    /* registros de protección, palabras 0x80.. (leen en modo 0x90) */
    uint64_t n_prog, n_borr, n_ordenes;
    unsigned n_log, log_max;
    uint8_t sucio[W580_NOR_TAM / W580_NOR_BLOQUE];
} W580Nor;

static void w580_nor_modo(W580NorPart *pt, uint8_t modo)
{
    pt->modo = modo;
    memory_region_rom_device_set_romd(&pt->mem, modo == 0xff);
}
static uint8_t *w580_nor_ptr(struct W580Nor *n, uint32_t abs);
#define W580_ES_PARTE_NOR
#include "w580-estricto.inc"      /* segunda parte: la NOR estricta (pista E4) */
#undef W580_ES_PARTE_NOR
static void w580_nor_log(W580Nor *n, const char *que, uint32_t abs, unsigned size, uint64_t value)
{
    if (n->n_log < n->log_max) {
        n->n_log++;
        qemu_log_mask(LOG_UNIMP, "W580_NOR %s dir=0x%08x size=%u valor=0x%" PRIx64 " pc=0x%08x lr=0x%08x\n",
                      que, W580_NOR_BASE + abs, size, value, n->s->cpu->env.regs[15], n->s->cpu->env.regs[14]);
    }
}
static uint8_t *w580_nor_ptr(W580Nor *n, uint32_t abs)
{
    return n->part[abs / W580_NOR_PART].p + (abs % W580_NOR_PART);
}
static void w580_nor_ensuciar(W580Nor *n, uint32_t abs, unsigned len)
{
    W580NorPart *pt = &n->part[abs / W580_NOR_PART];
    memory_region_set_dirty(&pt->mem, abs % W580_NOR_PART, len);
    n->sucio[abs / W580_NOR_BLOQUE] = 1;
}
static uint64_t w580_nor_read(void *opaque, hwaddr off, unsigned size)
{
    W580NorPart *pt = opaque;
    W580Nor *n = pt->nor;
    uint32_t abs = pt->idx * W580_NOR_PART + off;
    uint32_t pal = (off >> 1) & 0xffff;        /* índice de palabra dentro del bloque de 128 KiB de consulta */
    uint32_t v = 0;
    switch (pt->modo) {
    case 0xff: {
        uint64_t r = 0;
        memcpy(&r, pt->p + off, size);
        return r;
    }
    case 0x70:
        v = w580_es_nor_estado(n);
        break;
    case 0x90:
    case 0x98:
        pal = (abs % W580_NOR_BLOQUE) >> 1;
        if (pal == 0) { v = n->id_fab; }
        else if (pal == 1) { v = n->id_disp; }
        else if (pal == 2) { v = n->bloqueo[abs / W580_NOR_BLOQUE]; }
        else if (pal == 5) { v = n->rcr; }
        else if (pal == 6) { v = n->ecr; }
        else if (pal >= 0x80 && pal < 0x180) { v = lduw_le_p(n->otp + 2 * (pal - 0x80)); }
        else if (pal < 0x200) { v = n->cfi[pal]; }
        break;
    }
    if (size == 1) { v = (v >> ((off & 1) * 8)) & 0xff; }
    else if (size == 4) { v |= v << 16; }
    w580_nor_log(n, pt->modo == 0x70 ? "lee-estado" : pt->modo == 0x90 ? "lee-id" : "lee-cfi", abs, size, v);
    return v;
}
static void w580_nor_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    W580NorPart *pt = opaque;
    W580Nor *n = pt->nor;
    uint32_t abs = pt->idx * W580_NOR_PART + off;
    uint8_t cmd = value & 0xff;
    bool es = w580_es_activo() && current_cpu;
    n->n_ordenes++;
    if (es && w580_es_nor_orden(n, pt, abs, value)) { return; }
    if (n->pend == 0x40) {                       /* dato del programa de palabra */
        uint8_t *p = w580_nor_ptr(n, abs);
        if (es && w580_es_nor_palabra(n, abs, size, value)) { n->pend = 0; n->status = 0x80 | 0x10 | 0x02; return; }
        for (unsigned i = 0; i < size; i++) { p[i] &= (value >> (8 * i)) & 0xff; }
        w580_nor_ensuciar(n, abs, size);
        n->n_prog++; n->pend = 0; n->status = 0x80;
        w580_nor_log(n, "programa", abs, size, value);
        return;
    }
    if (n->pend == 0xe8) {
        if (n->fase == 1) {                      /* cuenta N-1 de palabras */
            n->buf_rest = (value & 0xffff) + 1; n->fase = 2; n->buf_ini = UINT32_MAX; n->buf_fin = 0;
            w580_nor_log(n, "bufer-cuenta", abs, size, value);
            return;
        }
        if (n->fase == 2) {
            /* Aborto del controlador del main (RAM 0x20002738): si hay una interrupción pendiente a mitad de la
             * carga, escribe 0xFF en la OTRA punta del bloque de borrado, fuera del tramo que se está cargando.
             * [inferido de ese código + la hoja del M18: una dirección fuera del tramo aborta con error de
             * secuencia, SR.4 y SR.5] Después manda 0x70, espera SR.7, bloquea, limpia el estado (0x50) y repite. */
            if (n->buf_ini == UINT32_MAX) { n->buf_ini = abs; n->buf_fin = abs + 2 * n->buf_rest; if (es) { w580_es_nor_buf_ini(n, abs); } }
            if (abs < n->buf_ini || abs >= n->buf_fin) {
                if (es) { w580_es_nor_buf_aborta(n); }
                n->pend = 0; n->status = 0x80 | 0x30; n->n_abortos++;
                w580_nor_log(n, "bufer-ABORTADO", abs, size, value);
                return;
            }
            uint8_t *p = w580_nor_ptr(n, abs);
            if (es) { w580_es_nor_buf_dato(n, abs, size, value); }
            for (unsigned i = 0; i < size; i++) { p[i] &= (value >> (8 * i)) & 0xff; }
            w580_nor_ensuciar(n, abs, size);
            if (n->buf_rest == 0 || --n->buf_rest == 0) { n->fase = 3; }
            return;
        }
        if (cmd != 0xd0) { n->status |= 0x30; }
        if (es && n->buf_ini != UINT32_MAX && w580_es_nor_buf_fin(n)) { n->status |= 0x10 | 0x02; }
        n->n_prog++; n->pend = 0; n->status |= 0x80;
        w580_nor_log(n, "bufer-confirma", abs, size, value);
        return;
    }
    if (n->pend == 0x20) {
        n->pend = 0;
        if (cmd == 0xd0) {
            uint32_t b = abs & ~(W580_NOR_BLOQUE - 1);
            if (es && w580_es_nor_borra(n, b)) { n->status = 0x80 | 0x20 | 0x02; return; }
            memset(w580_nor_ptr(n, b), 0xff, W580_NOR_BLOQUE);
            w580_nor_ensuciar(n, b, W580_NOR_BLOQUE);
            n->n_borr++; n->status = 0x80;
            w580_nor_log(n, "BORRA-bloque", b, size, value);
        } else {
            n->status = 0x80 | 0x30;
            w580_nor_log(n, "borrado-sin-confirmar", abs, size, value);
        }
        return;
    }
    if (n->pend == 0xbc) {                        /* comprobación de bloque en blanco */
        n->pend = 0; n->status = 0x80;
        if (cmd == 0xd0) {
            uint32_t b = abs & ~(W580_NOR_BLOQUE - 1);
            uint8_t *p = w580_nor_ptr(n, b);
            for (uint32_t i = 0; i < W580_NOR_BLOQUE; i++) { if (p[i] != 0xff) { n->status |= 0x20; break; } }
        }
        w580_nor_log(n, "en-blanco", abs, size, n->status);
        return;
    }
    if (n->pend == 0x60) {
        n->pend = 0; n->status = 0x80;
        unsigned b = abs / W580_NOR_BLOQUE;
        switch (cmd) {
        case 0x01: n->bloqueo[b] |= 1; w580_esn.n_bloq++; break;
        case 0xd0: if (!(n->bloqueo[b] & 2)) { n->bloqueo[b] &= ~1; } w580_esn.n_desbloq++; break;
        case 0x2f: n->bloqueo[b] |= 3; break;
        case 0x03: n->rcr = (abs >> 1) & 0xffff; w580_nor_modo(pt, 0xff); break;   /* fija RCR: vuelve a matriz */
        case 0x04: n->ecr = (abs >> 1) & 0xffff; w580_nor_modo(pt, 0xff); break;
        default: n->status |= 0x30; break;
        }
        w580_nor_log(n, cmd == 0x03 ? "fija-RCR" : cmd == 0x04 ? "fija-ECR" : "bloqueo", abs, size, value);
        return;
    }
    switch (cmd) {
    case 0xff: w580_nor_modo(pt, 0xff); break;
    case 0x90: w580_nor_modo(pt, 0x90); break;
    case 0x98: w580_nor_modo(pt, 0x98); break;
    case 0x70: w580_nor_modo(pt, 0x70); break;
    case 0x50: n->status = 0x80; break;
    case 0x20: n->pend = 0x20; n->pend_dir = abs; w580_nor_modo(pt, 0x70); break;
    case 0x60: n->pend = 0x60; n->pend_dir = abs; w580_nor_modo(pt, 0x70); break;
    case 0x10: case 0x40: case 0x41:
        n->pend = 0x40; n->pend_dir = abs; w580_nor_modo(pt, 0x70); break;
    case 0xe8: case 0xe9:
        n->pend = 0xe8; n->fase = 1; n->pend_dir = abs; n->status = 0x80; w580_nor_modo(pt, 0x70); break;
    case 0xbc: n->pend = 0xbc; n->pend_dir = abs; w580_nor_modo(pt, 0x70); break;
    case 0xb0: case 0xd0: break;                  /* suspender / reanudar: nada que suspender */
    default:
        w580_nor_log(n, "ORDEN-DESCONOCIDA", abs, size, value);
        return;
    }
    w580_nor_log(n, "orden", abs, size, value);
}
static const MemoryRegionOps w580_nor_ops = {
    .read = w580_nor_read, .write = w580_nor_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};
/* Pista E4: con estricto o aprender, la ventana 0x04000000 no es un alias sino E/S propia, para SABER por cuál de
 * las dos ventanas llega cada orden (el contenido es el mismo chip). Sin ellos, el alias de siempre. */
static uint64_t w580_nor04_read(void *opaque, hwaddr off, unsigned size)
{
    return w580_nor_read(opaque, off, size);
}
static void w580_nor04_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    w580_esn.via04 = true;
    w580_nor_write(opaque, off, value, size);
    w580_esn.via04 = false;
}
static const MemoryRegionOps w580_nor04_ops = {
    .read = w580_nor04_read, .write = w580_nor04_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};
static void w580_nor_cfi(W580Nor *n)
{
    static const uint8_t qry[] = {       /* desde la palabra 0x10 */
        'Q', 'R', 'Y', 0x00, 0x02, 0x0a, 0x01, 0, 0, 0, 0,     /* 10..1A: juego de órdenes 0x0200, P = 0x10A */
        0x17, 0x20, 0x85, 0x95,                                 /* 1B..1E: tensiones */
        0x08, 0x09, 0x0a, 0x00, 0x01, 0x01, 0x02, 0x00,         /* 1F..26: tiempos */
        0x1a, 0x01, 0x00, 0x0a, 0x00,                           /* 27 tamaño 2^26; 28/29 interfaz; 2A/2B búfer 2^10 */
        0x01, 0xff, 0x00, 0x00, 0x04,                           /* 2C: 1 región; 256 bloques de 0x400*256 = 256 KiB */
    };
    static const uint8_t pri[] = {       /* desde la palabra 0x10A */
        'P', 'R', 'I', '1', '4',
        0xe6, 0x07, 0x00, 0x00,          /* funciones opcionales */
        0x01,                            /* funciones tras suspender */
        0x33, 0x00,                      /* máscara del estado de bloque */
        0x18, 0x90,                      /* Vcc/Vpp óptimas */
        0x02,                            /* campos de protección */
        0x80, 0x00, 0x03, 0x03,          /* campo 1: palabra 0x80, 8 bytes de fábrica + 8 de usuario */
        0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x04,   /* campo 2 */
        0x05,                            /* lectura en modo página */
        0x03, 0x02, 0x03, 0x07,          /* configuraciones síncronas */
        0x01,                            /* regiones de partición */
        0x16, 0x00,                      /* tamaño de esta región de datos */
        0x08, 0x00,                      /* 8 particiones idénticas */
        0x11, 0x00, 0x00,                /* operaciones simultáneas */
        0x01,                            /* tipos de bloque en la partición */
        0x1f, 0x00, 0x00, 0x04,          /* 32 bloques de 256 KiB */
        0x64, 0x00,                      /* ciclos de borrado (miles) */
        0x12, 0x03,                      /* bits por celda, modo página/síncrono */
        0x0a, 0x00, 0x10, 0x00, 0x10, 0x00,   /* región de programación del M18 */
    };
    memset(n->cfi, 0, sizeof(n->cfi));
    for (unsigned i = 0; i < sizeof(qry); i++) { n->cfi[0x10 + i] = qry[i]; }
    for (unsigned i = 0; i < sizeof(pri); i++) { n->cfi[0x10a + i] = pri[i]; }
}
static void w580_nor_reset(W580Nor *n)
{
    n->status = 0x80; n->pend = 0; n->fase = 0;
    n->rcr = 0xbfcf; n->ecr = 0x0004;
    for (unsigned i = 0; i < W580_NOR_NPART; i++) { w580_nor_modo(&n->part[i], 0xff); }
    w580_es_nor_reset(n);
}
static W580Nor *w580_nor_init(W580State *s, MemoryRegion *sysmem, const char *imagen)
{
    W580Nor *n = g_new0(W580Nor, 1);
    n->s = s; n->id_fab = s->nor_id >> 16; n->id_disp = s->nor_id & 0xffff; n->log_max = 4000;      /* pista E7: nor-id (por omisión 0089 887E) */
    memset(n->otp, 0xff, sizeof(n->otp));
    memset(n->bloqueo, 0, sizeof(n->bloqueo));
    w580_nor_cfi(n);
    if (s->nor_cfi_pon && *s->nor_cfi_pon) {       /* pista E7: nor-cfi-pon=PAL=VALOR[:PAL=VALOR…] altera palabras del CFI (QRY en 0x10..0x12) */
        g_auto(GStrv) v = g_strsplit(s->nor_cfi_pon, ":", -1);
        for (unsigned i = 0; v[i]; i++) {
            unsigned pal, val;
            if (sscanf(v[i], "%i=%i", &pal, &val) == 2 && pal < 0x200) { n->cfi[pal] = val; }
            else if (*v[i]) { error_report("w580: nor-cfi-pon: «%s» no es PALABRA=VALOR (palabra < 0x200)", v[i]); exit(1); }
        }
    }
    FILE *f = imagen ? fopen(imagen, "rb") : NULL;
    if (!f) { error_report("w580: hace falta la imagen de flash de 64 MiB con -bios"); exit(1); }
    for (unsigned i = 0; i < W580_NOR_NPART; i++) {
        W580NorPart *pt = &n->part[i];
        g_autofree char *nombre = g_strdup_printf("w580.nor%u", i);
        pt->nor = n; pt->idx = i;
        memory_region_init_rom_device(&pt->mem, NULL, &w580_nor_ops, pt, nombre, W580_NOR_PART, &error_fatal);
        pt->p = memory_region_get_ram_ptr(&pt->mem);
        if (fread(pt->p, 1, W580_NOR_PART, f) != W580_NOR_PART) {
            error_report("w580: la imagen de flash no mide 64 MiB"); exit(1);
        }
        memory_region_add_subregion(sysmem, W580_NOR_BASE + i * W580_NOR_PART, &pt->mem);
        /* Segundo alias FÍSICO de la misma NOR en 0x04000000 (MEMMAP_FLASH_NOR_START, «cfi/mmap … rw_base»): la tabla
         * de páginas del main mapea 0x04000000..0x07FFFFFF sobre sí mismo (hito_b.txt), luego el chip responde ahí. */
        g_autofree char *nombre2 = g_strdup_printf("w580.nor%u-alias04", i);
        if (w580_es_activo()) { memory_region_init_io(&pt->alias, NULL, &w580_nor04_ops, pt, nombre2, W580_NOR_PART); }
        else { memory_region_init_alias(&pt->alias, NULL, nombre2, &pt->mem, 0, W580_NOR_PART); }
        memory_region_add_subregion(sysmem, 0x04000000 + i * W580_NOR_PART, &pt->alias);
    }
    fclose(f);
    return n;
}
/* Vuelca la NOR entera a un fichero (persistencia entre sesiones). */
static bool w580_nor_volcar(W580Nor *n, const char *ruta)
{
    FILE *f = fopen(ruta, "wb");
    if (!f) { return false; }
    for (unsigned i = 0; i < W580_NOR_NPART; i++) { fwrite(n->part[i].p, 1, W580_NOR_PART, f); }
    return fclose(f) == 0;
}
