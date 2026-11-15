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

/* Decodificador PVRTC 4bpp (PowerVR iPhone) -> RGBA8888 */
static uint32_t pvrtc_morton(uint32_t x, uint32_t y) {
    uint32_t z = 0;
    for (uint32_t i = 0; i < 16; i++) {
        z |= ((x & (1u << i)) << i) | ((y & (1u << i)) << (i + 1));
    }
    return z;
}

static void pvrtc_unpack_colors(uint32_t raw, int cA[4], int cB[4], int has_alpha) {
    if (raw & 0x8000u) {
        cA[0] = (raw >> 10) & 0x1F; cA[1] = (raw >> 5) & 0x1F; cA[2] = (raw & 0x1F);
        cA[0] = (cA[0] << 3) | (cA[0] >> 2);
        cA[1] = (cA[1] << 3) | (cA[1] >> 2);
        cA[2] = (cA[2] << 3) | (cA[2] >> 2);
        cA[3] = 255;
    } else {
        cA[3] = has_alpha ? (((raw >> 12) & 0x7) * 255 / 7) : 255;
        cA[0] = ((raw >> 8) & 0xF) * 17;
        cA[1] = ((raw >> 4) & 0xF) * 17;
        cA[2] = (raw & 0xF) * 17;
    }
    uint32_t hi = raw >> 16;
    if (hi & 0x8000u) {
        cB[0] = (hi >> 10) & 0x1F; cB[1] = (hi >> 5) & 0x1F; cB[2] = ((hi >> 1) & 0xF) << 1;
        cB[0] = (cB[0] << 3) | (cB[0] >> 2);
        cB[1] = (cB[1] << 3) | (cB[1] >> 2);
        cB[2] = (cB[2] << 3) | (cB[2] >> 2);
        cB[3] = 255;
    } else {
        cB[3] = has_alpha ? (((hi >> 12) & 0x7) * 255 / 7) : 255;
        cB[0] = ((hi >> 8) & 0xF) * 17;
        cB[1] = ((hi >> 4) & 0xF) * 17;
        cB[2] = (hi & 0xF) * 17;
    }
}

static void pvrtc4_decode(const uint8_t *src, uint32_t w, uint32_t h, int has_alpha, uint8_t *out) {
    uint32_t bw = w >> 2, bh = h >> 2;
    if (bw == 0) bw = 1;
    if (bh == 0) bh = 1;
    uint32_t mx = bw - 1, my = bh - 1;
    static const int W_NORM[4] = { 0, 3, 5, 8 };
    static const int W_PUNCH[4] = { 0, 4, 4, 8 };

    for (uint32_t by = 0; by < bh; by++) {
        uint32_t y0 = (by - 1) & my, y1 = by & my;
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint32_t x0 = (bx - 1) & mx, x1 = bx & mx;
            uint32_t idx[4] = {
                pvrtc_morton(x0, y0), pvrtc_morton(x1, y0),
                pvrtc_morton(x0, y1), pvrtc_morton(x1, y1)
            };
            int cA[4][4], cB[4][4];
            for (int k = 0; k < 4; k++) {
                const uint8_t *blk = src + idx[k] * 8;
                uint32_t col_raw = blk[4] | (blk[5] << 8) | (blk[6] << 16) | (blk[7] << 24);
                pvrtc_unpack_colors(col_raw, cA[k], cB[k], has_alpha);
            }
            const uint8_t *cblk = src + idx[3] * 8;
            uint32_t mod = cblk[0] | (cblk[1] << 8) | (cblk[2] << 16) | (cblk[3] << 24);
            uint32_t col_c = cblk[4] | (cblk[5] << 8) | (cblk[6] << 16) | (cblk[7] << 24);
            int punch = (col_c & 1u) != 0;

            for (uint32_t py = 0; py < 4; py++) {
                uint32_t y = by * 4 + py;
                if (y >= h) continue;
                int wy = (py < 2) ? (py + 2) : (py - 2);
                for (uint32_t px = 0; px < 4; px++) {
                    uint32_t x = bx * 4 + px;
                    if (x >= w) continue;
                    int wx = (px < 2) ? (px + 2) : (px - 2);
                    int w00 = (4 - wx) * (4 - wy), w10 = wx * (4 - wy);
                    int w01 = (4 - wx) * wy,       w11 = wx * wy;

                    uint32_t m = (mod >> ((py * 4 + px) * 2)) & 3u;
                    int factor = punch ? W_PUNCH[m] : W_NORM[m];
                    int inv = 8 - factor;

                    uint8_t *dst = out + (y * w + x) * 4;
                    for (int ch = 0; ch < 4; ch++) {
                        int a = (w00 * cA[0][ch] + w10 * cA[1][ch] + w01 * cA[2][ch] + w11 * cA[3][ch]) >> 4;
                        int b = (w00 * cB[0][ch] + w10 * cB[1][ch] + w01 * cB[2][ch] + w11 * cB[3][ch]) >> 4;
                        int val = (a * inv + b * factor) >> 3;
                        if (val < 0) val = 0; else if (val > 255) val = 255;
                        dst[ch] = (uint8_t)val;
                    }
                    if (punch && m == 2) dst[3] = 0;
                }
            }
        }
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
    if ((fmt == 124 || fmt == 125) && xl >= (np / 2)) {
        pvrtc4_decode(pix, w, h, fmt == 125, out);
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
    if
