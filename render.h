/* render.h - LiteVis spectrum analyzer + software renderer. Pure C, no OS calls. */
#ifndef LV_RENDER_H
#define LV_RENDER_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define FFT_N   2048
#define MAXBARS 256

#define MAXSTOPS 5

typedef struct {
    int bars, heightPct, widthPct, offset, fps;
    int nstops;                      /* gradient: 2..5 color stops               */
    int spos[MAXSTOPS];              /* stop positions, 0..1000, ascending       */
    uint32_t scol[MAXSTOPS];         /* stop colors 0xRRGGBB                     */
    int dir;                         /* 0 = gradient by height, 1 = across bars  */
    int mirror, round, glow, peaks, useWall, preset;
    int order;                       /* 0 = left->right, 1 = right->left, 2 = centered */
    int focusPause;                  /* 1 = freeze while another window has focus */
    int appOnly;                     /* 1 = react only to the selected apps       */
    int attach;                      /* 0 auto, 1 Win11 under icons, 2 Win11 inside wallpaper layer, 3 Win10 style */
} Settings;

typedef struct { const wchar_t *name; int n; int pos[MAXSTOPS]; uint32_t col[MAXSTOPS]; } Preset;
static const Preset PRESETS[] = {
    {L"Custom", 2, {0, 1000},            {0x00E5FF, 0xFF2BD6}},
    {L"Cyber",  2, {0, 1000},            {0x00E5FF, 0xFF2BD6}},
    {L"Sunset", 3, {0, 500, 1000},       {0xFF3D6E, 0xFF8A3D, 0xFFC53D}},
    {L"Aurora", 3, {0, 500, 1000},       {0x00F5A0, 0x00C8FF, 0x7B5CFF}},
    {L"Ice",    2, {0, 1000},            {0x2E5BFF, 0x8FFBFF}},
    {L"Fire",   3, {0, 550, 1000},       {0xB00000, 0xFF5A00, 0xFFD000}},
    {L"Neon",   2, {0, 1000},            {0x39FF14, 0x00E5FF}},
    {L"Mono",   2, {0, 1000},            {0x4A4A4A, 0xFFFFFF}},
    {L"Sage",   4, {0, 350, 700, 1000},  {0x7C9A62, 0xD9E3C5, 0xF4F8EC, 0xFFFFFF}},
};
#define NPRESETS ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static void apply_preset(Settings *s, int i)
{
    if (i <= 0 || i >= NPRESETS) return;
    s->nstops = PRESETS[i].n;
    for (int k = 0; k < PRESETS[i].n; k++) { s->spos[k] = PRESETS[i].pos[k]; s->scol[k] = PRESETS[i].col[k]; }
}

/* keep 2..5 stops, ascending, at least 10/1000 apart */
static void fix_stops(Settings *s)
{
    int n = s->nstops < 2 ? 2 : (s->nstops > MAXSTOPS ? MAXSTOPS : s->nstops);
    s->nstops = n;
    for (int i = 1; i < n; i++)                       /* insertion sort */
        for (int j = i; j > 0 && s->spos[j] < s->spos[j - 1]; j--) {
            int tp = s->spos[j]; s->spos[j] = s->spos[j - 1]; s->spos[j - 1] = tp;
            uint32_t tc = s->scol[j]; s->scol[j] = s->scol[j - 1]; s->scol[j - 1] = tc;
        }
    int ok = 1;
    for (int i = 0; i < n; i++) if (s->spos[i] < 0 || s->spos[i] > 1000) ok = 0;
    for (int i = 1; i < n; i++) if (s->spos[i] < s->spos[i - 1] + 10) ok = 0;
    if (!ok) for (int i = 0; i < n; i++) s->spos[i] = i * 1000 / (n - 1);
}

static void settings_defaults(Settings *s)
{
    s->bars = 64; s->heightPct = 40; s->widthPct = 70; s->offset = 0; s->fps = 30;
    s->dir = 0; s->mirror = 0; s->round = 1; s->glow = 1; s->peaks = 1; s->useWall = 1; s->preset = 1;
    s->order = 0; s->focusPause = 1; s->appOnly = 0; s->attach = 0;
    apply_preset(s, 1);
}

/* ------------------------------------------------------------------ */
/* Analyzer                                                            */
/* ------------------------------------------------------------------ */
static float g_ring[FFT_N];
static int   g_ringPos = 0, g_srate = 48000;
static float g_lvl[MAXBARS], g_peak[MAXBARS], g_peakVel[MAXBARS], g_peakHold[MAXBARS];
static float g_refDb = -50.f;
static float tC[FFT_N / 2], tS[FFT_N / 2], tW[FFT_N];
static int   tInit = 0;

static void tables(void)
{
    if (tInit) return;
    for (int k = 0; k < FFT_N / 2; k++) {
        float a = 6.28318531f * k / FFT_N;
        tC[k] = cosf(a); tS[k] = sinf(a);
    }
    for (int i = 0; i < FFT_N; i++)
        tW[i] = 0.5f - 0.5f * cosf(6.28318531f * i / (FFT_N - 1));
    tInit = 1;
}

static void fft(float *re, float *im)
{
    int i, j = 0, k;
    for (i = 0; i < FFT_N - 1; i++) {
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
        k = FFT_N >> 1;
        while (k <= j) { j -= k; k >>= 1; }
        j += k;
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1, step = FFT_N / len;
        for (i = 0; i < FFT_N; i += len) {
            for (j = 0, k = 0; j < half; j++, k += step) {
                float wr = tC[k], wi = -tS[k];
                int a = i + j, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void analyze(const Settings *s, float dt)
{
    static float re[FFT_N], im[FFT_N], mag[FFT_N / 2 + 2];
    float raw[MAXBARS], tgt[MAXBARS], sm[MAXBARS];
    int n = s->order == 2 ? (s->bars + 1) / 2 : s->bars, i, b;
    tables();

    double e = 0;
    for (i = 0; i < FFT_N; i++) {
        float x = g_ring[(g_ringPos + i) & (FFT_N - 1)];
        e += (double)x * x;
        re[i] = x * tW[i]; im[i] = 0;
    }
    if (sqrt(e / FFT_N) < 3e-4) {                     /* silence */
        for (b = 0; b < n; b++) tgt[b] = 0;
    } else {
        fft(re, im);
        for (i = 0; i <= FFT_N / 2; i++)
            mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) * (4.0f / FFT_N);
        mag[FFT_N / 2 + 1] = mag[FFT_N / 2];

        float mx = -200.f, f0 = 40.f, ratio = powf(16000.f / 40.f, 1.f / n);
        for (b = 0; b < n; b++) {
            float f1 = f0 * ratio, fc = sqrtf(f0 * f1);
            float p0 = f0 * FFT_N / g_srate, p1 = f1 * FFT_N / g_srate, m;
            if (p1 - p0 < 1.f) {                      /* narrow band: interpolate */
                float pc = 0.5f * (p0 + p1);
                int k = (int)pc;
                if (k > FFT_N / 2 - 1) k = FFT_N / 2 - 1;
                float fr = pc - k;
                m = mag[k] * (1 - fr) + mag[k + 1] * fr;
            } else {                                   /* wide band: strongest bin */
                int lo = (int)ceilf(p0), hi = (int)floorf(p1);
                if (hi > FFT_N / 2) hi = FFT_N / 2;
                if (lo > hi) lo = hi;
                m = 0;
                for (int k = lo; k <= hi; k++) if (mag[k] > m) m = mag[k];
            }
            float db = 20.f * log10f(m + 1e-7f) + 3.f * log2f(fc / 250.f); /* treble tilt */
            raw[b] = db;
            if (db > mx) mx = db;
            f0 = f1;
        }
        g_refDb -= 5.f * dt;                           /* slow auto-gain */
        if (mx > g_refDb) g_refDb = mx;
        if (g_refDb < -50.f) g_refDb = -50.f;
        for (b = 0; b < n; b++)
            tgt[b] = powf(clamp01((raw[b] - (g_refDb - 45.f)) / 45.f), 1.5f);
    }
    for (b = 0; b < n; b++) {                          /* spatial smoothing */
        float l = tgt[b > 0 ? b - 1 : b], r = tgt[b < n - 1 ? b + 1 : b];
        sm[b] = 0.25f * l + 0.5f * tgt[b] + 0.25f * r;
    }
    float ka = 1.f - expf(-dt / 0.03f), kd = 1.f - expf(-dt / 0.14f);
    for (b = 0; b < n; b++) {
        float t = sm[b], c = g_lvl[b];
        c += (t - c) * (t > c ? ka : kd);
        g_lvl[b] = c;
        if (c >= g_peak[b]) { g_peak[b] = c; g_peakHold[b] = 0.35f; g_peakVel[b] = 0; }
        else if (g_peakHold[b] > 0) g_peakHold[b] -= dt;
        else {
            g_peakVel[b] += 2.5f * dt;
            g_peak[b] -= g_peakVel[b] * dt;
            if (g_peak[b] < c) g_peak[b] = c;
        }
    }
    for (b = n; b < MAXBARS; b++) { g_lvl[b] = 0; g_peak[b] = 0; }
}

/* ------------------------------------------------------------------ */
/* Renderer (32-bit 0x00RRGGBB buffers)                                */
/* ------------------------------------------------------------------ */
typedef struct {
    int W, H;
    uint32_t *frame;          /* W*H working strip (what gets blitted)       */
    const uint32_t *bg;       /* W*H background strip (wallpaper)            */
    uint32_t *rowRGB;         /* H entries, per-row gradient color           */
    int py0, py1;             /* rows drawn last frame (dirty range)         */
} Canvas;

static uint32_t g_lut[256];

static void rgb2hsv(uint32_t c, float *h, float *s, float *v)
{
    float r = ((c >> 16) & 255) / 255.f, g = ((c >> 8) & 255) / 255.f, b = (c & 255) / 255.f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx; *s = mx > 0 ? d / mx : 0; *h = 0;
    if (d > 0) {
        if (mx == r) *h = fmodf((g - b) / d + 6.f, 6.f);
        else if (mx == g) *h = (b - r) / d + 2.f;
        else *h = (r - g) / d + 4.f;
    }
}

static uint32_t hsv2rgb(float h, float s, float v)
{
    float c = v * s, x = c * (1.f - fabsf(fmodf(h, 2.f) - 1.f)), m = v - c, r, g, b;
    int i = (int)h % 6;
    switch (i) {
        case 0: r = c; g = x; b = 0; break;  case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;  case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;  default: r = c; g = 0; b = x; break;
    }
    return ((uint32_t)((r + m) * 255.f + .5f) << 16) | ((uint32_t)((g + m) * 255.f + .5f) << 8) |
           (uint32_t)((b + m) * 255.f + .5f);
}

/* hue-aware blend (keeps colors vivid); falls back to linear for grays */
static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t)
{
    float ha, sa, va, hb, sb, vb;
    rgb2hsv(a, &ha, &sa, &va); rgb2hsv(b, &hb, &sb, &vb);
    if (sa < 0.12f || sb < 0.12f) {
        int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
        int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
        return ((uint32_t)(ar + (br - ar) * t) << 16) | ((uint32_t)(ag + (bg - ag) * t) << 8) |
               (uint32_t)(ab + (bb - ab) * t);
    }
    float dh = hb - ha;
    if (dh > 3.f) dh -= 6.f;
    if (dh < -3.f) dh += 6.f;
    float h = ha + dh * t;
    if (h < 0) h += 6.f;
    if (h >= 6.f) h -= 6.f;
    return hsv2rgb(h, sa + (sb - sa) * t, va + (vb - va) * t);
}

/* color of the multi-stop gradient at t = 0..1 */
static uint32_t grad_at(const Settings *s, float t)
{
    float p = t * 1000.f;
    int n = s->nstops;
    if (p <= s->spos[0]) return s->scol[0];
    for (int i = 0; i < n - 1; i++)
        if (p <= s->spos[i + 1]) {
            float u = (p - s->spos[i]) / (float)(s->spos[i + 1] - s->spos[i]);
            return lerp_rgb(s->scol[i], s->scol[i + 1], u);
        }
    return s->scol[n - 1];
}

static void canvas_colors(const Settings *s, Canvas *c)
{
    for (int i = 0; i < 256; i++) g_lut[i] = grad_at(s, i / 255.f);
    for (int y = 0; y < c->H; y++) {
        float t = s->mirror ? fabsf(y + 0.5f - c->H * 0.5f) / (c->H * 0.5f)
                            : (c->H - 1 - y) / (float)(c->H > 1 ? c->H - 1 : 1);
        t = clamp01(t * 1.1f);
        c->rowRGB[y] = g_lut[(int)(t * 255)];
    }
    c->py0 = 0; c->py1 = c->H;         /* force full restore on next frame */
}

static inline void blend(uint32_t *px, uint32_t rgb, int a)   /* a: 0..256 */
{
    if (a <= 0) return;
    if (a > 256) a = 256;
    uint32_t d = *px;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    dr += ((sr - dr) * a) >> 8; dg += ((sg - dg) * a) >> 8; db += ((sb - db) * a) >> 8;
    *px = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

/* anti-aliased vertical bar; rounded = capsule */
static void bar_px(uint32_t *buf, int W, int H, float cx, float hw, float yt, float yb,
                   int round, uint32_t col, const uint32_t *rowRGB, int useRow, int alpha)
{
    float r = round ? hw : 0.f;
    if (round && (yb - yt) < 2 * r) r = (yb - yt) * 0.5f;
    int ya = (int)floorf(yt), yz = (int)ceilf(yb) - 1;
    if (ya < 0) ya = 0;
    if (yz > H - 1) yz = H - 1;
    float xl = cx - hw, xr = cx + hw;
    int xa = (int)floorf(xl), xz = (int)ceilf(xr) - 1;
    int xa2 = xa - 1, xz2 = xz + 1;
    if (xa < 0) xa = 0;
    if (xz > W - 1) xz = W - 1;
    if (xa2 < 0) xa2 = 0;
    if (xz2 > W - 1) xz2 = W - 1;

    for (int y = ya; y <= yz; y++) {
        float py = y + 0.5f;
        uint32_t cc = useRow ? rowRGB[y] : col;
        uint32_t *row = buf + (size_t)y * W;
        if (round && (py < yt + r || py > yb - r)) {
            float cyc = py < yt + r ? yt + r : yb - r, dy = py - cyc;
            for (int x = xa2; x <= xz2; x++) {
                float dx = x + 0.5f - cx;
                float cov = 0.5f - (sqrtf(dx * dx + dy * dy) - r);
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                blend(row + x, cc, (int)(cov * alpha));
            }
        } else {
            float vc = 1.f;
            if (!round) {
                float a = (py + 0.5f < yb ? py + 0.5f : yb) - (py - 0.5f > yt ? py - 0.5f : yt);
                vc = clamp01(a);
            }
            for (int x = xa; x <= xz; x++) {
                float a = (x + 1 < xr ? x + 1.f : xr) - (x > xl ? (float)x : xl);
                if (a > 1) a = 1;
                if (a <= 0) continue;
                blend(row + x, cc, (int)(a * vc * alpha));
            }
        }
    }
}

/* soft halo beside / above a bar (no sqrt: lookup tables only) */
static void glow_px(uint32_t *buf, int W, int H, float cx, float hw, float yt, float yb, int g,
                    int round, int botFade, uint32_t col, const uint32_t *rowRGB, int useRow, int ga)
{
    int aL[32], vL[32];
    for (int d = 0; d <= g; d++) {
        float f = 1.f - (d - 0.5f) / g; if (f < 0) f = 0;
        aL[d] = (int)(ga * f * f);
        float v = 1.f - d / (float)(g + 1);
        vL[d] = (int)(256 * v * v);
    }
    aL[0] = ga;
    int xl = (int)floorf(cx - hw), xr = (int)ceilf(cx + hw);
    int ya = (int)floorf(yt), yz = (int)ceilf(yb) - 1;
    int y0 = ya - g, y1 = botFade ? yz + g : yz;
    if (y0 < 0) y0 = 0;
    if (y1 > H - 1) y1 = H - 1;
    int cya = ya < 0 ? 0 : (ya > H - 1 ? H - 1 : ya);
    int cyz = yz > H - 1 ? H - 1 : (yz < 0 ? 0 : yz);
    for (int y = y0; y <= y1; y++) {
        int vm = 256, inBar = 1;
        if (round && (y < ya + (int)ceilf(hw) + 1 || (botFade && y > yz - (int)ceilf(hw) - 1))) inBar = 0;
        if (y < ya)      { int d = ya - y; vm = d > g ? 0 : vL[d]; inBar = 0; }
        else if (y > yz) { int d = y - yz; vm = d > g ? 0 : vL[d]; inBar = 0; }
        if (vm <= 0) continue;
        int cy = y < cya ? cya : (y > cyz ? cyz : y);
        uint32_t cc = useRow ? rowRGB[cy] : col;
        uint32_t *row = buf + (size_t)y * W;
        for (int x = xl - g; x < xr + g; x++) {
            if (x < 0 || x >= W) continue;
            int a;
            if (x < xl)      a = aL[xl - x];
            else if (x >= xr) a = aL[x - xr + 1];
            else { if (inBar) continue; a = ga; }
            blend(row + x, cc, (a * vm) >> 8);
        }
    }
}

static void render(const Settings *s, Canvas *c, int *oy0, int *oy1)
{
    int W = c->W, H = c->H, n = s->bars, b;
    float slot = (float)W / n;
#define LI(b) (s->order == 2 ? (int)fabsf((b) - (n - 1) * 0.5f) : (s->order == 1 ? (n - 1 - (b)) : (b)))
    float bw = slot * s->widthPct * 0.01f;
    if (bw < 2.f) bw = 2.f;
    if (bw > slot) bw = slot;
    float hw = bw * 0.5f;
    int mir = s->mirror;
    float base = mir ? H * 0.5f : (float)H;
    float maxExt = (mir ? H * 0.5f : (float)H) * 0.97f;
    float nub = 1.5f + hw * 0.5f;
    int g = s->glow ? (int)(slot * 0.8f) : 0;
    if (s->glow) { if (g < 3) g = 3; if (g > 14) g = 14; }
    float pkH = bw * 0.28f; if (pkH < 2.f) pkH = 2.f;
    float pkGap = bw * 0.30f; if (pkGap < 2.f) pkGap = 2.f;

    int active = 0;
    for (b = 0; b < n; b++) if (g_lvl[LI(b)] > 0.004f) { active = 1; break; }

    float ext[MAXBARS], pext[MAXBARS];
    float minY = (float)H;
    if (active) {
        for (b = 0; b < n; b++) {
            float e = g_lvl[LI(b)] * maxExt;
            if (e < nub) e = nub;
            float pe = s->peaks ? g_peak[LI(b)] * maxExt : 0.f;
            if (pe < e) pe = e;
            ext[b] = e; pext[b] = pe;
            float top = base - pe - (s->peaks ? pkGap + pkH : 0.f) - g - 2.f;
            if (top < minY) minY = top;
        }
    }
    int y0 = H, y1 = 0;
    if (active) {
        float maxY = mir ? 2.f * base - minY : (float)H;
        y0 = (int)floorf(minY); if (y0 < 0) y0 = 0;
        y1 = (int)ceilf(maxY);  if (y1 > H) y1 = H;
    }
    int U0 = y0 < c->py0 ? y0 : c->py0;
    int U1 = y1 > c->py1 ? y1 : c->py1;
    if (U1 <= U0) { *oy0 = *oy1 = 0; c->py0 = H; c->py1 = 0; return; }

    for (int y = U0; y < U1; y++)
        memcpy(c->frame + (size_t)y * W, c->bg + (size_t)y * W, (size_t)W * 4);

    if (active) {
        int useRow = (s->dir == 0);
        for (b = 0; b < n; b++) {
            float cx = (b + 0.5f) * slot;
            uint32_t flat = g_lut[n > 1 ? (b * 255) / (n - 1) : 0];
            float yt = base - ext[b];
            int floating = (s->offset > 0 && s->round && !mir);
            float yb = mir ? base + ext[b] : (floating ? (float)H - 0.5f : H + hw + 1.f);
            if (s->glow)
                glow_px(c->frame, W, H, cx, hw, yt, yb, g, s->round, mir || floating, flat, c->rowRGB, useRow, 70);
            bar_px(c->frame, W, H, cx, hw, yt, yb, s->round, flat, c->rowRGB, useRow, 242);
        }
        if (s->peaks) {
            for (b = 0; b < n; b++) {
                float cx = (b + 0.5f) * slot;
                uint32_t flat = g_lut[n > 1 ? (b * 255) / (n - 1) : 0];
                float pt = base - pext[b] - pkGap - pkH, pb = pt + pkH;
                int cy = (int)pt; if (cy < 0) cy = 0; if (cy > H - 1) cy = H - 1;
                uint32_t col = lerp_rgb(useRow ? c->rowRGB[cy] : flat, 0xFFFFFF, 0.5f);
                bar_px(c->frame, W, H, cx, hw, pt, pb, s->round, col, c->rowRGB, 0, 235);
                if (mir) {
                    float mt = base + pext[b] + pkGap;
                    bar_px(c->frame, W, H, cx, hw, mt, mt + pkH, s->round, col, c->rowRGB, 0, 235);
                }
            }
        }
    }
    c->py0 = y0; c->py1 = y1;
    *oy0 = U0; *oy1 = U1;
}

#undef LI
#endif
