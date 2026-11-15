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
PSP_HEAP_SIZE_KB(16 * 1024);

#define BASE_DIR   "ms0:/PSP/GAME/nfs mobile"
#define ASSET_DIR  BASE_DIR "/assets"
#define LOG_PATH   BASE_DIR "/log.txt"
#define CAR_FILE   "car_viper.m3g"
#define TRACK_FILE "causeway.m3g"
#define WHEEL_FILE ""        /* "" = procura sozinho em assets/ (nome com wheel/tire/tyre/rim) */
#define CAR_SCALE  1.0f      /* L/R ajustam em tempo real; depois fixe aqui */
#define WHEEL_MIRROR 1       /* 1 = gira 180 graus as rodas do lado esquerdo; troque se o aro ficar pra dentro */
#define FLIP_V     0         /* 1 = inverte o V das texturas (se vierem de cabeca para baixo) */

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
    int *timg;
    int ntexobj, fallback;
    char base[64];
} M3G;

typedef struct { VArr *pos, *col, *uv; float bias[3], scale, ubias[3], uscale; } VBuf;

typedef struct { float u, v; uint32_t color; float x, y, z; } Vertex;
typedef struct { int tex, wrap, start, count; } Batch;
typedef struct {
    Vertex *v; int nv, capv;
    Batch *b; int nb, capb;
    float mn[3], mx[3];
    int nsub, nuv, ntexobj, nimg, next;
} Model;

typedef struct { void *data; int w, h, alpha; char name[64]; } Tex;
#define MAX_TEX 128
static Tex g_tex[MAX_TEX];
static int g_ntex;
static char g_miss[6][48];
static int g_nmiss;

static M3G *M;
static Model *OUT;
static int g_sub, g_honor;
static Model g_car, g_track, g_wheel;

/* ======================= Texturas ======================= */
static int p2(int v, int cap) { int p = 8; while (p < v && p < cap) p <<= 1; return p; }

static int tex_create(const uint8_t *rgba, int w, int h, const char *name) {
    if (g_ntex >= MAX_TEX || w <= 0 || h <= 0) return -1;
    int pw = p2(w, 256), ph = p2(h, 256);
    uint32_t *d = memalign(16, (size_t)pw * ph * 4);
    if (!d) return -1;
    const uint32_t *s = (const uint32_t *)rgba;
    int alpha = 0;
    for (int y = 0; y < ph; y++) {
        int sy = (int)((long long)y * h / ph);
        for (int x = 0; x < pw; x++) {
            int sx = (int)((long long)x * w / pw);
            uint32_t px = s[sy * w + sx];
            if ((px >> 24) != 0xFF) alpha = 1;
            d[y * pw + x] = px;
        }
    }
    Tex *t = &g_tex[g_ntex];
    t->data = d; t->w = pw; t->h = ph; t->alpha = alpha;
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

static const char *BAD[] = { "wheel", "tire", "tyre", "glass", "shadow", "icon", "thumb", "menu", "logo", "light", "small" };

/* melhor .png de assets/ cujo nome contem 'base' (ou o trecho depois do 1o '_') */
static int find_png(const char *base, char *out, size_t osz) {
    char b[64], alt[64] = "";
    lower(b, base, sizeof b);
    const char *u = strchr(b, '_');
    if (u && u[1]) snprintf(alt, sizeof alt, "%s", u + 1);
    SceUID d = sceIoDopen(ASSET_DIR);
    if (d < 0) return 0;
    SceIoDirent e;
    int best = 0;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        char low[256];
        lower(low, e.d_name, sizeof low);
        size_t n = strlen(low);
        if (n <= 4 || strcmp(low + n - 4, ".png")) continue;
        int hb = strstr(low, b) != NULL;
        int ha = alt[0] && strstr(low, alt) != NULL;
        if (!hb && !ha) continue;
        char ex[80];
        snprintf(ex, sizeof ex, "%s.png", b);
        int s = 1;
        if (!strcmp(low, ex)) s += 100;
        if (hb) s += 50;
        if (ha) s += 30;
        for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
            if (strstr(low, BAD[i]) && !strstr(b, BAD[i])) s -= 40;
        s -= (int)n / 16;
        if (s > best) { best = s; snprintf(out, osz, "%s", e.d_name); }
    }
    sceIoDclose(d);
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

static int tex_from_image2d(const Obj *o, uint32_t idx) {
    Rd r = { o->data, o->data + o->len, 0 };
    rd32(&r);
    { uint32_t n = rd32(&r); if (n > 0x10000) return -1; rdskip(&r, n * 4);
      n = rd32(&r); while (n-- && !r.bad) { rd32(&r); uint32_t l = rd32(&r); rdskip(&r, l); } }
    int fmt = rd8(&r), mut = rd8(&r);
    uint32_t w = rd32(&r), h = rd32(&r);
    if (r.bad || mut || w == 0 || h == 0 || w > 1024 || h > 1024) return -1;
    int bpp = fmt == 96 ? 1 : fmt == 97 ? 1 : fmt == 98 ? 2 : fmt == 99 ? 3 : fmt == 100 ? 4 : 0;
    if (!bpp) return -1;
    uint32_t pl = rd32(&r);
    const uint8_t *pal = r.p;
    rdskip(&r, pl);
    uint32_t xl = rd32(&r);
    const uint8_t *pix = r.p;
    if (r.bad || (uint32_t)(r.e - r.p) < xl) return -1;
    uint32_t np = w * h;
    if (pl ? xl < np : xl < np * bpp) return -1;
    uint8_t *rgba = malloc((size_t)np * 4);
    if (!rgba) return -1;
    for (uint32_t i = 0; i < np; i++) {
        const uint8_t *c;
        if (pl) {
            uint32_t k = (uint32_t)pix[i] * bpp;
            if (k + bpp > pl) k = 0;
            c = pal + k;
        } else c = pix + (size_t)i * bpp;
        uint8_t R = 255, G = 255, B = 255, A = 255;
        switch (fmt) {
        case 96:  A = c[0]; break;
        case 97:  R = G = B = c[0]; break;
        case 98:  R = G = B = c[0]; A = c[1]; break;
        case 99:  R = c[0]; G = c[1]; B = c[2]; break;
        default:  R = c[0]; G = c[1]; B = c[2]; A = c[3]; break;
        }
        rgba[i * 4] = R; rgba[i * 4 + 1] = G; rgba[i * 4 + 2] = B; rgba[i * 4 + 3] = A;
    }
    char name[64];
    snprintf(name, sizeof name, "%s#img%u", M->base, (unsigned)idx);
    int id = tex_create(rgba, (int)w, (int)h, name);
    free(rgba);
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
    rd32(&r);
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

static int appearance_tex(uint32_t app, int *wrap, int hasuv) {
    *wrap = 1;
    if (!hasuv) return -1;
    if (app > 0 && app < (uint32_t)M->nobj && M->obj[app].type == 3) {
        Rd r = { M->obj[app].data, M->obj[app].data + M->obj[app].len, 0 };
        parse_object3d(&r);
        rd8(&r);
        for (int i = 0; i < 4; i++) rd32(&r);
        uint32_t nt = rd32(&r);
        if (!r.bad && nt > 0) {
            int tx = texture2d_tex(rd32(&r), wrap);
            return tx >= 0 ? tx : fallback_tex();   /* referencia nao resolvida -> tenta pelo nome */
        }
    }
    if (M->ntexobj == 0) return fallback_tex();
    return -1;
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
    int nc = OUT->capv ? OUT->capv * 2 : 4096;
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
    float d = fabsf(n[0] * 0.4f + n[1] * 0.8f + n[2] * 0.45f) / len;
    float shade = 0.6f + 0.4f * d;
    if (shade > 1.0f) shade = 1.0f;

    if (!reserve(3)) return;
    for (int i = 0; i < 3; i++) {
        int r, g, b;
        if (vb->col && id[i] < (uint32_t)vb->col->count) {
            r = col8(vb->col, id[i], 0); g = col8(vb->col, id[i], 1); b = col8(vb->col, id[i], 2);
        } else if (tex >= 0) { r = g = b = 255; }
        else { r = PAL[sub % 6][0]; g = PAL[sub % 6][1]; b = PAL[sub % 6][2]; }
        Vertex *o = &OUT->v[OUT->nv++];
        o->u = 0; o->v = 0;
        if (vb->uv && id[i] < (uint32_t)vb->uv->count) {
            const int16_t *t = vb->uv->v + id[i] * vb->uv->comps;
            o->u = t[0] * vb->uscale + vb->ubias[0];
            o->v = t[1] * vb->uscale + vb->ubias[1];
            if (FLIP_V) o->v = 1.0f - o->v;
        }
        o->color = 0xFF000000u | ((uint32_t)(b * shade) << 16) | ((uint32_t)(g * shade) << 8) | (uint32_t)(r * shade);
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
        for (uint32_t k = 0; k < n && !r.bad; k++) visit((int)rd32(&r), world, depth + 1);
    } else {
        uint32_t vbi = rd32(&r), ns = rd32(&r);
        VBuf vb;
        if (r.bad || !parse_vbuf(vbi, &vb)) { LOG("  malha obj=%d IGNORADA (vertex buffer invalido)\n", idx); return; }
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
                LOG("  malha obj=%d sub=%d tris=%d uv=%d tex=%d bbox[%.2f %.2f %.2f]-[%.2f %.2f %.2f]\n",
                    idx, g_sub, (OUT->nv - start) / 3, vb.uv != NULL, tex, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
            }
            g_sub++;
        }
    }
}

static const char *load_model(const char *file, Model *out, int honorHidden) {
    M3G m;
    memset(&m, 0, sizeof m);
    m.nobj = 1; m.fallback = -2;
    snprintf(m.base, sizeof m.base, "%s", file);
    char *dot = strrchr(m.base, '.'); if (dot) *dot = 0;
    memset(out, 0, sizeof *out);
    for (int k = 0; k < 3; k++) { out->mn[k] = 1e30f; out->mx[k] = -1e30f; }
    M = &m; OUT = out; g_sub = 0; g_honor = honorHidden;

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
    uint8_t *ref = calloc(m.nobj, 1);
    if (!m.va || !m.timg || !ref) return "sem memoria";
    for (int i = 0; i < m.nobj; i++) m.timg[i] = -2;
    for (int i = 1; i < m.nobj; i++) if (m.obj[i].type == 20) parse_vertex_array(i);

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
    if (out->nv == 0) return "nenhuma malha/triangulo encontrado";
    LOG("total: %d triangulos, %d lotes\n", out->nv / 3, out->nb);
    return NULL;
}

/* ======================= Rodas: acha o arquivo e prepara o modelo ======================= */
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

/* centra a roda, alinha o eixo com X, ajusta a escala se estiver absurda; devolve raio e meia-largura */
static void wheel_prepare(float carLen, float *R, float *halfW) {
    Model *w = &g_wheel;
    float ext[3] = { w->mx[0] - w->mn[0], w->mx[1] - w->mn[1], w->mx[2] - w->mn[2] };
    float c[3] = { (w->mn[0] + w->mx[0]) * 0.5f, (w->mn[1] + w->mx[1]) * 0.5f, (w->mn[2] + w->mx[2]) * 0.5f };
    int ax = 0;
    if (ext[1] < ext[ax]) ax = 1;
    if (ext[2] < ext[ax]) ax = 2;                    /* eixo da roda = dimensao mais fina */
    float diam = fmaxf(ext[(ax + 1) % 3], ext[(ax + 2) % 3]);
    float ratio = diam / carLen;
    float s = (ratio < 0.08f || ratio > 0.30f) ? (0.16f * carLen / diam) : 1.0f;
    for (int i = 0; i < w->nv; i++) {
        float x = w->v[i].x - c[0], y = w->v[i].y - c[1], z = w->v[i].z - c[2], t;
        if (ax == 2)      { t = z; z = -x; x = t; }  /* eixo Z -> X */
        else if (ax == 1) { t = -y; y = x; x = t; }  /* eixo Y -> X */
        w->v[i].x = x * s; w->v[i].y = y * s; w->v[i].z = z * s;
    }
    *R = diam * s * 0.5f;
    *halfW = ext[ax] * s * 0.5f;
    LOG("roda: eixo=%d diametro=%.2f largura=%.2f escala aplicada=%.3f\n", ax, diam, ext[ax], s);
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
            if (bt->tex >= 0 && g_tex[bt->tex].alpha) continue;       /* folhas/grades nao sao chao */
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

/* inicio: 1) triangulo com textura de "estrada" mais perto do centro; 2) espiral, superficie mais ALTA */
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
        snprintf(how, hsz, "textura '%.30s'", bn);
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
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);
    sceGuTexScale(1.0f, 1.0f);       /* sem isso a escala de UV pode ficar 0 e a textura vira cor solida */
    sceGuTexOffset(0.0f, 0.0f);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void draw_model(const Model *md) {
    const int vt = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D;
    for (int i = 0; i < md->nb; i++) {
        const Batch *b = &md->b[i];
        if (b->tex >= 0) {
            const Tex *t = &g_tex[b->tex];
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(GU_PSM_8888, 0, 0, 0);
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

static int count_tex_batches(const Model *m) {
    int n = 0;
    for (int i = 0; i < m->nb; i++) if (m->b[i].tex >= 0) n++;
    return n;
}

static void fatal(const char *what, const char *err) {
    LOG("ERRO (%s): %s\n", what, err);
    if (g_log) { fclose(g_log); g_log = NULL; }
    pspDebugScreenInit();
    pspDebugScreenPrintf("Erro ao carregar %s:\n%s\n\nVeja log.txt em:\n%s\n", what, err, BASE_DIR);
    sceKernelSleepThread();
}

/* ======================= Programa ======================= */
int main(void) {
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    g_log = fopen(LOG_PATH, "w");

    const char *err = load_model(CAR_FILE, &g_car, 0);
    if (err) { fatal(CAR_FILE, err); return 0; }
    err = load_model(TRACK_FILE, &g_track, 1);
    if (err) { fatal(TRACK_FILE, err); return 0; }

    /* rodas: arquivo separado (se existir) */
    char stem[64], wfile[256] = "";
    snprintf(stem, sizeof stem, "%s", CAR_FILE);
    { char *d = strrchr(stem, '.'); if (d) *d = 0; }
    if (WHEEL_FILE[0]) snprintf(wfile, sizeof wfile, "%s", WHEEL_FILE);
    else find_wheel_file(stem, wfile, sizeof wfile);
    int haveWheel = 0;
    char wmsg[96] = "nenhum arquivo de roda em assets/";
    if (wfile[0]) {
        const char *we = load_model(wfile, &g_wheel, 0);
        if (we) snprintf(wmsg, sizeof wmsg, "%.30s: %.40s", wfile, we);
        else { haveWheel = 1; snprintf(wmsg, sizeof wmsg, "%.40s (%d tris)", wfile, g_wheel.nv / 3); }
    }

    /* carro: origem no centro da base */
    float ex = g_car.mx[0] - g_car.mn[0], ez = g_car.mx[2] - g_car.mn[2], ey = g_car.mx[1] - g_car.mn[1];
    float cx = (g_car.mn[0] + g_car.mx[0]) * 0.5f, cz = (g_car.mn[2] + g_car.mx[2]) * 0.5f, by = g_car.mn[1];
    float carLen = ex > ez ? ex : ez, carW = ex > ez ? ez : ex;
    float carYaw0 = (ex > ez) ? PI_F * 0.5f : 0.0f;

    float wheelR = 0, wheelHW = 0;
    if (haveWheel) wheel_prepare(carLen, &wheelR, &wheelHW);
    float lift = haveWheel ? 0.28f * wheelR : 0.02f * carLen;     /* folga entre a base do carro e o chao */
    for (int i = 0; i < g_car.nv; i++) { g_car.v[i].x -= cx; g_car.v[i].y += lift - by; g_car.v[i].z -= cz; }

    float wx = carW * 0.5f - wheelHW;
    if (wx < 0.2f * carW) wx = 0.2f * carW;
    float wz = 0.29f * carLen;

    float trkx = g_track.mx[0] - g_track.mn[0], trkz = g_track.mx[2] - g_track.mn[2], trky = g_track.mx[1] - g_track.mn[1];
    float trackExt = fmaxf(trkx, fmaxf(trkz, trky));

    g_grid.ok = grid_build(&g_grid, &g_track);
    float sx, sy, sz;
    char how[64];
    find_start(carLen * CAR_SCALE, &sx, &sy, &sz, how, sizeof how);

    /* tela de diagnostico */
    diag("carro %.1fx%.1fx%.1f: %d tris, %d lotes (%d c/ textura)", ex, ey, ez, g_car.nv / 3, g_car.nb, count_tex_batches(&g_car));
    diag("  submalhas=%d c/UV=%d | Texture2D=%d Image2D=%d ExtRef=%d", g_car.nsub, g_car.nuv, g_car.ntexobj, g_car.nimg, g_car.next);
    diag("pista %.0fx%.0fx%.0f: %d tris, %d lotes (%d c/ textura)", trkx, trky, trkz, g_track.nv / 3, g_track.nb, count_tex_batches(&g_track));
    diag("  submalhas=%d c/UV=%d | Texture2D=%d Image2D=%d ExtRef=%d", g_track.nsub, g_track.nuv, g_track.ntexobj, g_track.nimg, g_track.next);
    diag("texturas carregadas: %d", g_ntex);
    for (int i = 0; i < g_nmiss && i < 4; i++) diag("  png nao carregado: %.44s", g_miss[i]);
    diag("rodas: %s", wmsg);
    diag("chao: %s | inicio via %s", g_grid.ok ? "ok" : "FALHOU", how);
    diag("inicio (%.1f, %.1f, %.1f)", sx, sy, sz);
    if (g_log) { fclose(g_log); g_log = NULL; }

    sceKernelDcacheWritebackAll();

    pspDebugScreenInit();
    pspDebugScreenPrintf("%s\nDetalhes em %s/log.txt\n\nPressione X para iniciar...\n", g_diag, BASE_DIR);
    {
        SceCtrlData p;
        for (int t = 0; t < 900; t++) {
            sceCtrlPeekBufferPositive(&p, 1);
            if (p.Buttons & PSP_CTRL_CROSS) break;
            sceKernelDelayThread(16667);
        }
    }

    init_gu();

    SceCtrlData pad;
    unsigned int prev = 0;
    float carScale = CAR_SCALE, heading = 0, camHead = 0, speed = 0, pitch = 0, roll = 0;
    float steerVis = 0, wheelRot = 0;
    float px = sx, py = sy, pz = sz;
    int flip = 0;
    const float dt = 1.0f / 60.0f;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;

        if (pad.Buttons & PSP_CTRL_LTRIGGER) carScale *= 0.99f;
        if (pad.Buttons & PSP_CTRL_RTRIGGER) carScale *= 1.01f;
        if (pressed & PSP_CTRL_TRIANGLE) flip ^= 1;
        if (pressed & PSP_CTRL_SELECT) { px = sx; py = sy; pz = sz; speed = 0; heading += PI_F * 0.5f; camHead = heading; }

        float u = carLen * carScale;

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
        if (haveWheel) wheelRot -= (speed * dt / carScale) / (wheelR > 0.001f ? wheelR : 1.0f);

        /* acompanha altura e inclinacao do chao */
        if (g_grid.ok) {
            float gy;
            if (ground_at(px, pz, py + 0.6f * u, 0, &gy)) py += (gy - py) * 0.4f;
            else py -= 8.0f * u * dt;
            float d = 0.35f * u, yf, yr, yR, yL;
            float tp = 0, tr = 0;
            if (ground_at(px + fx * d, pz + fz * d, py + 0.6f * u, 0, &yf) &&
                ground_at(px - fx * d, pz - fz * d, py + 0.6f * u, 0, &yr)) tp = atan2f(yf - yr, 2.0f * d);
            float d2 = 0.2f * u;
            if (ground_at(px + rx * d2, pz + rz * d2, py + 0.6f * u, 0, &yR) &&
                ground_at(px - rx * d2, pz - rz * d2, py + 0.6f * u, 0, &yL)) tr = atan2f(yR - yL, 2.0f * d2);
            pitch += (tp - pitch) * 0.2f;
            roll  += (tr - roll) * 0.2f;
        }

        float dh = heading - camHead;
        while (dh > PI_F) dh -= 2.0f * PI_F;
        while (dh < -PI_F) dh += 2.0f * PI_F;
        camHead += dh * 0.08f;
        float cfx = sinf(camHead), cfz = -cosf(camHead);
        ScePspFVector3 eye    = { px - cfx * 2.4f * u, py + 0.95f * u, pz - cfz * 2.4f * u };
        ScePspFVector3 center = { px + cfx * 0.8f * u, py + 0.40f * u, pz + cfz * 0.8f * u };
        ScePspFVector3 up     = { 0.0f, 1.0f, 0.0f };
        float nearp = fmaxf(0.05f * u, 0.01f), farp = fmaxf(trackExt * 1.2f, 100.0f * u);

        sceGuStart(GU_DIRECT, list);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuClearColor(0xFFE0B080);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(70.0f, 16.0f / 9.0f, nearp, farp);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumLookAt(&eye, &center, &up);
        sceGumMatrixMode(GU_MODEL);

        sceGumLoadIdentity();
        draw_model(&g_track);

        float ry = py + 0.01f * u;                 /* pequena folga para nao brigar com o asfalto */
        car_base(px, ry, pz, heading, pitch, roll, carScale);
        sceGumRotateY(carYaw0 + (flip ? PI_F : 0.0f));
        draw_model(&g_car);

        if (haveWheel) {
            for (int k = 0; k < 4; k++) {
                float sxw = (k & 1) ? wx : -wx;
                float szw = (k & 2) ? wz : -wz;       /* k<2: frente (-Z) */
                ScePspFVector3 wp = { sxw, wheelR, szw };
                car_base(px, ry, pz, heading, pitch, roll, carScale);
                sceGumTranslate(&wp);
                if (k < 2) sceGumRotateY(-steerVis * 0.5f);
                sceGumRotateX(wheelRot);
                if (WHEEL_MIRROR && sxw < 0) sceGumRotateY(PI_F);
                draw_model(&g_wheel);
            }
        }

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    return 0;
}
