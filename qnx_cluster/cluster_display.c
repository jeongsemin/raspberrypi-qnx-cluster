/* Digital instrument cluster for QNX Screen (premium-sedan inspired look).
 *
 *  - Left dial: speed.  Right dial: engine RPM and current gear.
 *  - Centre panel: clock, drive mode, trip/range, fuel and coolant, gear selector.
 *  - Three colour themes, one per drive mode (ECO / COMFORT / SPORT).
 *  - No UI toolkit: everything is drawn into the Screen window buffer with
 *    anti-aliased primitives and a built-in thin stroke font.
 *
 * A receiver thread takes vehicle data from speed_source over QNX message
 * passing; the main thread renders at ~30 fps and never blocks on the sender.
 * Static parts (background, scales, labels) are pre-rendered once per theme.
 *
 * Usage: cluster_display [seconds]   (default 120) */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <screen/screen.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cluster_e2e.h"
#include "cluster_msg.h"

#define MAX_SPEED 240.0
#define MAX_RPM 8000.0
#define REDLINE_RPM 6500.0
#define START_DEG 135.0
#define SWEEP_DEG 270.0
#define STALE_SEC 0.5      /* no valid message for this long: show the safe state */
#define ERR_HOLD_SEC 2.0   /* a rejected message keeps the "data error" notice up this long */
#define PI 3.14159265358979323846

/* ===================================================================== */
/* shared state between receiver thread and renderer                      */
/* ===================================================================== */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vehicle_msg_t g_msg;   /* last message that passed every check in e2e_check() */
static double g_last_valid;   /* when it was accepted */
static double g_last_err;     /* when a message was last rejected (0 = never) */
static int g_have_valid;
static e2e_state_t g_e2e;     /* error counters, written by the receiver thread */

typedef enum {
    DATA_VALID,     /* fresh and no recent errors */
    DATA_DEGRADED,  /* fresh, but messages were rejected in the last ERR_HOLD_SEC */
    DATA_INVALID    /* nothing valid for STALE_SEC: the display must not show values */
} data_status_t;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef union {
    uint16_t type;
    vehicle_msg_t vehicle;
    struct _pulse pulse;
} recv_t;

static void *receiver(void *arg)
{
    (void)arg;
    name_attach_t *att = name_attach(NULL, CLUSTER_NAME, 0);
    if (!att) {
        perror("name_attach");
        exit(1);
    }
    printf("listening on /dev/name/local/%s\n", CLUSTER_NAME);
    fflush(stdout);

    for (;;) {
        recv_t m;
        struct _msg_info info;
        int rcvid = MsgReceive(att->chid, &m, sizeof m, &info);
        if (rcvid == -1) continue;
        if (rcvid == 0) { /* pulse */
            if (m.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(m.pulse.scoid);
            continue;
        }
        if (m.type != MSG_VEHICLE) {
            MsgError(rcvid, ENOSYS);
            continue;
        }
        int accepted = 0;
        pthread_mutex_lock(&g_lock);
        double t_rx = mono_now();
        /* the link was down long enough for the safe state: start the checks afresh */
        if (g_have_valid && t_rx - g_last_valid > STALE_SEC) e2e_reset_baseline(&g_e2e);
        if (info.msglen < (int)sizeof(vehicle_msg_t)) {
            g_e2e.n_crc++;              /* truncated message: treated like a corrupted one */
        } else {
            accepted = e2e_accepted(e2e_check(&g_e2e, &m.vehicle));
            if (accepted) {
                g_msg = m.vehicle;
                g_last_valid = t_rx;
                g_have_valid = 1;
            }
        }
        if (!accepted) g_last_err = t_rx;
        pthread_mutex_unlock(&g_lock);
        if (accepted) MsgReply(rcvid, EOK, NULL, 0);
        else MsgError(rcvid, EBADMSG);  /* the sender sees the rejection */
    }
    return NULL;
}

/* ===================================================================== */
/* drawing primitives (anti-aliased)                                      */
/* ===================================================================== */
typedef struct {
    uint8_t *px;
    int stride, w, h;
} canvas_t;

typedef struct { int r, g, b; } rgb_t;

static rgb_t C(int r, int g, int b)
{
    rgb_t c = {r, g, b};
    return c;
}

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* blend colour `col` over the pixel with opacity a (0..255) */
static void blend_px(canvas_t *c, int x, int y, rgb_t col, int a)
{
    if (a <= 0 || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    uint32_t *p = (uint32_t *)(c->px + (size_t)y * c->stride) + x;
    if (a >= 255) {
        *p = 0xFF000000u | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | (uint32_t)col.b;
        return;
    }
    uint32_t d = *p;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int r = dr + ((col.r - dr) * a) / 255;
    int g = dg + ((col.g - dg) * a) / 255;
    int b = db + ((col.b - db) * a) / 255;
    *p = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void disc_aa(canvas_t *c, double cx, double cy, double r, rgb_t col, int a)
{
    for (int y = (int)floor(cy - r - 1); y <= (int)ceil(cy + r + 1); y++)
        for (int x = (int)floor(cx - r - 1); x <= (int)ceil(cx + r + 1); x++) {
            double d = hypot(x - cx, y - cy);
            blend_px(c, x, y, col, (int)(a * clamp01(r + 0.5 - d)));
        }
}

/* capsule (line with round caps) of half-width `half` */
static void seg_aa(canvas_t *c, double x0, double y0, double x1, double y1, double half, rgb_t col, int a)
{
    double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    int xa = (int)floor(fmin(x0, x1) - half - 1), xb = (int)ceil(fmax(x0, x1) + half + 1);
    int ya = (int)floor(fmin(y0, y1) - half - 1), yb = (int)ceil(fmax(y0, y1) + half + 1);
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            double t = len2 > 0 ? clamp01(((x - x0) * dx + (y - y0) * dy) / len2) : 0;
            double d = hypot(x - (x0 + t * dx), y - (y0 + t * dy));
            blend_px(c, x, y, col, (int)(a * clamp01(half + 0.5 - d)));
        }
}

/* rounded rectangle: stroke <= 0 fills it, otherwise draws an inside outline */
static void rrect_aa(canvas_t *c, double x, double y, double w, double h, double r,
                     rgb_t col, int a, double stroke)
{
    double hx = w / 2, hy = h / 2, cx = x + hx, cy = y + hy;
    for (int j = (int)floor(y - 1); j <= (int)ceil(y + h + 1); j++)
        for (int i = (int)floor(x - 1); i <= (int)ceil(x + w + 1); i++) {
            double qx = fabs(i - cx) - (hx - r), qy = fabs(j - cy) - (hy - r);
            double d = hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - r;
            double cov = stroke <= 0 ? clamp01(0.5 - d)
                                     : clamp01(stroke / 2 + 0.5 - fabs(d + stroke / 2));
            blend_px(c, i, j, col, (int)(a * cov));
        }
}

static void tri_fill(canvas_t *c, double x0, double y0, double x1, double y1, double x2, double y2,
                     rgb_t col, int a)
{
    int minx = (int)floor(fmin(x0, fmin(x1, x2))), maxx = (int)ceil(fmax(x0, fmax(x1, x2)));
    int miny = (int)floor(fmin(y0, fmin(y1, y2))), maxy = (int)ceil(fmax(y0, fmax(y1, y2)));
    double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (den == 0) return;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            double l0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / den;
            double l1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / den;
            double l2 = 1 - l0 - l1;
            double m = fmin(l0, fmin(l1, l2)); /* soft edge from the smallest weight */
            blend_px(c, x, y, col, (int)(a * clamp01(m * 6.0 + 0.5)));
        }
}

/* ===================================================================== */
/* thin stroke font: 6 x 10 grid, y grows downwards, 'a' = 10             */
/* ===================================================================== */
typedef struct { char ch; const char *path; } glyph_t;

static const glyph_t GLYPHS[] = {
    {'0', "10 50 61 69 5a 1a 09 01 10"},
    {'1', "12 30 3a"},
    {'2', "01 10 50 61 64 0a 6a"},
    {'3', "01 10 50 61 64 55 25;55 66 69 5a 1a 09"},
    {'4', "5a 50 07 67"},
    {'5', "60 00 05 55 66 69 5a 1a 09"},
    {'6', "50 10 01 09 1a 5a 69 66 55 05"},
    {'7', "00 60 2a"},
    {'8', "10 50 61 64 55 15 04 01 10;15 55 66 69 5a 1a 09 06 15"},
    {'9', "65 15 04 01 10 50 61 69 5a 1a"},
    {'.', "3a 3a"},
    {':', "32 32;38 38"},
    {'-', "15 55"},
    {'/', "0a 60"},
    {'o', "40 60 62 42 40"}, /* degree sign */
    {'A', "0a 02 20 40 62 6a;06 66"},
    {'B', "0a 00 50 61 64 55 05;55 66 69 5a 0a"},
    {'C', "61 50 10 01 09 1a 5a 69"},
    {'D', "00 40 62 68 4a 0a 00"},
    {'E', "60 00 0a 6a;05 45"},
    {'F', "60 00 0a;05 45"},
    {'G', "61 50 10 01 09 1a 5a 69 66 36"},
    {'H', "00 0a;60 6a;05 65"},
    {'I', "10 50;30 3a;1a 5a"},
    {'K', "00 0a;60 05 6a"},
    {'L', "00 0a 6a"},
    {'M', "0a 00 36 60 6a"},
    {'N', "0a 00 6a 60"},
    {'O', "10 50 61 69 5a 1a 09 01 10"},
    {'P', "0a 00 50 61 64 55 05"},
    {'R', "0a 00 50 61 64 55 05;35 6a"},
    {'S', "61 50 10 01 04 15 55 66 69 5a 1a 09"},
    {'T', "00 60;30 3a"},
    {'U', "00 09 1a 5a 69 60"},
    {'V', "00 3a 60"},
    {'X', "00 6a;60 0a"},
    {'J', "50 59 4a 1a 09"},
    {'Q', "10 50 61 69 5a 1a 09 01 10;36 6a"},
};

static int hexv(char ch) { return ch >= 'a' ? ch - 'a' + 10 : ch - '0'; }

static const char *glyph_path(char ch)
{
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++)
        if (GLYPHS[i].ch == ch) return GLYPHS[i].path;
    return NULL;
}

static void draw_glyph(canvas_t *c, const char *path, double x, double y, double sc, double th,
                       rgb_t col, int a)
{
    double px = 0, py = 0;
    int have = 0;
    for (const char *p = path; *p;) {
        if (*p == ' ') { p++; continue; }
        if (*p == ';') { have = 0; p++; continue; }
        double X = x + hexv(p[0]) * sc, Y = y + hexv(p[1]) * sc;
        p += 2;
        if (have) seg_aa(c, px, py, X, Y, th / 2, col, a);
        px = X; py = Y; have = 1;
    }
}

static double text_width(const char *s, double h)
{
    int n = (int)strlen(s);
    return n ? (n * 8.0 - 2.0) * h / 10.0 : 0;
}

/* align: 0 = left edge at x, 1 = centred on x, 2 = right edge at x.  y = top of the text. */
static void draw_text(canvas_t *c, const char *s, double x, double y, double h, double th,
                      rgb_t col, int a, int align)
{
    double sc = h / 10.0, w = text_width(s, h);
    double cx = align == 1 ? x - w / 2 : (align == 2 ? x - w : x);
    for (; *s; s++, cx += 8.0 * sc) {
        const char *g = glyph_path(*s);
        if (g) draw_glyph(c, g, cx, y, sc, th, col, a);
    }
}

/* ===================================================================== */
/* themes and layout                                                      */
/* ===================================================================== */
typedef struct {
    const char *name;
    rgb_t accent;   /* gauges, mode name, highlights */
    rgb_t bg_in, bg_out;
} theme_t;

static const theme_t THEMES[3] = {
    /* DRIVE_ECO     */ {"ECO", {96, 206, 164}, {10, 22, 21}, {3, 8, 8}},
    /* DRIVE_COMFORT */ {"COMFORT", {218, 186, 136}, {17, 18, 23}, {5, 6, 9}},
    /* DRIVE_SPORT   */ {"SPORT", {238, 72, 54}, {29, 12, 12}, {8, 3, 3}},
};

static const rgb_t WHITE = {240, 243, 247};
static const rgb_t DIM = {128, 134, 142};
static const rgb_t TRACK = {40, 45, 52};
static const rgb_t RED = {232, 76, 60};
static const rgb_t AMBER = {255, 170, 0};
static const rgb_t GREEN = {46, 204, 113};

typedef struct {
    int cx, cy, n;
    double rc, ht, glow;
    int *x, *y;
    float *f;
    uint8_t *core, *halo;
} ring_t;

typedef struct {
    double u;                 /* scale relative to a 1280x800 design */
    int w, h;
    int cxl, cxr, cy;         /* dial centres */
    double R, rc, ht;         /* dial outer radius, ring centre radius, ring half width */
    double px, py, pw, ph;    /* centre panel */
    ring_t ring[2];           /* 0 = speed, 1 = rpm */
} layout_t;

static layout_t L;

static void ring_build(ring_t *r, int cx, int cy, double rc, double ht, double glow)
{
    free(r->x); free(r->y); free(r->f); free(r->core); free(r->halo);
    int cap = (int)(2 * PI * rc * (2 * (ht + glow))) + 2048;
    r->x = malloc(cap * sizeof(int));
    r->y = malloc(cap * sizeof(int));
    r->f = malloc(cap * sizeof(float));
    r->core = malloc(cap);
    r->halo = malloc(cap);
    r->n = 0;
    r->cx = cx; r->cy = cy; r->rc = rc; r->ht = ht; r->glow = glow;

    double outer = rc + ht + glow + 1;
    for (int y = (int)(cy - outer); y <= (int)(cy + outer); y++)
        for (int x = (int)(cx - outer); x <= (int)(cx + outer); x++) {
            double dx = x - cx, dy = y - cy, d = sqrt(dx * dx + dy * dy), dd = fabs(d - rc);
            double core = clamp01(ht + 0.5 - dd);
            double halo = clamp01((ht + glow - dd) / glow);
            if (core <= 0 && halo <= 0) continue;
            double rel = atan2(dy, dx) * 180.0 / PI - START_DEG;
            if (rel < 0) rel += 360.0;
            if (rel > SWEEP_DEG || r->n >= cap) continue;
            r->x[r->n] = x;
            r->y[r->n] = y;
            r->f[r->n] = (float)(rel / SWEEP_DEG);
            r->core[r->n] = (uint8_t)(core * 255);
            r->halo[r->n] = (uint8_t)(halo * halo * 0.30 * 255);
            r->n++;
        }
}

static void layout_init(int w, int h)
{
    memset(&L, 0, sizeof L);
    L.w = w; L.h = h;
    L.u = fmin(w / 1280.0, h / 800.0);
    double u = L.u;
    L.R = fmin(w * 0.19, h * 0.30);
    L.ht = 4.5 * u;
    L.rc = L.R - 6 * u - L.ht;
    L.cxl = (int)(w * 0.195);
    L.cxr = (int)(w * 0.805);
    L.cy = (int)(h * 0.56);
    L.pw = (L.cxr - L.cxl) - 2 * L.R - 24 * u;
    L.ph = 560 * u;
    L.px = w / 2.0 - L.pw / 2;
    L.py = 120 * u;
    ring_build(&L.ring[0], L.cxl, L.cy, L.rc, L.ht, 10 * u);
    ring_build(&L.ring[1], L.cxr, L.cy, L.rc, L.ht, 10 * u);
}

/* ===================================================================== */
/* static layer (per theme)                                               */
/* ===================================================================== */
static uint8_t *g_layer[3];

static void fill_background(canvas_t *c, const theme_t *t)
{
    double cx = c->w / 2.0, cy = c->h * 0.52;
    double maxd = hypot(cx, cy) * 0.95;
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = (uint32_t *)(c->px + (size_t)y * c->stride);
        for (int x = 0; x < c->w; x++) {
            double k = clamp01(hypot(x - cx, (y - cy) * 1.15) / maxd);
            k = k * k * (3 - 2 * k); /* smoothstep */
            int r = (int)(t->bg_in.r + (t->bg_out.r - t->bg_in.r) * k);
            int g = (int)(t->bg_in.g + (t->bg_out.g - t->bg_in.g) * k);
            int b = (int)(t->bg_in.b + (t->bg_out.b - t->bg_in.b) * k);
            row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void polar(double cx, double cy, double r, double deg, double *x, double *y)
{
    *x = cx + r * cos(deg * PI / 180.0);
    *y = cy + r * sin(deg * PI / 180.0);
}

/* scale: ticks, labels and the dim track of one dial */
static void draw_scale(canvas_t *c, ring_t *r, int n, int major_every, int label_mul, int label_div,
                       double red_from_f, double u)
{
    /* track */
    for (int i = 0; i < r->n; i++)
        blend_px(c, r->x[i], r->y[i], r->f[i] >= red_from_f ? C(86, 36, 34) : TRACK, r->core[i]);

    double r_out = r->rc - r->ht - 9 * u;
    for (int i = 0; i <= n; i++) {
        double f = (double)i / n, deg = START_DEG + SWEEP_DEG * f;
        int major = i % major_every == 0;
        double len = (major ? 22 : 11) * u;
        rgb_t col = f >= red_from_f ? C(190, 82, 74) : (major ? C(205, 210, 216) : C(112, 118, 126));
        double x0, y0, x1, y1;
        polar(r->cx, r->cy, r_out, deg, &x0, &y0);
        polar(r->cx, r->cy, r_out - len, deg, &x1, &y1);
        seg_aa(c, x0, y0, x1, y1, (major ? 1.6 : 1.0) * u, col, 255);
        if (major) {
            char buf[8];
            snprintf(buf, sizeof buf, "%d", i * label_mul / label_div);
            polar(r->cx, r->cy, r_out - len - 20 * u, deg, &x1, &y1);
            draw_text(c, buf, x1, y1 - 8 * u, 16 * u, 1.7 * u, col, 255, 1);
        }
    }
}

static void build_layer(int i)
{
    const theme_t *t = &THEMES[i];
    double u = L.u;
    g_layer[i] = malloc((size_t)L.w * L.h * 4);
    canvas_t c = {g_layer[i], L.w * 4, L.w, L.h};
    fill_background(&c, t);

    /* dial scales. speed: 0..240, a label every 40. rpm: 0..8 (x1000), redline from 6500 */
    draw_scale(&c, &L.ring[0], 24, 4, 10, 1, 2.0, u);
    draw_scale(&c, &L.ring[1], 16, 2, 1, 2, REDLINE_RPM / MAX_RPM, u);

    /* dial captions in the open bottom of each dial */
    draw_text(&c, "KM/H", L.cxl, L.cy + L.R * 0.60, 16 * u, 1.7 * u, DIM, 255, 1);
    draw_text(&c, "X1000 RPM", L.cxr, L.cy + L.R * 0.60, 16 * u, 1.7 * u, DIM, 255, 1);

    /* centre panel frame */
    rrect_aa(&c, L.px, L.py, L.pw, L.ph, 26 * u, C(255, 255, 255), 10, 0);
    rrect_aa(&c, L.px, L.py, L.pw, L.ph, 26 * u, t->accent, 70, 1.4 * u);

    /* two thin accent lines above the panel (design motif) */
    double lx0 = L.px + 30 * u, lx1 = L.px + L.pw - 30 * u;
    seg_aa(&c, lx0, L.py - 18 * u, lx1, L.py - 18 * u, 0.8 * u, t->accent, 150);
    seg_aa(&c, lx0 + 20 * u, L.py - 11 * u, lx1 - 20 * u, L.py - 11 * u, 0.8 * u, t->accent, 80);

    /* panel labels */
    double lx = L.px + 30 * u, rx = L.px + L.pw - 30 * u;
    double y_trip = L.py + 188 * u, y_range = L.py + 240 * u;
    draw_text(&c, "TRIP", lx, y_trip, 14 * u, 1.5 * u, DIM, 255, 0);
    draw_text(&c, "RANGE", lx, y_range, 14 * u, 1.5 * u, DIM, 255, 0);
    seg_aa(&c, lx, L.py + 160 * u, rx, L.py + 160 * u, 0.6 * u, C(255, 255, 255), 40);
    seg_aa(&c, lx, L.py + 302 * u, rx, L.py + 302 * u, 0.6 * u, C(255, 255, 255), 40);

    double y_fuel = L.py + 340 * u, y_cool = L.py + 396 * u;
    draw_text(&c, "E", lx, y_fuel - 20 * u, 13 * u, 1.4 * u, DIM, 255, 0);
    draw_text(&c, "F", rx, y_fuel - 20 * u, 13 * u, 1.4 * u, DIM, 255, 2);
    draw_text(&c, "C", lx, y_cool - 20 * u, 13 * u, 1.4 * u, DIM, 255, 0);
    draw_text(&c, "H", rx, y_cool - 20 * u, 13 * u, 1.4 * u, DIM, 255, 2);
    rrect_aa(&c, lx, y_fuel, rx - lx, 8 * u, 4 * u, TRACK, 255, 0);
    rrect_aa(&c, lx, y_cool, rx - lx, 8 * u, 4 * u, TRACK, 255, 0);
    seg_aa(&c, lx, L.py + 442 * u, rx, L.py + 442 * u, 0.6 * u, C(255, 255, 255), 40);
}

/* ===================================================================== */
/* dynamic drawing                                                        */
/* ===================================================================== */
static void lit_ring(canvas_t *c, ring_t *r, double frac, double red_f, rgb_t base, double u)
{
    frac = clamp01(frac);
    for (int i = 0; i < r->n; i++) {
        if (r->f[i] > frac) continue;
        rgb_t col = r->f[i] >= red_f ? RED : base;
        blend_px(c, r->x[i], r->y[i], col, r->halo[i]);
        blend_px(c, r->x[i], r->y[i], col, r->core[i]);
    }
    double deg = START_DEG + SWEEP_DEG * frac, x, y;
    polar(r->cx, r->cy, r->rc, deg, &x, &y);
    rgb_t hc = frac >= red_f ? RED : base;
    disc_aa(c, x, y, 17 * u, hc, 60);
    disc_aa(c, x, y, 8.5 * u, WHITE, 255);
}

/* Icons are designed on a 48 px grid; g_is scales every size and offset with the screen. */
static double g_is = 1.0;
#define K(v) ((v) * g_is)

static void icon_engine(canvas_t *c, double x, double y, rgb_t col, int a)
{
    rrect_aa(c, x + K(10), y + K(16), K(26), K(18), K(3), col, a, 0);
    rrect_aa(c, x + K(16), y + K(10), K(14), K(7), K(2), col, a, 0);
    rrect_aa(c, x + K(4), y + K(20), K(6), K(10), K(2), col, a, 0);
    rrect_aa(c, x + K(36), y + K(20), K(6), K(10), K(2), col, a, 0);
}

static void icon_battery(canvas_t *c, double x, double y, rgb_t col, int a)
{
    rrect_aa(c, x + K(4), y + K(14), K(40), K(24), K(3), col, a, 0);
    rrect_aa(c, x + K(10), y + K(10), K(8), K(6), K(1), col, a, 0);
    rrect_aa(c, x + K(30), y + K(10), K(8), K(6), K(1), col, a, 0);
    seg_aa(c, x + K(11), y + K(26), x + K(19), y + K(26), K(1.3), C(10, 11, 14), 255);
    seg_aa(c, x + K(29), y + K(26), x + K(37), y + K(26), K(1.3), C(10, 11, 14), 255);
    seg_aa(c, x + K(33), y + K(22), x + K(33), y + K(30), K(1.3), C(10, 11, 14), 255);
}

static void icon_oil(canvas_t *c, double x, double y, rgb_t col, int a)
{
    disc_aa(c, x + K(22), y + K(30), K(11), col, a);
    tri_fill(c, x + K(22), y + K(6), x + K(11.5), y + K(27), x + K(32.5), y + K(27), col, a);
}

static void icon_seatbelt(canvas_t *c, double x, double y, rgb_t col, int a)
{
    disc_aa(c, x + K(24), y + K(10), K(6), col, a);
    rrect_aa(c, x + K(16), y + K(18), K(16), K(22), K(4), col, a, 0);
    seg_aa(c, x + K(14), y + K(17), x + K(36), y + K(42), K(1.6), C(10, 11, 14), 255);
}

static void icon_turn(canvas_t *c, double x, double y, int left, rgb_t col, int a)
{
    if (left) {
        tri_fill(c, x + K(4), y + K(24), x + K(24), y + K(6), x + K(24), y + K(42), col, a);
        rrect_aa(c, x + K(22), y + K(17), K(22), K(14), K(2), col, a, 0);
    } else {
        tri_fill(c, x + K(44), y + K(24), x + K(24), y + K(6), x + K(24), y + K(42), col, a);
        rrect_aa(c, x + K(4), y + K(17), K(22), K(14), K(2), col, a, 0);
    }
}

static void bar(canvas_t *c, double x, double y, double w, double h, double frac, rgb_t col)
{
    frac = clamp01(frac);
    if (frac * w > h) rrect_aa(c, x, y, frac * w, h, h / 2, col, 255, 0);
    else if (frac > 0) disc_aa(c, x + h / 2, y + h / 2, h / 2, col, 255);
}

/* In the safe state (DATA_INVALID) the display must not show a value that could be taken for a
 * real one: no zeros and no frozen last value, only dashes. */
static void draw_dynamic(canvas_t *c, const vehicle_msg_t *v, data_status_t st, int recent_err,
                         const e2e_state_t *e, const theme_t *t, double trip_km, double clock_s,
                         int blink)
{
    double u = L.u;
    int live = st != DATA_INVALID;
    const rgb_t safe = {200, 80, 70};

    /* dial arcs */
    if (live) {
        lit_ring(c, &L.ring[0], v->speed_kmh / MAX_SPEED, 2.0, t->accent, u);
        lit_ring(c, &L.ring[1], v->rpm / MAX_RPM, REDLINE_RPM / MAX_RPM, t->accent, u);
    }

    /* speed value inside the left dial */
    char buf[48];
    if (live) snprintf(buf, sizeof buf, "%d", (int)(v->speed_kmh + 0.5));
    else snprintf(buf, sizeof buf, "---");
    draw_text(c, buf, L.cxl, L.cy - 62 * u, 118 * u, 8.5 * u, live ? WHITE : safe, 255, 1);

    /* gear + rpm inside the right dial */
    char gear = live ? (v->gear >= 1 && v->gear <= 6 ? (char)('0' + v->gear) : 'N') : '-';
    char gs[2] = {gear, 0};
    draw_text(c, gs, L.cxr, L.cy - 62 * u, 118 * u, 8.5 * u, live ? WHITE : safe, 255, 1);
    if (live) snprintf(buf, sizeof buf, "%.1f", v->rpm / 1000.0);
    else snprintf(buf, sizeof buf, "-.-");
    draw_text(c, buf, L.cxr, L.cy + 84 * u, 26 * u, 2.4 * u, live ? t->accent : safe, 255, 1);

    /* centre panel: clock */
    int mins = 10 * 60 + 24 + (int)(clock_s / 6.0);
    snprintf(buf, sizeof buf, "%02d:%02d", (mins / 60) % 24, mins % 60);
    draw_text(c, buf, L.px + L.pw / 2, L.py + 30 * u, 40 * u, 3.0 * u, WHITE, 255, 1);

    /* drive mode pill */
    double pw = 180 * u, ph = 38 * u, pxl = L.px + L.pw / 2 - pw / 2, pyl = L.py + 92 * u;
    rrect_aa(c, pxl, pyl, pw, ph, ph / 2, t->accent, 35, 0);
    rrect_aa(c, pxl, pyl, pw, ph, ph / 2, t->accent, 255, 1.6 * u);
    draw_text(c, t->name, L.px + L.pw / 2, pyl + 10 * u, 18 * u, 2.0 * u, t->accent, 255, 1);

    /* trip / range */
    double lx = L.px + 30 * u, rx = L.px + L.pw - 30 * u;
    snprintf(buf, sizeof buf, "%.1f KM", trip_km);
    draw_text(c, buf, rx, L.py + 182 * u, 24 * u, 2.3 * u, WHITE, 255, 2);
    if (live) snprintf(buf, sizeof buf, "%d KM", (int)(v->fuel_pct * 6.0));
    else snprintf(buf, sizeof buf, "--- KM");
    draw_text(c, buf, rx, L.py + 234 * u, 24 * u, 2.3 * u, live ? WHITE : safe, 255, 2);

    /* fuel and coolant bars (left empty in the safe state) */
    double y_fuel = L.py + 340 * u, y_cool = L.py + 396 * u;
    if (live) {
        bar(c, lx, y_fuel, rx - lx, 8 * u, v->fuel_pct / 100.0, v->fuel_pct < 15 ? RED : t->accent);
        double cool_f = clamp01((v->coolant_c - 40.0) / 80.0);
        bar(c, lx, y_cool, rx - lx, 8 * u, cool_f, v->coolant_c > 110 ? RED : t->accent);
    }

    /* gear selector P R N D (nothing highlighted in the safe state) */
    static const char sel[4] = {'P', 'R', 'N', 'D'};
    int active = !live ? -1 : (v->gear == 0 ? 2 : 3);
    for (int i = 0; i < 4; i++) {
        char s[2] = {sel[i], 0};
        double gx = L.px + L.pw * (0.2 + 0.2 * i);
        draw_text(c, s, gx, L.py + 470 * u, 30 * u, 2.6 * u, i == active ? t->accent : C(86, 92, 100),
                  255, 1);
        if (i == active)
            seg_aa(c, gx - 12 * u, L.py + 518 * u, gx + 12 * u, L.py + 518 * u, 1.2 * u, t->accent, 255);
    }

    /* warning lamps (dim when off, all dim in the safe state) */
    uint16_t w = live ? v->warnings : 0;
    const rgb_t off = {70, 76, 84};
    double s = u; /* icons are drawn on a 48 px grid */
    (void)s;
    g_is = u;
    double iy = 28 * u, cell = 70 * u, gx0 = L.w / 2.0 - 2 * cell + (cell - 48 * u) / 2;
    icon_engine(c, gx0, iy, (w & W_CHECK_ENGINE) ? AMBER : off, (w & W_CHECK_ENGINE) ? 255 : 90);
    icon_battery(c, gx0 + cell, iy, (w & W_BATTERY) ? RED : off, (w & W_BATTERY) ? 255 : 90);
    icon_oil(c, gx0 + 2 * cell, iy, (w & W_OIL) ? RED : off, (w & W_OIL) ? 255 : 90);
    icon_seatbelt(c, gx0 + 3 * cell, iy, (w & W_SEATBELT) ? RED : off, (w & W_SEATBELT) ? 255 : 90);
    int tl = (w & W_TURN_LEFT) && blink, tr = (w & W_TURN_RIGHT) && blink;
    icon_turn(c, L.cxl - 24 * u, iy, 1, tl ? GREEN : off, tl ? 255 : 70);
    icon_turn(c, L.cxr - 24 * u, iy, 0, tr ? GREEN : off, tr ? 255 : 70);

    /* data link lamp (top-left): green valid, amber errors seen recently, red safe state */
    disc_aa(c, 28 * u, 28 * u, 7 * u, st == DATA_VALID ? GREEN : (st == DATA_DEGRADED ? AMBER : RED), 255);

    /* status notice under the drive mode */
    const char *notice = NULL;
    rgb_t ncol = RED;
    if (st == DATA_DEGRADED) { notice = "DATA ERROR"; ncol = AMBER; }
    else if (st == DATA_INVALID) notice = recent_err ? "DATA INVALID" : "NO SIGNAL";
    if (notice) draw_text(c, notice, L.px + L.pw / 2, L.py + 138 * u, 14 * u, 1.6 * u, ncol, 255, 1);

    /* error counters (only once something was rejected): CRC, wrong counter, range, source fault, jump, lost, repeat */
    if (e->n_crc + e->n_wrongseq + e->n_range + e->n_jump + e->n_lost + e->n_repeat + e->n_src > 0) {
        snprintf(buf, sizeof buf, "CRC %u SEQ %u RNG %u SRC %u", (unsigned)(e->n_crc > 999 ? 999 : e->n_crc),
                 (unsigned)(e->n_wrongseq > 999 ? 999 : e->n_wrongseq),
                 (unsigned)(e->n_range > 999 ? 999 : e->n_range), (unsigned)(e->n_src > 999 ? 999 : e->n_src));
        draw_text(c, buf, L.px + L.pw / 2, L.py + 528 * u, 10 * u, 1.2 * u, DIM, 255, 1);
        snprintf(buf, sizeof buf, "JMP %u LOST %u REP %u", (unsigned)(e->n_jump > 999 ? 999 : e->n_jump),
                 (unsigned)(e->n_lost > 999 ? 999 : e->n_lost),
                 (unsigned)(e->n_repeat > 999 ? 999 : e->n_repeat));
        draw_text(c, buf, L.px + L.pw / 2, L.py + 542 * u, 10 * u, 1.2 * u, DIM, 255, 1);
    }
}

/* ===================================================================== */
/* main                                                                   */
/* ===================================================================== */
int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 120;

    pthread_t th;
    if (pthread_create(&th, NULL, receiver, NULL)) {
        perror("pthread_create");
        return 1;
    }

    screen_context_t ctx;
    screen_window_t win;
    int format = SCREEN_FORMAT_RGBA8888;
    int usage = SCREEN_USAGE_WRITE | SCREEN_USAGE_NATIVE;
    int size[2];

    if (screen_create_context(&ctx, SCREEN_APPLICATION_CONTEXT) ||
        screen_create_window(&win, ctx)) {
        perror("screen_create_context/window");
        return 1;
    }
    screen_set_window_property_iv(win, SCREEN_PROPERTY_FORMAT, &format);
    screen_set_window_property_iv(win, SCREEN_PROPERTY_USAGE, &usage);
    if (screen_create_window_buffers(win, 2)) {
        perror("screen_create_window_buffers");
        return 1;
    }
    screen_get_window_property_iv(win, SCREEN_PROPERTY_BUFFER_SIZE, size);
    printf("window buffer %dx%d, running %d s\n", size[0], size[1], seconds);
    fflush(stdout);

    layout_init(size[0], size[1]);
    for (int i = 0; i < 3; i++) build_layer(i); /* all themes up front: no hitch on mode change */
    printf("static layers ready\n");
    fflush(stdout);

    double t0 = mono_now(), t_stat = t0, t_prev = t0, trip = 0;
    int frames = 0, theme = DRIVE_COMFORT;
    double draw_t = 0, post_t = 0;
    int dirty[4] = {0, 0, size[0], size[1]};

    while (mono_now() - t0 <= seconds) {
        screen_buffer_t rb[2];
        void *ptr = NULL;
        int stride = 0;
        screen_get_window_property_pv(win, SCREEN_PROPERTY_RENDER_BUFFERS, (void **)rb);
        screen_get_buffer_property_pv(rb[0], SCREEN_PROPERTY_POINTER, &ptr);
        screen_get_buffer_property_iv(rb[0], SCREEN_PROPERTY_STRIDE, &stride);

        vehicle_msg_t v;
        e2e_state_t e;
        double now = mono_now(), last_valid, last_err;
        int have_valid;
        pthread_mutex_lock(&g_lock);
        v = g_msg;
        e = g_e2e;
        last_valid = g_last_valid;
        last_err = g_last_err;
        have_valid = g_have_valid;
        pthread_mutex_unlock(&g_lock);

        /* decide what the driver may be shown */
        int recent_err = last_err > 0 && now - last_err < ERR_HOLD_SEC;
        data_status_t st = DATA_INVALID;
        if (have_valid && now - last_valid < STALE_SEC) st = recent_err ? DATA_DEGRADED : DATA_VALID;
        if (st == DATA_INVALID) memset(&v, 0, sizeof v); /* never draw a stale value */

        if (st != DATA_INVALID) {
            theme = v.drive_mode <= DRIVE_SPORT ? v.drive_mode : DRIVE_COMFORT;
            trip += v.speed_kmh * (now - t_prev) / 3600.0;
        }
        t_prev = now;

        double d0 = mono_now();
        for (int y = 0; y < size[1]; y++)
            memcpy((uint8_t *)ptr + (size_t)y * stride, g_layer[theme] + (size_t)y * size[0] * 4,
                   (size_t)size[0] * 4);
        canvas_t cv = {ptr, stride, size[0], size[1]};
        draw_dynamic(&cv, &v, st, recent_err, &e, &THEMES[theme], trip, now - t0, (int)(now * 2.5) & 1);
        double d1 = mono_now();
        screen_post_window(win, rb[0], 1, dirty, 0);
        double d2 = mono_now();
        usleep(10000);

        frames++;
        draw_t += d1 - d0;
        post_t += d2 - d1;
        if (now - t_stat >= 5.0) {
            printf("%.1f fps (draw %.1f ms, post %.1f ms per frame)  data: %s  ok=%u crc=%u seq=%u rng=%u jmp=%u lost=%u rep=%u src=%u\n",
                   frames / (now - t_stat), 1000 * draw_t / frames, 1000 * post_t / frames,
                   st == DATA_VALID ? "valid" : (st == DATA_DEGRADED ? "degraded" : "INVALID"), e.n_ok, e.n_crc,
                   e.n_wrongseq, e.n_range, e.n_jump, e.n_lost, e.n_repeat, e.n_src);
            fflush(stdout);
            frames = 0;
            draw_t = post_t = 0;
            t_stat = now;
        }
    }

    pthread_mutex_lock(&g_lock);
    e2e_state_t fin = g_e2e;
    pthread_mutex_unlock(&g_lock);
    printf("final: ok=%u crc=%u seq=%u rng=%u jmp=%u lost=%u rep=%u src=%u\n", fin.n_ok, fin.n_crc, fin.n_wrongseq,
           fin.n_range, fin.n_jump, fin.n_lost, fin.n_repeat, fin.n_src);
    fflush(stdout);

    screen_destroy_window(win);
    screen_destroy_context(ctx);
    return 0;
}
