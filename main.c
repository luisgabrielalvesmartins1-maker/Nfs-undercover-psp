#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <zlib.h>

PSP_MODULE_INFO("NFS M3G Viewer", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(16 * 1024);

#define MODEL_PATH "ms0:/PSP/GAME/nfs mobile/assets/car_viper.m3g"

#define BUF_WIDTH  512
#define SCR_WIDTH  480
#define SCR_HEIGHT 272

static unsigned int __attribute__((aligned(16))) list[262144];

/* ---------- Leitor binário (M3G é little-endian) ---------- */
typedef struct { const uint8_t *p, *e; int bad; } Rd;

static uint8_t rd8(Rd *r) {
    if (r->p >= r->e) { r->bad = 1; return 0; }
    return *r->p++;
}
static uint16_t rd16(Rd *r) { uint16_t a = rd8(r); a |= (uint16_t)rd8(r) << 8; return a; }
static uint32_t rd32(Rd *r) { uint32_t a = rd16(r); a |= (uint32_t)rd16(r) << 16; return a; }
static float rdf(Rd *r) { uint32_t u = rd32(r); float f; memcpy(&f, &u, 4); return f; }
static void rdskip(Rd *r, uint32_t n) {
    if ((uint32_t)(r->e - r->p) < n) { r->bad = 1; r->p = r->e; } else r->p += n;
}

/* ---------- Tabela de objetos (índice 1 = objeto de cabeçalho) ---------- */
typedef struct { uint8_t type; const uint8_t *data; uint32_t len; } Obj;
static Obj *g_obj;
static int g_nobj = 1, g_objcap;

typedef struct { int count, comps, size; int16_t *v; } VArr;
static VArr *g_va;

typedef struct { VArr *pos, *col; float bias[3], scale; } VBuf;

static int add_obj(uint8_t type, const uint8_t *data, uint32_t len) {
    if (g_nobj >= g_objcap) {
        int nc = g_objcap ? g_objcap * 2 : 256;
        Obj *n = realloc(g_obj, nc * sizeof(Obj));
        if (!n) return 0;
        g_obj = n; g_objcap = nc;
    }
    g_obj[g_nobj].type = type;
    g_obj[g_nobj].data = data;
    g_obj[g_nobj].len = len;
    g_nobj++;
    return 1;
}

static int inflate_buf(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, int wbits) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, wbits) != Z_OK) return 0;
    zs.next_in = (Bytef *)src;  zs.avail_in = slen;
    zs.next_out = dst;          zs.avail_out = dlen;
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

/* Lê o arquivo, valida o cabeçalho, descomprime as seções e cataloga os objetos.
   Os buffers ficam alocados até o fim do programa de propósito. */
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
        uint32_t total = rd32(&h);
        uint32_t unc = rd32(&h);
        if (total < 13 || pos + total > (size_t)sz) return "secao corrompida";

        const uint8_t *src = buf + pos + 9;
        uint32_t srclen = total - 13;
        const uint8_t *objs;
        uint32_t len;

        if (comp == 0) {
            objs = src;
            len = unc <= srclen ? unc : srclen;
        } else if (comp == 1) {
            uint8_t *dst = malloc(unc ? unc : 1);
            if (!dst) return "sem memoria para descomprimir";
            if (!inflate_buf(src, srclen, dst, unc, 15) &&
                !inflate_buf(src, srclen, dst, unc, -15)) return "falha ao descomprimir (zlib)";
            objs = dst;
            len = unc;
        } else {
            return "tipo de compressao desconhecido";
        }

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

/* ---------- Parsing dos objetos M3G ---------- */
static void parse_object3d(Rd *r) {
    rd32(r);                                    /* userID */
    uint32_t n = rd32(r);                       /* animation tracks */
    if (n > 0x10000) { r->bad = 1; return; }
    rdskip(r, n * 4);
    n = rd32(r);                                /* user parameters */
    while (n-- && !r->bad) { rd32(r); uint32_t l = rd32(r); rdskip(r, l); }
}

static void parse_vertex_array(int idx) {
    Rd r = { g_obj[idx].data, g_obj[idx].data + g_obj[idx].len, 0 };
    parse_object3d(&r);
    int size = rd8(&r), comps = rd8(&r), enc = rd8(&r);
    int count = rd16(&r);
    if (r.bad || (size != 1 && size != 2) || comps < 2 || comps > 4 || count == 0) return;

    int16_t *v = malloc((size_t)count * comps * sizeof(int16_t));
    if (!v) return;
    int prev[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < comps; c++) {
            int x = (size == 1) ? (int8_t)rd8(&r) : (int16_t)rd16(&r);
            if (enc == 1) {                     /* codificação delta */
                prev[c] += x;
                prev[c] = (size == 1) ? (int8_t)prev[c] : (int16_t)prev[c];
                x = prev[c];
            }
            v[i * comps + c] = (int16_t)x;
        }
    }
    if (r.bad) { free(v); return; }
    g_va[idx].count = count; g_va[idx].comps = comps; g_va[idx].size = size; g_va[idx].v = v;
}

static int parse_vbuf(uint32_t idx, VBuf *vb) {
    if (idx == 0 || idx >= (uint32_t)g_nobj || g_obj[idx].type != 21) return 0;
    Rd r = { g_obj[idx].data, g_obj[idx].data + g_obj[idx].len, 0 };
    parse_object3d(&r);
    rd32(&r);                                   /* cor padrão */
    uint32_t pi = rd32(&r);
    for (int i = 0; i < 3; i++) vb->bias[i] = rdf(&r);
    vb->scale = rdf(&r);
    rd32(&r);                                   /* normais (ignoradas) */
    uint32_t ci = rd32(&r);
    if (r.bad || pi == 0 || pi >= (uint32_t)g_nobj) return 0;
    vb->pos = &g_va[pi];
    if (!vb->pos->v || vb->pos->comps < 3) return 0;
    vb->col = (ci > 0 && ci < (uint32_t)g_nobj && g_va[ci].v && g_va[ci].comps >= 3) ? &g_va[ci] : NULL;
    return 1;
}

/* ---------- Matrizes 4x4 (linha-maior) ---------- */
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

/* Object3D + Transformable + Node; devolve a matriz local do nó */
static void parse_node(Rd *r, float *m) {
    parse_object3d(r);
    mat_ident(m);
    if (rd8(r)) {                               /* transformação por componentes */
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
            float a = ang * 3.14159265f / 180.0f;
            float c = cosf(a), s = sinf(a), k = 1.0f - c;
            R[0] = c + x * x * k;      R[1] = x * y * k - z * s;  R[2]  = x * z * k + y * s;
            R[4] = y * x * k + z * s;  R[5] = c + y * y * k;      R[6]  = y * z * k - x * s;
            R[8] = z * x * k - y * s;  R[9] = z * y * k + x * s;  R[10] = c + z * z * k;
        }
        mat_mul(m, T, R);
        mat_mul(m, m, S);
    }
    if (rd8(r)) {                               /* matriz geral */
        float G[16];
        for (int i = 0; i < 16; i++) G[i] = rdf(r);
        mat_mul(m, m, G);
    }
    rd8(r); rd8(r); rd8(r); rd32(r);            /* render, pick, alpha, scope */
    if (rd8(r)) { rd8(r); rd8(r); rd32(r); rd32(r); }   /* alinhamento */
}

/* ---------- Geometria final (lista de triângulos com cor) ---------- */
typedef struct { uint32_t color; float x, y, z; } Vertex;
static Vertex *g_verts;
static int g_nv, g_capv;

static int reserve(int n) {
    if (g_nv + n <= g_capv) return 1;
    int nc = g_capv ? g_capv * 2 : 4096;
    Vertex *nv = realloc(g_verts, (size_t)nc * sizeof(Vertex));
    if (!nv) return 0;
    g_verts = nv; g_capv = nc;
    return 1;
}

static const uint8_t PAL[6][3] = {
    {200, 40, 40}, {190, 190, 200}, {60, 60, 70}, {220, 170, 40}, {70, 130, 210}, {90, 180, 100}
};

static int col8(const VArr *a, int i, int c) {
    int v = a->v[i * a->comps + c];
    return a->size == 1 ? (v & 0xFF) : ((v >> 8) & 0xFF);
}

static void add_tri(const VBuf *vb, const float *world, const uint32_t id[3], int sub) {
    float P[3][3];
    for (int i = 0; i < 3; i++) {
        if (id[i] >= (uint32_t)vb->pos->count) return;
        const int16_t *v = vb->pos->v + id[i] * vb->pos->comps;
        float lp[3] = { v[0] * vb->scale + vb->bias[0],
                        v[1] * vb->scale + vb->bias[1],
                        v[2] * vb->scale + vb->bias[2] };
        mat_pt(world, lp, P[i]);
    }
    float e1[3], e2[3], n[3];
    for (int k = 0; k < 3; k++) { e1[k] = P[1][k] - P[0][k]; e2[k] = P[2][k] - P[0][k]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len < 1e-9f) return;

    /* luz fixa; usa |dot| para não depender do sentido dos triângulos */
    float d = fabsf(n[0] * 0.4f + n[1] * 0.8f + n[2] * 0.45f) / len;
    float shade = 0.35f + 0.65f * d;
    if (shade > 1.0f) shade = 1.0f;

    if (!reserve(3)) return;
    for (int i = 0; i < 3; i++) {
        int r, g, b;
        if (vb->col && id[i] < (uint32_t)vb->col->count) {
            r = col8(vb->col, id[i], 0); g = col8(vb->col, id[i], 1); b = col8(vb->col, id[i], 2);
        } else {
            r = PAL[sub % 6][0]; g = PAL[sub % 6][1]; b = PAL[sub % 6][2];
        }
        Vertex *o = &g_verts[g_nv++];
        o->color = 0xFF000000u | ((uint32_t)(b * shade) << 16) | ((uint32_t)(g * shade) << 8) | (uint32_t)(r * shade);
        o->x = P[i][0]; o->y = P[i][1]; o->z = P[i][2];
    }
}

static void emit_strips(const VBuf *vb, uint32_t ib, const float *world, int sub) {
    if (ib == 0 || ib >= (uint32_t)g_nobj || g_obj[ib].type != 11) return;
    Rd r = { g_obj[ib].data, g_obj[ib].data + g_obj[ib].len, 0 };
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
            add_tri(vb, world, id, sub);
        }
        pos += len;
    }
done:
    free(arr);
}

static int g_sub;

static void visit(int idx, const float *parent, int depth) {
    if (idx <= 0 || idx >= g_nobj || depth > 16) return;
    Obj *o = &g_obj[idx];
    int t = o->type;
    if (t != 9 && t != 22 && t != 14 && t != 15 && t != 16) return;

    Rd r = { o->data, o->data + o->len, 0 };
    float local[16], world[16];
    parse_node(&r, local);
    mat_mul(world, parent, local);

    if (t == 9 || t == 22) {                    /* Group / World */
        uint32_t n = rd32(&r);
        for (uint32_t k = 0; k < n && !r.bad; k++) visit((int)rd32(&r), world, depth + 1);
    } else {                                    /* Mesh / Morphing / Skinned */
        uint32_t vbi = rd32(&r), ns = rd32(&r);
        VBuf vb;
        if (r.bad || !parse_vbuf(vbi, &vb)) return;
        for (uint32_t k = 0; k < ns && !r.bad; k++) {
            uint32_t ib = rd32(&r);
            rd32(&r);                           /* appearance (ignorada) */
            emit_strips(&vb, ib, world, g_sub++);
        }
    }
}

static const char *load_model(const char *path) {
    const char *err = load_m3g(path);
    if (err) return err;

    g_va = calloc(g_nobj, sizeof(VArr));
    if (!g_va) return "sem memoria";
    for (int i = 1; i < g_nobj; i++)
        if (g_obj[i].type == 20) parse_vertex_array(i);

    /* descobre os nós raiz (que nenhum Group referencia) */
    uint8_t *ref = calloc(g_nobj, 1);
    if (!ref) return "sem memoria";
    for (int i = 1; i < g_nobj; i++) {
        if (g_obj[i].type != 9 && g_obj[i].type != 22) continue;
        Rd r = { g_obj[i].data, g_obj[i].data + g_obj[i].len, 0 };
        float m[16];
        parse_node(&r, m);
        uint32_t n = rd32(&r);
        for (uint32_t k = 0; k < n && !r.bad; k++) {
            uint32_t c = rd32(&r);
            if (c > 0 && c < (uint32_t)g_nobj) ref[c] = 1;
        }
    }
    float id[16];
    mat_ident(id);
    for (int i = 1; i < g_nobj; i++) {
        int t = g_obj[i].type;
        if ((t == 9 || t == 14 || t == 15 || t == 16 || t == 22) && !ref[i]) visit(i, id, 0);
    }
    free(ref);

    if (g_nv == 0) return "nenhuma malha/triangulo encontrado";

    /* centraliza e escala para caber numa caixa de tamanho 2 */
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < g_nv; i++) {
        float p[3] = { g_verts[i].x, g_verts[i].y, g_verts[i].z };
        for (int k = 0; k < 3; k++) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
    }
    float ext = 0;
    for (int k = 0; k < 3; k++) if (mx[k] - mn[k] > ext) ext = mx[k] - mn[k];
    float s = ext > 0 ? 2.0f / ext : 1.0f;
    float cx = (mn[0] + mx[0]) * 0.5f, cy = (mn[1] + mx[1]) * 0.5f, cz = (mn[2] + mx[2]) * 0.5f;
    for (int i = 0; i < g_nv; i++) {
        g_verts[i].x = (g_verts[i].x - cx) * s;
        g_verts[i].y = (g_verts[i].y - cy) * s;
        g_verts[i].z = (g_verts[i].z - cz) * s;
    }
    return NULL;
}

/* ---------- Callbacks do PSP (botão HOME) ---------- */
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

/* ---------- Gráficos ---------- */
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
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

int main(void) {
    setup_callbacks();

    const char *err = load_model(MODEL_PATH);
    if (err) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("Erro ao carregar o modelo:\n%s\n\n%s\n", err, MODEL_PATH);
        sceKernelSleepThread();
        return 0;
    }

    sceKernelDcacheWritebackAll();
    init_gu();

    float angle = 0.0f;
    while (1) {
        sceGuStart(GU_DIRECT, list);
        sceGuClearColor(0xFF201008);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(75.0f, 16.0f / 9.0f, 0.5f, 1000.0f);

        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();

        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        ScePspFVector3 pos = { 0.0f, 0.0f, -4.0f };
        sceGumTranslate(&pos);
        sceGumRotateX(0.35f);       /* inclina para ver de cima */
        sceGumRotateY(angle);

        for (int off = 0; off < g_nv; off += 60000) {
            int n = g_nv - off;
            if (n > 60000) n = 60000;
            sceGumDrawArray(GU_TRIANGLES,
                            GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                            n, 0, g_verts + off);
        }

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();

        angle += 0.02f;
    }
    return 0;
}
