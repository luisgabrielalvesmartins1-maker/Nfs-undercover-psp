#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <malloc.h>
#include <png.h>
#include <zlib.h>

PSP_MODULE_INFO("NFS PSP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(20 * 1024);

#define BASE_DIR   "ms0:/PSP/GAME/nfs mobile"
#define ASSET_DIR  BASE_DIR "/assets"
#define LOG_PATH   BASE_DIR "/log.txt"
#define CFG_PATH   BASE_DIR "/textures.cfg"
#define SET_PATH   BASE_DIR "/settings.cfg"
#define CAR_FILE   "car_viper.m3g"
#define TRACK_FILE "causeway.m3g"
#define WHEEL_FILE ""          /* "" = procura sozinho (nome com wheel/tire/tyre/rim) */
#define CAR_SCALE  1.0f        /* L/R ajustam em tempo real; START salva */
#define WHEEL_MIRROR 1         /* 1 = gira 180 graus as rodas do lado esquerdo */
#define FLIP_V     0           /* 1 = inverte o V das texturas */
#define USE_LIGHTING 1         /* 0 = desliga a luz de hardware */
#define SHOW_DIAG_ALWAYS 0     /* 1 = sempre mostra a tela de diagnostico ao abrir */
#define MAX_WV     12
#define MAX_TEX    128

#define BUF_WIDTH  512
#define SCR_WIDTH  480
#define SCR_HEIGHT 272
#define PI_F       3.14159265f

static unsigned int __attribute__((aligned(16))) list[262144];

static FILE *g_log;
#define LOG(...) do { if (g_log) fprintf(g_log, __VA_ARGS__); } while (0)

static char g_diag[2048];
static void diag(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    LOG("%s\n", line);
    if (strlen(g_diag) + strlen(line) + 2 < sizeof g_diag) { strcat(g_diag, line); strcat(g_diag, "\n"); }
}

/* ======================= Leitor binario (M3G = little-endian) ======================= */
typedef struct { const uint8_t *p, *e; int bad; } Rd;

static uint8_t rd8(Rd *r) { if (r->p >= r->e) { r->bad = 1; return 0; } return *r->p++; }
static uint16_t rd16(Rd *r) { uint16_t a = rd8(r); a |= (uint16_t)rd8(r) << 8; return a; }
static uint32_t rd32(Rd *r) { uint32_t a = rd16(r); a |= (uint32_t)rd16(r) << 16; return a; }
static float rdf(Rd *r) { uint32_t u = rd32(r); float f; memcpy(&f, &u, 4); return f; }
static void rdskip(Rd *r, uint32_t n) {
    if ((uint32_t)(r->e - r->p) < n) { r->bad = 1; r->p = r->e; } else r->p += n;
}

/* ======================= Estruturas ======================= */
typedef struct { uint8_t type; const uint8_t *data; uint32_t len; } Obj;
typedef struct { int count, comps, size; int16_t *v; } VArr;

typedef struct {
    Obj *obj; int nobj, cap;
    VArr *va;
    int *timg, *t2dord;
    int ntexobj, fallback;
    char base[64];
    void *bufs[16]; int nbufs;
} M3G;

typedef struct { VArr *pos, *col, *uv, *nrm; float bias[3], scale, ubias[3], uscale; } VBuf;

typedef struct { float u, v; uint32_t color; float nx, ny, nz, x, y, z; } Vertex;
typedef struct { int tex, wrap, start, count; } Batch;
typedef struct {
    Vertex *v; int nv, capv;
    Batch *b; int nb, capb;
    float mn[3], mx[3];
    int nsub, nuv, ntexobj, nimg, next;
    uint32_t slotmask;
    char key[48];
} Model;

typedef struct { void *data; int w, h, alpha, psm; char name[64]; } Tex;
static Tex g_tex[MAX_TEX];
static int g_ntex;
static char g_miss[6][48];
static int g_nmiss;
static char g_imginfo[4][96];
static int g_nimginfo;

static M3G *M;
static Model *OUT;
static int g_sub, g_honor;
static int g_pick = -1, g_active = 1, g_split, g_nsplit;
static Model g_car, g_track, g_wv[MAX_WV];
static int g_nwv, g_wsel;

static void model_free(Model *m) { free(m->v); free(m->b); memset(m, 0, sizeof *m); }

/* ======================= Configuracao (texturas escolhidas / ajustes) ======================= */
typedef struct { char model[48]; int slot; char png[48]; } Cfg;
static Cfg g_cfg[64];
static int g_ncfg;

static const char *cfg_get(const char *model, int slot) {
    for (int i = 0; i < g_ncfg; i++)
        if (g_cfg[i].slot == slot && !strcmp(g_cfg[i].model, model)) return g_cfg[i].png;
    return NULL;
}
static void cfg_set(const char *model, int slot, const char *png) {
    for (int i = 0; i < g_ncfg; i++)
        if (g_cfg[i].slot == slot && !strcmp(g_cfg[i].model, model)) { snprintf(g_cfg[i].png, sizeof g_cfg[i].png, "%s", png); return; }
    if (g_ncfg >= 64) return;
    snprintf(g_cfg[g_ncfg].model, sizeof g_cfg[0].model, "%s", model);
    g_cfg[g_ncfg].slot = slot;
    snprintf(g_cfg[g_ncfg].png, sizeof g_cfg[0].png, "%s", png);
    g_ncfg++;
}
static void cfg_del(const char *model, int slot) {
    for (int i = 0; i < g_ncfg; i++)
        if (g_cfg[i].slot == slot && !strcmp(g_cfg[i].model, model)) { g_cfg[i] = g_cfg[g_ncfg - 1]; g_ncfg--; return; }
}
static void cfg_load(void) {
    g_ncfg = 0;
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[160], m[48], p[48];
    int s;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "%47s %d %47s", m, &s, p) == 3) cfg_set(m, s, p);
    fclose(f);
}
static void cfg_save(void) {
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_ncfg; i++) fprintf(f, "%s %d %s\n", g_cfg[i].model, g_cfg[i].slot, g_cfg[i].png);
    fclose(f);
}
static void settings_load(float *scale, int *flip, int *wheel) {
    FILE *f = fopen(SET_PATH, "r");
    if (!f) return;
    float s; int fl, w;
    if (fscanf(f, "%f %d %d", &s, &fl, &w) == 3 && s > 0.0001f && s < 1000.0f) { *scale = s; *flip = fl; *wheel = w; }
    fclose(f);
}
static void settings_save(float scale, int flip, int wheel) {
    FILE *f = fopen(SET_PATH, "w");
    if (f) { fprintf(f, "%f %d %d\n", scale, flip, wheel); fclose(f); }
}

/* ======================= Texturas ======================= */
static int p2(int v, int cap) { int p = 8; while (p < v && p < cap) p <<= 1; return p; }

/* rgba = bytes R,G,B,A. Reduz com media de caixa (ex.: 1024 -> 512) para potencia de 2 (max 512x512)
   e converte para 16 bits. */
static int tex_create(const uint8_t *rgba, int w, int h, const char *name) {
    if (g_ntex >= MAX_TEX || w <= 0 || h <= 0) return -1;
    int pw = p2(w, 512), ph = p2(h, 512);
    const uint32_t *s = (const uint32_t *)rgba;
    int translucent = 0;
    for (int i = 0, n = w * h; i < n; i++) if ((s[i] >> 24) < 200) translucent++;
    int alpha = translucent > (w * h) / 256;
    uint16_t *d = memalign(16, (size_t)pw * ph * 2);
    if (!d) return -1;
    for (int y = 0; y < ph; y++) {
        int sy0 = (int)((long long)y * h / ph), sy1 = (int)((long long)(y + 1) * h / ph);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int x = 0; x < pw; x++) {
            int sx0 = (int)((long long)x * w / pw), sx1 = (int)((long long)(x + 1) * w / pw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t px = s[yy * w + xx];
                    r += px & 255; g += (px >> 8) & 255; b += (px >> 16) & 255; a += px >> 24; cnt++;
                }
            r /= cnt; g /= cnt; b /= cnt; a /= cnt;
            d[y * pw + x] = alpha
                ? (uint16_t)(((a >> 4) << 12) | ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4))
                : (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
        }
    }
    Tex *t = &g_tex[g_ntex];
    t->data = d; t->w = pw; t->h = ph; t->alpha = alpha;
    t->psm = alpha ? GU_PSM_4444 : GU_PSM_5650;
    snprintf(t->name, sizeof t->name, "%s", name);
    LOG("textura %d: %s (%dx%d -> %dx%d, alpha=%d)\n", g_ntex, name, w, h, pw, ph, alpha);
    return g_ntex++;
}

static uint8_t *load_png(const char *path, int *ow, int *oh) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    uint8_t sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) { fclose(fp); return NULL; }
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) { fclose(fp); return NULL; }
    uint8_t * volatile pixels = NULL;
    png_bytep * volatile rows = NULL;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp); free(pixels); free(rows);
        return NULL;
    }
    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);
    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int ct = png_get_color_type(png, info), bd = png_get_bit_depth(png, info);
    if (bd == 16) png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);
    pixels = malloc((size_t)w * h * 4);
    rows = malloc(sizeof(png_bytep) * h);
    if (!pixels || !rows) png_error(png, "sem memoria");
    for (png_uint_32 y = 0; y < h; y++) rows[y] = pixels + (size_t)y * w * 4;
    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    free(rows);
    *ow = (int)w; *oh = (int)h;
    return pixels;
}

static void lower(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
    dst[i] = 0;
}

static int tex_from_png_file(const char *fname) {
    char low[64];
    lower(low, fname, sizeof low);
    for (int i = 0; i < g_ntex; i++) if (!strcmp(g_tex[i].name, low)) return i;
    char path[256];
    snprintf(path, sizeof path, "%s/%s", ASSET_DIR, fname);
    int w, h;
    uint8_t *px = load_png(path, &w, &h);
    if (!px) {
        LOG("png nao carregado: %s\n", path);
        if (g_nmiss < 6) snprintf(g_miss[g_nmiss++], sizeof g_miss[0], "%s", fname);
        return -1;
    }
    int id = tex_create(px, w, h, low);
    free(px);
    return id;
}

/* ---- lista de PNGs de assets/ (nome + dimensoes lidas do cabecalho) ---- */
typedef struct { char name[48]; int w, h; } PngInfo;
static PngInfo g_pngs[700];
static int g_npng, g_scanned;

static int png_cmp(const void *a, const void *b) { return strcmp(((const PngInfo *)a)->name, ((const PngInfo *)b)->name); }

static void scan_pngs(void) {
    if (g_scanned) return;
    g_scanned = 1;
    SceUID d = sceIoDopen(ASSET_DIR);
    if (d < 0) return;
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        char low[256];
        lower(low, e.d_name, sizeof low);
        size_t n = strlen(low);
        if (n <= 4 || strcmp(low + n - 4, ".png") || g_npng >= 700) continue;
        PngInfo *p = &g_pngs[g_npng];
        snprintf(p->name, sizeof p->name, "%s", e.d_name);
        p->w = p->h = 0;
        char path[300];
        snprintf(path, sizeof path, "%s/%s", ASSET_DIR, e.d_name);
        FILE *f = fopen(path, "rb");
        if (f) {
            uint8_t hd[24];
            if (fread(hd, 1, 24, f) == 24) {
                p->w = (hd[16] << 24) | (hd[17] << 16) | (hd[18] << 8) | hd[19];
                p->h = (hd[20] << 24) | (hd[21] << 16) | (hd[22] << 8) | hd[23];
            }
            fclose(f);
        }
        g_npng++;
    }
    sceIoDclose(d);
    qsort(g_pngs, g_npng, sizeof(PngInfo), png_cmp);
}

static const char *BAD[] = { "wheel", "tire", "tyre", "glass", "shadow", "icon", "thumb", "menu", "logo", "light", "small" };

/* melhor .png cujo nome contem 'base' (ou o trecho depois do 1o '_') */
static int find_png(const char *base, char *out, size_t osz) {
    char b[64], alt[64] = "";
    lower(b, base, sizeof b);
    const char *u = strchr(b, '_');
    if (u && u[1]) snprintf(alt, sizeof alt, "%s", u + 1);
    scan_pngs();
    int best = 0;
    for (int i = 0; i < g_npng; i++) {
        char low[64];
        lower(low, g_pngs[i].name, sizeof low);
        size_t n = strlen(low);
        int hb = strstr(low, b) != NULL;
        int ha = alt[0] && strstr(low, alt) != NULL;
        if (!hb && !ha) continue;
        char ex[80];
        snprintf(ex, sizeof ex, "%s.png", b);
        int s = 1;
        if (!strcmp(low, ex)) s += 100;
        if (hb) s += 50;
        if (ha) s += 30;
        for (unsigned k = 0; k < sizeof BAD / sizeof BAD[0]; k++)
            if (strstr(low, BAD[k]) && !strstr(b, BAD[k])) s -= 40;
        s -= (int)n / 16;
        if (s > best) { best = s; snprintf(out, osz, "%s", g_pngs[i].name); }
    }
    return best > 0;
}

static int fallback_tex(void) {
    if (M->fallback != -2) return M->fallback;
    char file[256];
    int id = -1;
    if (find_png(M->base, file, sizeof file)) id = tex_from_png_file(file);
    LOG("fallback de textura por nome (%s): %s\n", M->base, id >= 0 ? file : "(nenhum png encontrado)");
    M->fallback = id;
    return id;
}

/* ---- Image2D embutida ----
   Formatos padrao: 96=A 97=L 98=LA 99=RGB 100=RGBA. Os valores 121..125 (ex.: 124, 125) sao tratados como
   o formato padrao + 25 (124 = RGB, 125 = RGBA). O layout real e deduzido pelo tamanho dos dados. */
static int px_bpp(int base) { return base == 96 ? 1 : base == 97 ? 1 : base == 98 ? 2 : base == 99 ? 3 : base == 100 ? 4 : 0; }

static void px_expand(int base, const uint8_t *c, uint8_t *o) {
    switch (base) {
    case 96: o[0] = o[1] = o[2] = 255; o[3] = c[0]; break;
    case 97: o[0] = o[1] = o[2] = c[0]; o[3] = 255; break;
    case 98: o[0] = o[1] = o[2] = c[0]; o[3] = c[1]; break;
    case 99: o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255; break;
    default: o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3]; break;
    }
}

static void px16(int base, const uint8_t *c, uint8_t *o) {
    unsigned v = c[0] | (c[1] << 8);
    if (base == 100) {
        o[0] = ((v >> 12) & 15) * 17; o[1] = ((v >> 8) & 15) * 17; o[2] = ((v >> 4) & 15) * 17; o[3] = (v & 15) * 17;
    } else {
        o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255;
    }
}

static uint8_t *decode_image(int fmt, uint32_t w, uint32_t h, const uint8_t *pal, uint32_t pl,
                             const uint8_t *pix, uint32_t xl, char *why) {
    int base = (fmt >= 121 && fmt <= 125) ? fmt - 25 : fmt;
    int bpp = px_bpp(base);
    uint32_t np = w * h;
    uint8_t *out = malloc((size_t)np * 4);
    if (!out) { strcpy(why, "sem memoria"); return NULL; }

    if (pl > 0) {                                        /* paleta: indices de 8 ou 4 bits */
        int bits = (xl >= np) ? 8 : (xl >= (np + 1) / 2) ? 4 : 0;
        if (!bits) { free(out); strcpy(why, "indices"); return NULL; }
        uint32_t entries = 1u << bits, es = pl / entries;
        if (es < 1 || es > 4) { free(out); strcpy(why, "paleta"); return NULL; }
        uint8_t table[256][4];
        for (uint32_t i = 0; i < entries; i++) {
            const uint8_t *c = pal + i * es;
            if (es == 2) px16(base, c, table[i]);
            else if (es == 3) px_expand(99, c, table[i]);
            else if (es == 4) px_expand(100, c, table[i]);
            else px_expand(97, c, table[i]);
        }
        for (uint32_t i = 0; i < np; i++) {
            unsigned idx = (bits == 8) ? pix[i] : ((i & 1) ? (pix[i / 2] & 15) : (pix[i / 2] >> 4));
            memcpy(out + i * 4, table[idx], 4);
        }
        return out;
    }
    if (bpp && xl >= np * bpp) {                         /* cru: RGB / RGBA / L / LA / A */
        for (uint32_t i = 0; i < np; i++) px_expand(base, pix + (size_t)i * bpp, out + i * 4);
        return out;
    }
    if (xl >= np * 2) {                                  /* 16 bits: RGB565 ou RGBA4444 */
        for (uint32_t i = 0; i < np; i++) px16(base, pix + (size_t)i * 2, out + i * 4);
        return out;
    }
    if (xl >= np) {                                      /* 8 bits: tons de cinza */
        for (uint32_t i = 0; i < np; i++) px_expand(97, pix + i, out + i * 4);
        return out;
    }
    free(out);
    strcpy(why, "formato");
    return NULL;
}

static int tex_from_image2d(const Obj *o, uint32_t idx) {
    char name[64];
    snprintf(name, sizeof name, "%s#img%u", M->base, (unsigned)idx);
    for (int i = 0; i < g_ntex; i++) if (!strcmp(g_tex[i].name, name)) return i;   /* ja carregada */

    Rd r = { o->data, o->data + o->len, 0 };
    rd32(&r);
    { uint32_t n = rd32(&r); if (n > 0x10000) return -1; rdskip(&r, n * 4);
      n = rd32(&r); while (n-- && !r.bad) { rd32(&r); uint32_t l = rd32(&r); rdskip(&r, l); } }
    int fmt = rd8(&r), mut = rd8(&r);
    uint32_t w = rd32(&r), h = rd32(&r);
    char *info = g_nimginfo < 4 ? g_imginfo[g_nimginfo++] : NULL;
    if (info) snprintf(info, 96, "%s f%d %ux%u", name, fmt, (unsigned)w, (unsigned)h);
    if (r.bad || mut || w == 0 || h == 0 || w > 4096 || h > 4096) {
        if (info) strncat(info, " sem pixels", 95 - strlen(info));
        return -1;
    }
    uint32_t pl = rd32(&r);
    const uint8_t *pal = r.p;
    rdskip(&r, pl);
    uint32_t xl = rd32(&r);
    const uint8_t *pix = r.p;
    if (r.bad || (uint32_t)(r.e - r.p) < xl) {
        if (info) strncat(info, " tamanho invalido", 95 - strlen(info));
        return -1;
    }
    if (info) { char t[40]; snprintf(t, sizeof t, " pal=%u pix=%u", (unsigned)pl, (unsigned)xl); strncat(info, t, 95 - strlen(info)); }
    LOG("%s fmt=%d %ux%u pal=%u pix=%u\n  pix:", name, fmt, (unsigned)w, (unsigned)h, (unsigned)pl, (unsigned)xl);
    for (uint32_t i = 0; i < 16 && i < xl; i++) LOG(" %02x", pix[i]);
    LOG("\n  pal:");
    for (uint32_t i = 0; i < 16 && i < pl; i++) LOG(" %02x", pal[i]);
    LOG("\n");

    char why[32] = "";
    uint8_t *rgba = decode_image(fmt, w, h, pal, pl, pix, xl, why);
    if (!rgba) {
        if (info) { strncat(info, " ERRO ", 95 - strlen(info)); strncat(info, why, 95 - strlen(info)); }
        return -1;
    }
    int id = tex_create(rgba, (int)w, (int)h, name);
    free(rgba);
    if (info) strncat(info, id >= 0 ? " OK" : " ERRO tex", 95 - strlen(info));
    return id;
}

static int image_tex(uint32_t ii) {
    if (ii == 0 || ii >= (uint32_t)M->nobj) return -1;
    if (M->timg[ii] != -2) return M->timg[ii];
    int id = -1;
    const Obj *o = &M->obj[ii];
    if (o->type == 10) {
        id = tex_from_image2d(o, ii);
    } else if (o->type == 255) {
        char uri[128];
        uint32_t n = o->len < 127 ? o->len : 127;
        memcpy(uri, o->data, n); uri[n] = 0;
        const char *b = uri;
        for (const char *p = uri; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
        char file[128];
        snprintf(file, sizeof file, "%s", b);
        if (!strchr(file, '.')) strncat(file, ".png", sizeof file - strlen(file) - 1);
        LOG("referencia externa de imagem: %s\n", uri);
        id = tex_from_png_file(file);
        if (id < 0) {
            char stem[64], alt[256];
            snprintf(stem, sizeof stem, "%s", file);
            char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
            if (find_png(stem, alt, sizeof alt)) id = tex_from_png_file(alt);
        }
    }
    M->timg[ii] = id;
    return id;
}

/* ======================= Carregamento do M3G ======================= */
static int add_obj(uint8_t type, const uint8_t *data, uint32_t len) {
    if (M->nobj >= M->cap) {
        int nc = M->cap ? M->cap * 2 : 256;
        Obj *n = realloc(M->obj, nc * sizeof(Obj));
        if (!n) return 0;
        M->obj = n; M->cap = nc;
    }
    M->obj[M->nobj].type = type; M->obj[M->nobj].data = data; M->obj[M->nobj].len = len;
    M->nobj++;
    return 1;
}

static int inflate_buf(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, int wbits) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, wbits) != Z_OK) return 0;
    zs.next_in = (Bytef *)src; zs.avail_in = slen;
    zs.next_out = dst;         zs.avail_out = dlen;
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

static const char *load_m3g(const char *path) {
    static const uint8_t magic[12] = { 0xAB, 'J', 'S', 'R', '1', '8', '4', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    FILE *f = fopen(path, "rb");
    if (!f) return "nao consegui abrir o arquivo";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 25) { fclose(f); return "arquivo pequeno demais"; }
    uint8_t *buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); return "falha ao ler o arquivo"; }
    fclose(f);
    M->bufs[M->nbufs++] = buf;
    if (memcmp(buf, magic, 12) != 0) return "cabecalho nao e M3G/JSR-184";

    size_t pos = 12;
    while (pos + 13 <= (size_t)sz) {
        Rd h = { buf + pos, buf + sz, 0 };
        uint8_t comp = rd8(&h);
        uint32_t total = rd32(&h), unc = rd32(&h);
        if (total < 13 || pos + total > (size_t)sz) return "secao corrompida";
        const uint8_t *src = buf + pos + 9, *objs;
        uint32_t srclen = total - 13, len;
        if (comp == 0) {
            objs = src; len = unc <= srclen ? unc : srclen;
        } else if (comp == 1) {
            uint8_t *dst = malloc(unc ? unc : 1);
            if (!dst) return "sem memoria para descomprimir";
            if (M->nbufs < 16) M->bufs[M->nbufs++] = dst;
            if (!inflate_buf(src, srclen, dst, unc, 15) && !inflate_buf(src, srclen, dst, unc, -15))
                return "falha ao descomprimir (zlib)";
            objs = dst; len = unc;
        } else return "tipo de compressao desconhecido";

        Rd s = { objs, objs + len, 0 };
        while (s.p + 5 <= s.e) {
            uint8_t type = rd8(&s);
            uint32_t olen = rd32(&s);
            if ((uint32_t)(s.e - s.p) < olen) return "objeto corrompido";
            if (!add_obj(type, s.p, olen)) return "sem memoria";
            s.p += olen;
        }
        pos += total;
        if (M->nbufs >= 15) break;
    }
    return NULL;
}

static void parse_object3d(Rd *r) {
    rd32(r);
    uint32_t n = rd32(r);
    if (n > 0x10000) { r->bad = 1; return; }
    rdskip(r, n * 4);
    n = rd32(r);
    while (n-- && !r->bad) { rd32(r); uint32_t l = rd32(r); rdskip(r, l); }
}

static void parse_vertex_array(int idx) {
    Rd r = { M->obj[idx].data, M->obj[idx].data + M->obj[idx].len, 0 };
    parse_object3d(&r);
    int size = rd8(&r), comps = rd8(&r), enc = rd8(&r);
    int count = rd16(&r);
    if (r.bad || (size != 1 && size != 2) || comps < 2 || comps > 4 || count == 0) return;
    int16_t *v = malloc((size_t)count * comps * sizeof(int16_t));
    if (!v) return;
    int prev[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < count; i++)
        for (int c = 0; c < comps; c++) {
            int x = (size == 1) ? (int8_t)rd8(&r) : (int16_t)rd16(&r);
            if (enc == 1) {
                prev[c] += x;
                prev[c] = (size == 1) ? (int8_t)prev[c] : (int16_t)prev[c];
                x = prev[c];
            }
            v[i * comps + c] = (int16_t)x;
        }
    if (r.bad) { free(v); return; }
    M->va[idx].count = count; M->va[idx].comps = comps; M->va[idx].size = size; M->va[idx].v = v;
}

static int parse_vbuf(uint32_t idx, VBuf *vb) {
    if (idx == 0 || idx >= (uint32_t)M->nobj || M->obj[idx].type != 21) return 0;
    Rd r = { M->obj[idx].data, M->obj[idx].data + M->obj[idx].len, 0 };
    parse_object3d(&r);
    rd32(&r);
    uint32_t pi = rd32(&r);
    for (int i = 0; i < 3; i++) vb->bias[i] = rdf(&r);
    vb->scale = rdf(&r);
    uint32_t ni = rd32(&r);
    uint32_t ci = rd32(&r);
    uint32_t nt = rd32(&r);
    vb->uv = NULL; vb->uscale = 1.0f; vb->ubias[0] = vb->ubias[1] = vb->ubias[2] = 0;
    if (nt > 0) {
        uint32_t ti = rd32(&r);
        for (int i = 0; i < 3; i++) vb->ubias[i] = rdf(&r);
        vb->uscale = rdf(&r);
        if (!r.bad && ti > 0 && ti < (uint32_t)M->nobj && M->va[ti].v && M->va[ti].comps >= 2) vb->uv = &M->va[ti];
    }
    if (r.bad || pi == 0 || pi >= (uint32_t)M->nobj) return 0;
    vb->pos = &M->va[pi];
    if (!vb->pos->v || vb->pos->comps < 3) return 0;
    vb->col = (ci > 0 && ci < (uint32_t)M->nobj && M->va[ci].v && M->va[ci].comps >= 3) ? &M->va[ci] : NULL;
    vb->nrm = (ni > 0 && ni < (uint32_t)M->nobj && M->va[ni].v && M->va[ni].comps >= 3) ? &M->va[ni] : NULL;
    return 1;
}

static int texture2d_tex(uint32_t ti, int *wrap) {
    if (ti == 0 || ti >= (uint32_t)M->nobj || M->obj[ti].type != 17) return -1;
    Rd r = { M->obj[ti].data, M->obj[ti].data + M->obj[ti].len, 0 };
    parse_object3d(&r);
    if (rd8(&r)) rdskip(&r, 40);
    if (rd8(&r)) rdskip(&r, 64);
    uint32_t img = rd32(&r);
    rdskip(&r, 3);
    rd8(&r);
    int ws = rd8(&r);
    if (r.bad) return -1;
    *wrap = (ws == 241);
    return image_tex(img);
}

/* ordem: textura escolhida no seletor > textura do arquivo > png achado pelo nome */
static int slot_tex(int slot, int nat) {
    const char *o = cfg_get(M->base, slot);
    if (o) {
        if (!strcmp(o, "-")) return -1;
        int id = tex_from_png_file(o);
        if (id >= 0) return id;
    }
    if (nat >= 0) return nat;
    return fallback_tex();
}

static int appearance_tex(uint32_t app, int *wrap, int hasuv) {
    *wrap = 1;
    if (!hasuv) return -1;
    int slot = -1, nat = -1;
    if (app > 0 && app < (uint32_t)M->nobj && M->obj[app].type == 3) {
        Rd r = { M->obj[app].data, M->obj[app].data + M->obj[app].len, 0 };
        parse_object3d(&r);
        rd8(&r);
        for (int i = 0; i < 4; i++) rd32(&r);
        uint32_t nt = rd32(&r);
        if (!r.bad && nt > 0) {
            uint32_t ti = rd32(&r);
            if (ti > 0 && ti < (uint32_t)M->nobj && M->t2dord[ti] >= 0) slot = M->t2dord[ti];
            nat = texture2d_tex(ti, wrap);
        }
    }
    if (slot < 0 && M->ntexobj == 0) slot = 0;
    if (slot < 0) return -1;
    OUT->slotmask |= 1u << (slot & 31);
    return slot_tex(slot, nat);
}

/* ======================= Matrizes ======================= */
static void mat_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void mat_mul(float *o, const float *a, const float *b) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = s;
        }
    memcpy(o, t, 64);
}
static void mat_pt(const float *m, const float *p, float *o) {
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2]  * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6]  * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

static void parse_transformable(Rd *r, float *m) {
    mat_ident(m);
    if (rd8(r)) {
        float t[3], sc[3], ax[3], ang;
        for (int i = 0; i < 3; i++) t[i] = rdf(r);
        for (int i = 0; i < 3; i++) sc[i] = rdf(r);
        ang = rdf(r);
        for (int i = 0; i < 3; i++) ax[i] = rdf(r);
        float T[16], R[16], S[16];
        mat_ident(T); T[3] = t[0]; T[7] = t[1]; T[11] = t[2];
        mat_ident(S); S[0] = sc[0]; S[5] = sc[1]; S[10] = sc[2];
        mat_ident(R);
        float len = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (len > 1e-6f && ang != 0.0f) {
            float x = ax[0] / len, y = ax[1] / len, z = ax[2] / len;
            float a = ang * PI_F / 180.0f, c = cosf(a), s = sinf(a), k = 1.0f - c;
            R[0] = c + x * x * k;      R[1] = x * y * k - z * s;  R[2]  = x * z * k + y * s;
            R[4] = y * x * k + z * s;  R[5] = c + y * y * k;      R[6]  = y * z * k - x * s;
            R[8] = z * x * k - y * s;  R[9] = z * y * k + x * s;  R[10] = c + z * z * k;
        }
        mat_mul(m, T, R);
        mat_mul(m, m, S);
    }
    if (rd8(r)) {
        float G[16];
        for (int i = 0; i < 16; i++) G[i] = rdf(r);
        mat_mul(m, m, G);
    }
}

static void parse_node(Rd *r, float *m, int *enabled) {
    parse_object3d(r);
    parse_transformable(r, m);
    *enabled = rd8(r) != 0;
    rd8(r); rd8(r); rd32(r);
    if (rd8(r)) { rd8(r); rd8(r); rd32(r); rd32(r); }
}

/* ======================= Geometria ======================= */
static int reserve(int n) {
    if (OUT->nv + n <= OUT->capv) return 1;
    int nc = OUT->capv ? OUT->capv + OUT->capv / 2 + 1024 : 4096;
    Vertex *nv = realloc(OUT->v, (size_t)nc * sizeof(Vertex));
    if (!nv) return 0;
    OUT->v = nv; OUT->capv = nc;
    return 1;
}

static void add_batch(int start, int tex, int wrap) {
    int count = OUT->nv - start;
    if (count <= 0) return;
    if (OUT->nb > 0) {
        Batch *l = &OUT->b[OUT->nb - 1];
        if (l->tex == tex && l->wrap == wrap) { l->count += count; return; }
    }
    if (OUT->nb >= OUT->capb) {
        int nc = OUT->capb ? OUT->capb * 2 : 64;
        Batch *nb = realloc(OUT->b, nc * sizeof(Batch));
        if (!nb) return;
        OUT->b = nb; OUT->capb = nc;
    }
    Batch *b = &OUT->b[OUT->nb++];
    b->tex = tex; b->wrap = wrap; b->start = start; b->count = count;
}

static const uint8_t PAL[6][3] = {
    {200, 40, 40}, {190, 190, 200}, {60, 60, 70}, {220, 170, 40}, {70, 130, 210}, {90, 180, 100}
};

static int col8(const VArr *a, int i, int c) {
    int v = a->v[i * a->comps + c];
    return a->size == 1 ? (v & 0xFF) : ((v >> 8) & 0xFF);
}

static void add_tri(const VBuf *vb, const float *world, const uint32_t id[3], int sub, int tex) {
    float P[3][3];
    for (int i = 0; i < 3; i++) {
        if (id[i] >= (uint32_t)vb->pos->count) return;
        const int16_t *v = vb->pos->v + id[i] * vb->pos->comps;
        float lp[3] = { v[0] * vb->scale + vb->bias[0], v[1] * vb->scale + vb->bias[1], v[2] * vb->scale + vb->bias[2] };
        mat_pt(world, lp, P[i]);
    }
    float e1[3], e2[3], n[3];
    for (int k = 0; k < 3; k++) { e1[k] = P[1][k] - P[0][k]; e2[k] = P[2][k] - P[0][k]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-9f) return;
    n[0] /= len; n[1] /= len; n[2] /= len;

    if (!reserve(3)) return;
    for (int i = 0; i < 3; i++) {
        int r, g, b;
        if (vb->col && id[i] < (uint32_t)vb->col->count) {
            r = col8(vb->col, id[i], 0); g = col8(vb->col, id[i], 1); b = col8(vb->col, id[i], 2);
        } else if (tex >= 0) { r = g = b = 255; }
        else { r = PAL[sub % 6][0]; g = PAL[sub % 6][1]; b = PAL[sub % 6][2]; }

        float nn[3] = { n[0], n[1], n[2] };
        if (vb->nrm && id[i] < (uint32_t)vb->nrm->count) {
            const int16_t *q = vb->nrm->v + id[i] * vb->nrm->comps;
            float a = q[0], bb = q[1], cc = q[2];
            float l = sqrtf(a * a + bb * bb + cc * cc);
            if (l > 1e-6f) {
                a /= l; bb /= l; cc /= l;
                float x_ = world[0] * a + world[1] * bb + world[2] * cc;
                float y_ = world[4] * a + world[5] * bb + world[6] * cc;
                float z_ = world[8] * a + world[9] * bb + world[10] * cc;
                float wl = sqrtf(x_ * x_ + y_ * y_ + z_ * z_);
                if (wl > 1e-6f) { nn[0] = x_ / wl; nn[1] = y_ / wl; nn[2] = z_ / wl; }
            }
        }
        Vertex *o = &OUT->v[OUT->nv++];
        o->u = 0; o->v = 0;
        if (vb->uv && id[i] < (uint32_t)vb->uv->count) {
            const int16_t *t = vb->uv->v + id[i] * vb->uv->comps;
            o->u = t[0] * vb->uscale + vb->ubias[0];
            o->v = t[1] * vb->uscale + vb->ubias[1];
            if (FLIP_V) o->v = 1.0f - o->v;
        }
        o->color = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        o->nx = nn[0]; o->ny = nn[1]; o->nz = nn[2];
        o->x = P[i][0]; o->y = P[i][1]; o->z = P[i][2];
        for (int k = 0; k < 3; k++) {
            if (P[i][k] < OUT->mn[k]) OUT->mn[k] = P[i][k];
            if (P[i][k] > OUT->mx[k]) OUT->mx[k] = P[i][k];
        }
    }
}

static void emit_strips(const VBuf *vb, uint32_t ib, const float *world, int sub, int tex) {
    if (ib == 0 || ib >= (uint32_t)M->nobj || M->obj[ib].type != 11) return;
    Rd r = { M->obj[ib].data, M->obj[ib].data + M->obj[ib].len, 0 };
    parse_object3d(&r);
    int enc = rd8(&r);
    uint32_t first = 0, n = 0;
    uint32_t *arr = NULL;
    if (enc == 0)      first = rd32(&r);
    else if (enc == 1) first = rd8(&r);
    else if (enc == 2) first = rd16(&r);
    else if (enc >= 128 && enc <= 130) {
        n = rd32(&r);
        if (n > (uint32_t)(r.e - r.p)) return;
        arr = malloc(n * 4 + 4);
        if (!arr) return;
        for (uint32_t i = 0; i < n; i++)
            arr[i] = (enc == 128) ? rd32(&r) : (enc == 129) ? rd8(&r) : rd16(&r);
    } else return;
    uint32_t ns = rd32(&r);
    if (r.bad || ns > (uint32_t)(r.e - r.p) / 4) { free(arr); return; }
    uint32_t pos = 0;
    for (uint32_t s = 0; s < ns; s++) {
        uint32_t len = rd32(&r);
        for (uint32_t k = 0; k + 2 < len; k++) {
            uint32_t id[3];
            for (int q = 0; q < 3; q++) {
                uint32_t j = pos + k + q;
                if (arr) { if (j >= n) goto done; id[q] = arr[j]; }
                else id[q] = first + j;
            }
            if (k & 1) { uint32_t t = id[0]; id[0] = id[1]; id[1] = t; }
            if (id[0] == id[1] || id[1] == id[2] || id[0] == id[2]) continue;
            add_tri(vb, world, id, sub, tex);
        }
        pos += len;
    }
done:
    free(arr);
}

static void visit(int idx, const float *parent, int depth) {
    if (idx <= 0 || idx >= M->nobj || depth > 16) return;
    Obj *o = &M->obj[idx];
    int t = o->type;
    if (t != 9 && t != 22 && t != 14 && t != 15 && t != 16) return;
    Rd r = { o->data, o->data + o->len, 0 };
    float local[16], world[16];
    int en;
    parse_node(&r, local, &en);
    if (!en && g_honor) return;
    mat_mul(world, parent, local);
    if (t == 9 || t == 22) {
        uint32_t n = rd32(&r);
        if (g_pick < 0 && g_split == 0 && n >= 4) { g_split = idx; g_nsplit = (int)n; }
        for (uint32_t k = 0; k < n && !r.bad; k++) {
            int c = (int)rd32(&r);
            if (g_pick >= 0 && idx == g_split) {
                if ((int)k != g_pick) continue;
                int sv = g_active;
                g_active = 1;
                visit(c, world, depth + 1);
                g_active = sv;
            } else visit(c, world, depth + 1);
        }
    } else {
        if (!g_active) return;
        uint32_t vbi = rd32(&r), ns = rd32(&r);
        VBuf vb;
        if (r.bad || !parse_vbuf(vbi, &vb)) { LOG("  malha obj=%d IGNORADA\n", idx); return; }
        for (uint32_t k = 0; k < ns && !r.bad; k++) {
            uint32_t ib = rd32(&r), app = rd32(&r);
            int wrap, tex = appearance_tex(app, &wrap, vb.uv != NULL);
            int start = OUT->nv;
            emit_strips(&vb, ib, world, g_sub, tex);
            add_batch(start, tex, wrap);
            OUT->nsub++;
            if (vb.uv) OUT->nuv++;
            if (OUT->nv > start) {
                float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
                for (int i = start; i < OUT->nv; i++) {
                    float p[3] = { OUT->v[i].x, OUT->v[i].y, OUT->v[i].z };
                    for (int q = 0; q < 3; q++) { if (p[q] < mn[q]) mn[q] = p[q]; if (p[q] > mx[q]) mx[q] = p[q]; }
                }
                LOG("  malha obj=%d sub=%d tris=%d uv=%d nrm=%d tex=%d bbox[%.1f %.1f %.1f]-[%.1f %.1f %.1f]\n",
                    idx, g_sub, (OUT->nv - start) / 3, vb.uv != NULL, vb.nrm != NULL, tex, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
            }
            g_sub++;
        }
    }
}

static const char *load_model(const char *file, Model *out, int honorHidden, int pick) {
    M3G m;
    memset(&m, 0, sizeof m);
    m.nobj = 1; m.fallback = -2;
    char stem[64];
    lower(stem, file, sizeof stem);
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    snprintf(m.base, sizeof m.base, "%s", stem);
    model_free(out);
    for (int k = 0; k < 3; k++) { out->mn[k] = 1e30f; out->mx[k] = -1e30f; }
    snprintf(out->key, sizeof out->key, "%s", m.base);
    M = &m; OUT = out; g_sub = 0; g_honor = honorHidden;
    g_pick = pick; g_active = (pick < 0);
    if (pick < 0) { g_split = 0; g_nsplit = 0; }

    char path[256];
    snprintf(path, sizeof path, "%s/%s", ASSET_DIR, file);
    LOG("=== %s ===\n", file);
    const char *err = load_m3g(path);
    if (err) return err;

    int cnt[256];
    memset(cnt, 0, sizeof cnt);
    for (int i = 1; i < m.nobj; i++) cnt[m.obj[i].type]++;
    m.ntexobj = cnt[17];
    out->ntexobj = cnt[17]; out->nimg = cnt[10]; out->next = cnt[255];
    LOG("objetos: Mesh=%d Skinned=%d Morph=%d Group=%d World=%d Appearance=%d Texture2D=%d Image2D=%d ExtRef=%d VertexArray=%d\n",
        cnt[14], cnt[16], cnt[15], cnt[9], cnt[22], cnt[3], cnt[17], cnt[10], cnt[255], cnt[20]);

    m.va = calloc(m.nobj, sizeof(VArr));
    m.timg = malloc(m.nobj * sizeof(int));
    m.t2dord = malloc(m.nobj * sizeof(int));
    uint8_t *ref = calloc(m.nobj, 1);
    if (!m.va || !m.timg || !m.t2dord || !ref) return "sem memoria";
    int ord = 0;
    for (int i = 0; i < m.nobj; i++) { m.timg[i] = -2; m.t2dord[i] = -1; }
    for (int i = 1; i < m.nobj; i++) {
        if (m.obj[i].type == 17) m.t2dord[i] = ord++;
        if (m.obj[i].type == 20) parse_vertex_array(i);
    }
    /* decodifica as imagens embutidas ANTES de montar a geometria (menor pico de memoria) */
    for (int i = 1; i < m.nobj; i++)
        if (m.obj[i].type == 17) { int w; texture2d_tex((uint32_t)i, &w); }

    for (int i = 1; i < m.nobj; i++) {
        if (m.obj[i].type != 9 && m.obj[i].type != 22) continue;
        Rd r = { m.obj[i].data, m.obj[i].data + m.obj[i].len, 0 };
        float tmp[16]; int en;
        parse_node(&r, tmp, &en);
        uint32_t n = rd32(&r);
        for (uint32_t k = 0; k < n && !r.bad; k++) {
            uint32_t c = rd32(&r);
            if (c > 0 && c < (uint32_t)m.nobj) ref[c] = 1;
        }
    }
    float id[16];
    mat_ident(id);
    for (int i = 1; i < m.nobj; i++) {
        int t = m.obj[i].type;
        if ((t == 9 || t == 14 || t == 15 || t == 16 || t == 22) && !ref[i]) visit(i, id, 0);
    }
    free(ref);
    LOG("total: %d triangulos, %d lotes\n", out->nv / 3, out->nb);

    if (out->nv > 0 && out->capv > out->nv) {          /* devolve a sobra de memoria */
        Vertex *nv = realloc(out->v, (size_t)out->nv * sizeof(Vertex));
        if (nv) { out->v = nv; out->capv = out->nv; }
    }
    for (int i = 0; i < m.nobj; i++) free(m.va[i].v);
    free(m.va); free(m.timg); free(m.t2dord); free(m.obj);
    for (int i = 0; i < m.nbufs; i++) free(m.bufs[i]);
    M = NULL;
    if (out->nv == 0 && pick < 0) return "nenhuma malha/triangulo encontrado";
    return NULL;
}

/* ======================= Rodas ======================= */
static int find_wheel_file(const char *carbase, char *out, size_t osz) {
    char b[64], alt[64] = "";
    lower(b, carbase, sizeof b);
    const char *u = strchr(b, '_');
    if (u && u[1]) snprintf(alt, sizeof alt, "%s", u + 1);
    SceUID d = sceIoDopen(ASSET_DIR);
    if (d < 0) return 0;
    SceIoDirent e;
    int best = -9999, found = 0;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        char low[256];
        lower(low, e.d_name, sizeof low);
        size_t n = strlen(low);
        if (n <= 4 || strcmp(low + n - 4, ".m3g")) continue;
        if (!strstr(low, "wheel") && !strstr(low, "tire") && !strstr(low, "tyre") && !strstr(low, "rim")) continue;
        int s = 10 - (int)n / 16;
        if (alt[0] && strstr(low, alt)) s += 50;
        if (strstr(low, b)) s += 20;
        LOG("candidato de roda: %s (pontos %d)\n", e.d_name, s);
        if (s > best) { best = s; snprintf(out, osz, "%s", e.d_name); found = 1; }
    }
    sceIoDclose(d);
    return found;
}

/* arquivo de rodas com varios filhos (>=4) = catalogo: cada filho e uma variante */
static const char *load_wheels(const char *file) {
    for (int i = 0; i < MAX_WV; i++) model_free(&g_wv[i]);
    g_nwv = 0;
    const char *e = load_model(file, &g_wv[0], 0, -1);
    if (e) return e;
    int ns = g_nsplit, sp = g_split;
    g_nwv = 1;
    if (ns >= 4) {
        model_free(&g_wv[0]);
        g_nwv = 0;
        for (int k = 0; k < ns && k < 40 && g_nwv < MAX_WV; k++) {
            g_split = sp; g_nsplit = ns;
            if (!load_model(file, &g_wv[g_nwv], 0, k) && g_wv[g_nwv].nv > 0) g_nwv++;
            else model_free(&g_wv[g_nwv]);
        }
        if (g_nwv == 0) return "nenhuma variante de roda";
    }
    return NULL;
}

/* centra a roda, alinha o eixo com X e corrige escala absurda */
static void wheel_prepare(Model *w, float carLen, float *R, float *halfW) {
    float ext[3] = { w->mx[0] - w->mn[0], w->mx[1] - w->mn[1], w->mx[2] - w->mn[2] };
    float c[3] = { (w->mn[0] + w->mx[0]) * 0.5f, (w->mn[1] + w->mx[1]) * 0.5f, (w->mn[2] + w->mx[2]) * 0.5f };
    int ax = 0;
    if (ext[1] < ext[ax]) ax = 1;
    if (ext[2] < ext[ax]) ax = 2;
    float diam = fmaxf(ext[(ax + 1) % 3], ext[(ax + 2) % 3]);
    if (diam < 1e-6f) diam = 1.0f;
    float ratio = diam / carLen;
    float s = (ratio < 0.08f || ratio > 0.30f) ? (0.16f * carLen / diam) : 1.0f;
    for (int i = 0; i < w->nv; i++) {
        Vertex *v = &w->v[i];
        float x = v->x - c[0], y = v->y - c[1], z = v->z - c[2], t;
        float nx = v->nx, ny = v->ny, nz = v->nz;
        if (ax == 2)      { t = z; z = -x; x = t;  t = nz; nz = -nx; nx = t; }
        else if (ax == 1) { t = -y; y = x; x = t;  t = -ny; ny = nx; nx = t; }
        v->x = x * s; v->y = y * s; v->z = z * s;
        v->nx = nx; v->ny = ny; v->nz = nz;
    }
    *R = diam * s * 0.5f;
    *halfW = ext[ax] * s * 0.5f;
    LOG("roda: eixo=%d diametro=%.2f largura=%.2f escala=%.3f\n", ax, diam, ext[ax], s);
}

/* ======================= Terreno ======================= */
typedef struct { int ok, nx, nz; float x0, z0, cs; int *start, *list; } Grid;
static Grid g_grid;

static float tri_ny(const Vertex *v) {
    float ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, az = v[1].z - v[0].z;
    float bx = v[2].x - v[0].x, by = v[2].y - v[0].y, bz = v[2].z - v[0].z;
    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);
    return len < 1e-12f ? 0.0f : fabsf(ny) / len;
}

static int grid_build(Grid *g, const Model *t) {
    float ex = t->mx[0] - t->mn[0], ez = t->mx[2] - t->mn[2];
    float ext = ex > ez ? ex : ez;
    g->cs = ext / 128.0f; if (g->cs <= 0) g->cs = 1.0f;
    g->x0 = t->mn[0]; g->z0 = t->mn[2];
    g->nx = (int)(ex / g->cs) + 1; g->nz = (int)(ez / g->cs) + 1;
    int nc = g->nx * g->nz;
    g->start = calloc(nc + 1, sizeof(int));
    if (!g->start) return 0;
    int *fill = NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (int bi = 0; bi < t->nb; bi++) {
            const Batch *bt = &t->b[bi];
            if (bt->tex >= 0 && g_tex[bt->tex].alpha) continue;
            int t0 = bt->start / 3, t1 = (bt->start + bt->count) / 3;
            for (int i = t0; i < t1; i++) {
                const Vertex *v = &t->v[i * 3];
                float minx = fminf(v[0].x, fminf(v[1].x, v[2].x)), maxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x));
                float minz = fminf(v[0].z, fminf(v[1].z, v[2].z)), maxz = fmaxf(v[0].z, fmaxf(v[1].z, v[2].z));
                int ix0 = (int)((minx - g->x0) / g->cs), ix1 = (int)((maxx - g->x0) / g->cs);
                int iz0 = (int)((minz - g->z0) / g->cs), iz1 = (int)((maxz - g->z0) / g->cs);
                if (ix0 < 0) ix0 = 0;
                if (iz0 < 0) iz0 = 0;
                if (ix1 >= g->nx) ix1 = g->nx - 1;
                if (iz1 >= g->nz) iz1 = g->nz - 1;
                for (int iz = iz0; iz <= iz1; iz++)
                    for (int ix = ix0; ix <= ix1; ix++) {
                        int c = iz * g->nx + ix;
                        if (pass == 0) g->start[c + 1]++; else g->list[fill[c]++] = i;
                    }
            }
        }
        if (pass == 0) {
            for (int c = 0; c < nc; c++) g->start[c + 1] += g->start[c];
            if (g->start[nc] > 3000000) return 0;
            g->list = malloc((size_t)(g->start[nc] + 1) * sizeof(int));
            fill = malloc(nc * sizeof(int));
            if (!g->list || !fill) return 0;
            memcpy(fill, g->start, nc * sizeof(int));
        }
    }
    free(fill);
    return 1;
}

/* lowest=1: menor superficie; senao a mais alta com y <= maxY */
static int ground_at(float x, float z, float maxY, int lowest, float *outY) {
    const Grid *g = &g_grid;
    if (!g->ok) return 0;
    int ix = (int)floorf((x - g->x0) / g->cs), iz = (int)floorf((z - g->z0) / g->cs);
    if (ix < 0 || iz < 0 || ix >= g->nx || iz >= g->nz) return 0;
    int c = iz * g->nx + ix, found = 0;
    float best = 0;
    for (int k = g->start[c]; k < g->start[c + 1]; k++) {
        const Vertex *v = &g_track.v[g->list[k] * 3];
        float x1 = v[0].x, z1 = v[0].z, x2 = v[1].x, z2 = v[1].z, x3 = v[2].x, z3 = v[2].z;
        float den = (z2 - z3) * (x1 - x3) + (x3 - x2) * (z1 - z3);
        if (fabsf(den) < 1e-9f) continue;
        float l1 = ((z2 - z3) * (x - x3) + (x3 - x2) * (z - z3)) / den;
        float l2 = ((z3 - z1) * (x - x3) + (x1 - x3) * (z - z3)) / den;
        float l3 = 1.0f - l1 - l2;
        if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
        if (tri_ny(v) < 0.35f) continue;
        float y = l1 * v[0].y + l2 * v[1].y + l3 * v[2].y;
        if (lowest) { if (!found || y < best) { best = y; found = 1; } }
        else if (y <= maxY && (!found || y > best)) { best = y; found = 1; }
    }
    if (found) *outY = best;
    return found;
}

static int is_road(const char *n) {
    static const char *k[] = { "road", "asph", "tarm", "street", "track", "bridge", "deck", "pave", "lane", "highway", "hwy", "race" };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (strstr(n, k[i])) return 1;
    return 0;
}

static void find_start(float u, float *sx, float *sy, float *sz, char *how, size_t hsz) {
    float cx = (g_track.mn[0] + g_track.mx[0]) * 0.5f, cz = (g_track.mn[2] + g_track.mx[2]) * 0.5f;
    *sx = cx; *sy = g_track.mx[1]; *sz = cz;
    snprintf(how, hsz, "centro (sem grade de chao)");
    if (!g_grid.ok) return;

    float bestd = 1e30f, bx = 0, by = 0, bz = 0;
    int have = 0;
    const char *bn = "";
    for (int bi = 0; bi < g_track.nb; bi++) {
        const Batch *b = &g_track.b[bi];
        if (b->tex < 0 || g_tex[b->tex].alpha || !is_road(g_tex[b->tex].name)) continue;
        for (int i = b->start; i + 2 < b->start + b->count; i += 3) {
            const Vertex *v = &g_track.v[i];
            if (tri_ny(v) < 0.6f) continue;
            float tx = (v[0].x + v[1].x + v[2].x) / 3.0f, tz = (v[0].z + v[1].z + v[2].z) / 3.0f;
            float d = (tx - cx) * (tx - cx) + (tz - cz) * (tz - cz);
            if (d < bestd) { bestd = d; bx = tx; bz = tz; by = (v[0].y + v[1].y + v[2].y) / 3.0f; have = 1; bn = g_tex[b->tex].name; }
        }
    }
    if (have) {
        float y;
        if (!ground_at(bx, bz, by + 1.0f * u, 0, &y)) y = by;
        *sx = bx; *sy = y; *sz = bz;
        snprintf(how, hsz, "textura '%.28s'", bn);
        return;
    }
    float step = g_grid.cs * 2.0f;
    for (int ring = 0; ring < 64; ring++) {
        int n = ring ? ring * 8 : 1;
        for (int k = 0; k < n; k++) {
            float a = 6.2831853f * k / n;
            float x = cx + cosf(a) * ring * step, z = cz + sinf(a) * ring * step, y;
            if (ground_at(x, z, 1e9f, 0, &y)) { *sx = x; *sy = y; *sz = z; snprintf(how, hsz, "espiral (mais alta)"); return; }
        }
    }
}

/* ======================= Callbacks do PSP ======================= */
static int exit_callback(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}

/* ======================= Graficos ======================= */
static void init_gu(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)0x88000, BUF_WIDTH);
    sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthMask(0);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void draw_model(const Model *md) {
    const int vt = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    for (int i = 0; i < md->nb; i++) {
        const Batch *b = &md->b[i];
        if (b->tex >= 0) {
            const Tex *t = &g_tex[b->tex];
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(t->psm, 0, 0, 0);
            sceGuTexImage(0, t->w, t->h, t->w, t->data);
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexFilter(GU_LINEAR, GU_LINEAR);
            sceGuTexWrap(b->wrap ? GU_REPEAT : GU_CLAMP, b->wrap ? GU_REPEAT : GU_CLAMP);
            if (t->alpha) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0x60, 0xFF); }
            else sceGuDisable(GU_ALPHA_TEST);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
            sceGuDisable(GU_ALPHA_TEST);
        }
        for (int off = 0; off < b->count; off += 60000) {
            int n = b->count - off;
            if (n > 60000) n = 60000;
            sceGumDrawArray(GU_TRIANGLES, vt, n, 0, md->v + b->start + off);
        }
    }
}

static void car_base(float px, float py, float pz, float heading, float pitch, float roll, float scale) {
    ScePspFVector3 pos = { px, py, pz }, sc = { scale, scale, scale };
    sceGumLoadIdentity();
    sceGumTranslate(&pos);
    sceGumRotateY(-heading);
    sceGumRotateX(pitch);
    sceGumRotateZ(roll);
    sceGumScale(&sc);
}

/* ======================= Seletor de texturas (tela de texto + miniatura) ======================= */
static unsigned g_pprev;
static int g_prep;

static unsigned in_poll(void) {
    SceCtrlData p;
    sceCtrlPeekBufferPositive(&p, 1);
    unsigned b = p.Buttons, pressed = b & ~g_pprev;
    unsigned ud = PSP_CTRL_UP | PSP_CTRL_DOWN;
    if (b & ud) { if (++g_prep > 20 && (g_prep % 3) == 0) pressed |= (b & ud); } else g_prep = 0;
    g_pprev = b;
    return pressed;
}

static void preview_png(int idx) {
    volatile uint32_t *vr = (volatile uint32_t *)0x44000000;
    for (int y = 0; y < 150; y++) for (int x = 0; x < 150; x++) vr[(40 + y) * 512 + 324 + x] = 0xFF202020;
    if (idx < 0) return;
    char path[300];
    snprintf(path, sizeof path, "%s/%s", ASSET_DIR, g_pngs[idx].name);
    int w, h;
    uint8_t *px = load_png(path, &w, &h);
    if (!px) return;
    const uint32_t *s = (const uint32_t *)px;
    int bw = w >= h ? 150 : 150 * w / h, bh = h > w ? 150 : 150 * h / w;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            vr[(40 + y) * 512 + 324 + x] = s[(y * h / bh) * w + (x * w / bw)] | 0xFF000000u;
    free(px);
}

/* devolve indice do png, -1 = sem textura, -2 = voltar, -3 = automatico */
static int choose_png(const char *title, const char *cur) {
    int sel = 0, shown = -2;
    if (cur) for (int i = 0; i < g_npng; i++) if (!strcmp(g_pngs[i].name, cur)) sel = i;
    for (;;) {
        int top = sel - 10;
        if (top < 0) top = 0;
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("%-39.39s\n", title);
        pspDebugScreenPrintf("%-39.39s\n", "X=usar TRI=sem textura");
        pspDebugScreenPrintf("%-39.39s\n", "QUAD=auto O=voltar L/R=pagina");
        for (int r = 0; r < 24; r++) {
            int i = top + r;
            char line[80];
            if (i < g_npng) snprintf(line, sizeof line, "%c%-24.24s %dx%d", i == sel ? '>' : ' ', g_pngs[i].name, g_pngs[i].w, g_pngs[i].h);
            else line[0] = 0;
            pspDebugScreenPrintf("%-39.39s\n", line);
        }
        if (shown != sel) { preview_png(sel); shown = sel; }
        sceDisplayWaitVblankStart();
        unsigned p = in_poll();
        if ((p & PSP_CTRL_UP) && sel > 0) sel--;
        if ((p & PSP_CTRL_DOWN) && sel < g_npng - 1) sel++;
        if (p & PSP_CTRL_LTRIGGER) { sel -= 10; if (sel < 0) sel = 0; }
        if (p & PSP_CTRL_RTRIGGER) { sel += 10; if (sel > g_npng - 1) sel = g_npng - 1; }
        if (p & PSP_CTRL_CROSS) return sel;
        if (p & PSP_CTRL_TRIANGLE) return -1;
        if (p & PSP_CTRL_SQUARE) return -3;
        if (p & PSP_CTRL_CIRCLE) return -2;
    }
}

static int picker(void) {
    scan_pngs();
    pspDebugScreenInitEx((void *)0x44000000, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    if (g_npng == 0) {
        pspDebugScreenPrintf("Nenhum .png encontrado em assets/\n");
        sceKernelDelayThread(3000000);
        return 0;
    }
    struct { const char *key; int slot; } sl[40];
    int ns = 0;
    for (int m = 0; m < 3; m++) {
        const Model *md = m == 0 ? &g_car : m == 1 ? &g_wv[0] : &g_track;
        uint32_t mask = md->slotmask;
        if (m == 1) { mask = 0; for (int k = 0; k < g_nwv; k++) mask |= g_wv[k].slotmask; }
        for (int b = 0; b < 32 && ns < 40; b++) if (mask & (1u << b)) { sl[ns].key = md->key; sl[ns].slot = b; ns++; }
    }
    SceCtrlData p0;
    sceCtrlPeekBufferPositive(&p0, 1);
    g_pprev = p0.Buttons;
    if (ns == 0) {
        pspDebugScreenPrintf("Nenhuma textura a escolher (sem UV).\n");
        sceKernelDelayThread(3000000);
        return 0;
    }
    int sel = 0, changed = 0;
    pspDebugScreenClear();
    for (;;) {
        int top = sel - 12;
        if (top < 0) top = 0;
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("%-39.39s\n", "ESCOLHA AS TEXTURAS");
        pspDebugScreenPrintf("%-39.39s\n", "CIMA/BAIXO  X=escolher PNG");
        pspDebugScreenPrintf("%-39.39s\n", "START=salvar  O=cancelar");
        for (int r = 0; r < 26; r++) {
            int i = top + r;
            char line[96];
            if (i < ns) {
                const char *c = cfg_get(sl[i].key, sl[i].slot);
                snprintf(line, sizeof line, "%c%s/%d: %s", i == sel ? '>' : ' ', sl[i].key, sl[i].slot, c ? c : "(auto)");
            } else line[0] = 0;
            pspDebugScreenPrintf("%-39.39s\n", line);
        }
        sceDisplayWaitVblankStart();
        unsigned p = in_poll();
        if ((p & PSP_CTRL_UP) && sel > 0) sel--;
        if ((p & PSP_CTRL_DOWN) && sel < ns - 1) sel++;
        if (p & PSP_CTRL_CROSS) {
            char title[64];
            snprintf(title, sizeof title, "%s/%d", sl[sel].key, sl[sel].slot);
            pspDebugScreenClear();
            int r = choose_png(title, cfg_get(sl[sel].key, sl[sel].slot));
            if (r >= 0) { cfg_set(sl[sel].key, sl[sel].slot, g_pngs[r].name); changed = 1; }
            else if (r == -1) { cfg_set(sl[sel].key, sl[sel].slot, "-"); changed = 1; }
            else if (r == -3) { cfg_del(sl[sel].key, sl[sel].slot); changed = 1; }
            pspDebugScreenClear();
        }
        if (p & PSP_CTRL_START) { if (changed) cfg_save(); return changed; }
        if (p & PSP_CTRL_CIRCLE) { cfg_load(); return 0; }
    }
}

/* ======================= Montagem da cena ======================= */
static float g_carLen, g_carYaw0, g_wx, g_wz, g_trackExt, g_sx, g_sy, g_sz;
static float g_wR[MAX_WV], g_wHW[MAX_WV];
static char g_wmsg[96], g_how[64];
static const char *g_what = "";

static int count_tex_batches(const Model *m) {
    int n = 0;
    for (int i = 0; i < m->nb; i++) if (m->b[i].tex >= 0) n++;
    return n;
}

static const char *build_all(float scale) {
    g_diag[0] = 0; g_nmiss = 0; g_nimginfo = 0;
    if (g_log) fclose(g_log);
    g_log = fopen(LOG_PATH, "w");

    scan_pngs();
    for (int i = 0; i < g_npng; i++) LOG("png: %s %dx%d\n", g_pngs[i].name, g_pngs[i].w, g_pngs[i].h);

    g_what = CAR_FILE;
    const char *err = load_model(CAR_FILE, &g_car, 0, -1);
    if (err) return err;
    g_what = TRACK_FILE;
    err = load_model(TRACK_FILE, &g_track, 1, -1);
    if (err) return err;

    char stem[64], wfile[256] = "";
    snprintf(stem, sizeof stem, "%s", CAR_FILE);
    { char *d = strrchr(stem, '.'); if (d) *d = 0; }
    if (WHEEL_FILE[0]) snprintf(wfile, sizeof wfile, "%s", WHEEL_FILE);
    else find_wheel_file(stem, wfile, sizeof wfile);
    g_nwv = 0;
    snprintf(g_wmsg, sizeof g_wmsg, "nenhum arquivo de roda em assets/");
    if (wfile[0]) {
        const char *we = load_wheels(wfile);
        if (we) snprintf(g_wmsg, sizeof g_wmsg, "%.28s: %.40s", wfile, we);
        else snprintf(g_wmsg, sizeof g_wmsg, "%.28s: %d variante(s) (CIMA/BAIXO troca)", wfile, g_nwv);
    }

    float ex = g_car.mx[0] - g_car.mn[0], ez = g_car.mx[2] - g_car.mn[2], ey = g_car.mx[1] - g_car.mn[1];
    float cx = (g_car.mn[0] + g_car.mx[0]) * 0.5f, cz = (g_car.mn[2] + g_car.mx[2]) * 0.5f, by = g_car.mn[1];
    float carW = ex > ez ? ez : ex;
    g_carLen = ex > ez ? ex : ez;
    g_carYaw0 = (ex > ez) ? PI_F * 0.5f : 0.0f;

    for (int k = 0; k < g_nwv; k++) wheel_prepare(&g_wv[k], g_carLen, &g_wR[k], &g_wHW[k]);
    float lift = g_nwv ? 0.28f * g_wR[0] : 0.02f * g_carLen;
    for (int i = 0; i < g_car.nv; i++) { g_car.v[i].x -= cx; g_car.v[i].y += lift - by; g_car.v[i].z -= cz; }
    g_wx = carW * 0.5f - (g_nwv ? g_wHW[0] : 0.0f);
    if (g_wx < 0.2f * carW) g_wx = 0.2f * carW;
    g_wz = 0.29f * g_carLen;

    float trkx = g_track.mx[0] - g_track.mn[0], trkz = g_track.mx[2] - g_track.mn[2], trky = g_track.mx[1] - g_track.mn[1];
    g_trackExt = fmaxf(trkx, fmaxf(trkz, trky));

    free(g_grid.start); free(g_grid.list);
    memset(&g_grid, 0, sizeof g_grid);
    g_grid.ok = grid_build(&g_grid, &g_track);
    find_start(g_carLen * scale, &g_sx, &g_sy, &g_sz, g_how, sizeof g_how);

    diag("carro %.0fx%.0fx%.0f %dtris %dlotes %dc/tex", ex, ey, ez, g_car.nv / 3, g_car.nb, count_tex_batches(&g_car));
    diag(" sub=%d UV=%d T2D=%d I2D=%d Ext=%d", g_car.nsub, g_car.nuv, g_car.ntexobj, g_car.nimg, g_car.next);
    diag("pista %.0fx%.0fx%.0f %dtris %dlotes %dc/tex", trkx, trky, trkz, g_track.nv / 3, g_track.nb, count_tex_batches(&g_track));
    diag(" sub=%d UV=%d T2D=%d I2D=%d Ext=%d", g_track.nsub, g_track.nuv, g_track.ntexobj, g_track.nimg, g_track.next);
    diag("PNGs em assets: %d | texturas carregadas: %d | cfg: %d", g_npng, g_ntex, g_ncfg);
    for (int i = 0; i < g_nimginfo && i < 3; i++) diag(" %s", g_imginfo[i]);
    for (int i = 0; i < g_nmiss && i < 2; i++) diag(" png nao carregado: %.40s", g_miss[i]);
    diag("rodas: %s", g_wmsg);
    diag("chao %s | inicio via %s", g_grid.ok ? "ok" : "FALHOU", g_how);
    diag("inicio (%.1f, %.1f, %.1f)", g_sx, g_sy, g_sz);
    if (g_log) { fclose(g_log); g_log = NULL; }
    return NULL;
}

static int diag_screen(void) {
    pspDebugScreenInitEx((void *)0x44000000, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    pspDebugScreenPrintf("%s\nSELECT = escolher texturas    X = iniciar\n", g_diag);
    SceCtrlData p;
    sceCtrlPeekBufferPositive(&p, 1);
    for (int t = 0; t < 1800; t++) {
        sceCtrlPeekBufferPositive(&p, 1);
        if (p.Buttons & PSP_CTRL_CROSS) return 0;
        if (p.Buttons & PSP_CTRL_SELECT) return 1;
        sceKernelDelayThread(16667);
    }
    return 0;
}

static void fatal(const char *what, const char *err) {
    if (g_log) { fprintf(g_log, "ERRO (%s): %s\n", what, err); fclose(g_log); g_log = NULL; }
    pspDebugScreenInitEx((void *)0x44000000, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);
    pspDebugScreenPrintf("Erro ao carregar %s:\n%s\n\nVeja log.txt em:\n%s\n", what, err, BASE_DIR);
    sceKernelSleepThread();
}

/* ======================= Programa ======================= */
int main(void) {
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    cfg_load();
    float carScale = CAR_SCALE;
    int flip = 0;
    g_wsel = 0;
    settings_load(&carScale, &flip, &g_wsel);

    /* abre direto no jogo. A tela de diagnostico so aparece se as texturas da pista falharem;
       segurar SELECT ao abrir leva ao seletor de texturas. */
    for (;;) {
        const char *err = build_all(carScale);
        if (err) { fatal(g_what, err); return 0; }
        sceKernelDcacheWritebackAll();

        int select = 0;
        {
            SceCtrlData p;
            for (int t = 0; t < 45 && !select; t++) {
                sceCtrlPeekBufferPositive(&p, 1);
                if (p.Buttons & PSP_CTRL_SELECT) select = 1;
                sceKernelDelayThread(16667);
            }
        }
        if (select) { picker(); continue; }
        if (SHOW_DIAG_ALWAYS || count_tex_batches(&g_track) == 0) {
            if (diag_screen() == 1) { picker(); continue; }
        }
        break;
    }
    if (g_wsel < 0 || g_wsel >= g_nwv) g_wsel = 0;

    init_gu();

    SceCtrlData pad;
    unsigned int prev = 0;
    float heading = 0, camHead = 0, speed = 0, pitch = 0, roll = 0, steerVis = 0, wheelRot = 0;
    float px = g_sx, py = g_sy, pz = g_sz;
    const float dt = 1.0f / 60.0f;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;

        if (pad.Buttons & PSP_CTRL_LTRIGGER) carScale *= 0.985f;
        if (pad.Buttons & PSP_CTRL_RTRIGGER) carScale *= 1.015f;
        if (pressed & PSP_CTRL_TRIANGLE) flip ^= 1;
        if (g_nwv > 1) {
            if (pressed & PSP_CTRL_UP)   g_wsel = (g_wsel + 1) % g_nwv;
            if (pressed & PSP_CTRL_DOWN) g_wsel = (g_wsel + g_nwv - 1) % g_nwv;
        }
        if (pressed & PSP_CTRL_START) settings_save(carScale, flip, g_wsel);
        if (pressed & PSP_CTRL_SELECT) { px = g_sx; py = g_sy; pz = g_sz; speed = 0; heading += PI_F * 0.5f; camHead = heading; }

        float u = g_carLen * carScale;

        if (pad.Buttons & PSP_CTRL_CROSS) speed += 8.0f * u * dt;
        else if (pad.Buttons & PSP_CTRL_SQUARE) speed -= (speed > 0 ? 16.0f : 6.0f) * u * dt;
        else {
            float f = 3.0f * u * dt;
            if (speed > f) speed -= f; else if (speed < -f) speed += f; else speed = 0;
        }
        speed -= speed * 0.25f * dt;
        if (speed > 16.0f * u) speed = 16.0f * u;
        if (speed < -5.0f * u) speed = -5.0f * u;

        float steer = ((int)pad.Lx - 128) / 128.0f;
        if (fabsf(steer) < 0.15f) steer = 0;
        if (pad.Buttons & PSP_CTRL_LEFT)  steer = -1.0f;
        if (pad.Buttons & PSP_CTRL_RIGHT) steer = 1.0f;
        float grip = speed / (3.0f * u);
        if (grip > 1.0f) grip = 1.0f;
        if (grip < -1.0f) grip = -1.0f;
        heading += steer * 1.9f * grip * dt;
        steerVis += (steer - steerVis) * 0.2f;

        float fx = sinf(heading), fz = -cosf(heading);
        float rx = cosf(heading), rz = sinf(heading);
        px += fx * speed * dt;
        pz += fz * speed * dt;
        float wR = g_nwv ? g_wR[g_wsel] : 0.0f;
        if (g_nwv) wheelRot -= (speed * dt / carScale) / (wR > 0.001f ? wR : 1.0f);

        /* apoia nas 4 rodas: altura, inclinacao e nenhuma roda abaixo do chao */
        if (g_grid.ok) {
            float wxs = g_wx * carScale, wzs = g_wz * carScale;
            float gi[4], sum = 0;
            int ok[4], nok = 0;
            for (int k = 0; k < 4; k++) {
                float lx = (k & 1) ? wxs : -wxs, lz = (k & 2) ? wzs : -wzs;
                float x = px + rx * lx + fx * (-lz), z = pz + rz * lx + fz * (-lz);
                ok[k] = ground_at(x, z, py + 0.6f * u, 0, &gi[k]);
                if (ok[k]) { sum += gi[k]; nok++; }
            }
            if (nok > 0) {
                float avg = sum / nok;
                for (int k = 0; k < 4; k++) if (!ok[k]) gi[k] = avg;
                float gF = (gi[0] + gi[1]) * 0.5f, gB = (gi[2] + gi[3]) * 0.5f;
                float gRt = (gi[1] + gi[3]) * 0.5f, gL = (gi[0] + gi[2]) * 0.5f;
                float tp = atan2f(gF - gB, 2.0f * wzs), tr = atan2f(gRt - gL, 2.0f * wxs);
                float target = (gi[0] + gi[1] + gi[2] + gi[3]) * 0.25f;
                for (int k = 0; k < 4; k++) {
                    float zf = (k & 2) ? -wzs : wzs;
                    float xr = (k & 1) ? wxs : -wxs;
                    float pred = target + zf * tanf(tp) + xr * tanf(tr);
                    if (gi[k] > pred) target += gi[k] - pred;
                }
                if (target > py) py = target; else py += (target - py) * 0.35f;
                pitch += (tp - pitch) * 0.25f;
                roll  += (tr - roll) * 0.25f;
            } else py -= 8.0f * u * dt;
        }

        float dh = heading - camHead;
        while (dh > PI_F) dh -= 2.0f * PI_F;
        while (dh < -PI_F) dh += 2.0f * PI_F;
        camHead += dh * 0.08f;
        float cfx = sinf(camHead), cfz = -cosf(camHead);
        ScePspFVector3 eye    = { px - cfx * 2.4f * u, py + 0.95f * u, pz - cfz * 2.4f * u };
        ScePspFVector3 center = { px + cfx * 0.8f * u, py + 0.40f * u, pz + cfz * 0.8f * u };
        ScePspFVector3 up     = { 0.0f, 1.0f, 0.0f };
        float nearp = fmaxf(0.05f * u, 0.01f), farp = fmaxf(g_trackExt * 1.2f, 100.0f * u);

        sceGuStart(GU_DIRECT, list);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuEnable(GU_DEPTH_TEST);
        sceGuClearColor(0xFFE0B080);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

#if USE_LIGHTING
        {
            ScePspFVector3 ld = { 0.35f, 0.85f, 0.40f };      /* direcao PARA a luz (sol alto) */
            sceGuEnable(GU_LIGHTING);
            sceGuEnable(GU_LIGHT0);
            sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE);
            sceGuAmbient(0xFF606060);
            sceGuLightMode(GU_SINGLE_COLOR);
            sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &ld);
            sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFFFF);
        }
#endif

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(70.0f, 16.0f / 9.0f, nearp, farp);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumLookAt(&eye, &center, &up);
        sceGumMatrixMode(GU_MODEL);

        sceGumLoadIdentity();
        draw_model(&g_track);

        float ry = py + 0.004f * u;
        car_base(px, ry, pz, heading, pitch, roll, carScale);
        sceGumRotateY(g_carYaw0 + (flip ? PI_F : 0.0f));
        draw_model(&g_car);

        if (g_nwv) {
            for (int k = 0; k < 4; k++) {
                float sxw = (k & 1) ? g_wx : -g_wx;
                float szw = (k & 2) ? g_wz : -g_wz;
                ScePspFVector3 wp = { sxw, wR, szw };
                car_base(px, ry, pz, heading, pitch, roll, carScale);
                sceGumTranslate(&wp);
                if (k < 2) sceGumRotateY(-steerVis * 0.5f);
                sceGumRotateX(wheelRot);
                if (WHEEL_MIRROR && sxw < 0) sceGumRotateY(PI_F);
                draw_model(&g_wv[g_wsel]);
            }
        }

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    return 0;
}
