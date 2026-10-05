/*
 * stlview - rotating STL viewer for the terminal.
 *
 * Renders a solid, z-buffered, Lambert-shaded model using upper-half-block
 * glyphs (U+2580) with truecolor foreground/background, giving two vertical
 * pixels per character cell. A braille wireframe mode (U+2800..U+28FF) is
 * available with 'w' for a crisp line-art look at 2x4 sub-cell resolution.
 *
 * Single file, libc + libm only. Linux / POSIX terminals.
 *
 *   cc -O2 -o stlview stlview.c -lm
 *   ./stlview model.stl
 *
 * Controls:
 *   q / Ctrl-C   quit
 *   space        pause / resume auto-rotation
 *   arrows       nudge rotation
 *   + / -        zoom in / out
 *   w            toggle solid / braille-wireframe
 *   p            toggle orthographic / perspective projection
 *   [ / ]        shorten / lengthen focal length (perspective strength)
 *   r            reset view
 */

#define _POSIX_C_SOURCE 200809L   /* clock_gettime, nanosleep under -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

/* ------------------------------------------------------------------ vec3 */

typedef struct { float x, y, z; } Vec3;

static inline Vec3 vsub(Vec3 a, Vec3 b) { return (Vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 vcross(Vec3 a, Vec3 b) {
    return (Vec3){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float vdot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 vnorm(Vec3 a) {
    float l = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);
    if (l < 1e-12f) return (Vec3){0,0,0};
    return (Vec3){a.x/l, a.y/l, a.z/l};
}

/* ------------------------------------------------------------------ mesh */

typedef struct {
    float *tri;   /* 9 floats per triangle: v0,v1,v2 (already normalized) */
    int    n;     /* triangle count */
} Mesh;

/* Read a little-endian uint32 from a buffer. */
static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static int load_stl(const char *path, Mesh *m) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "stlview: cannot open '%s': %s\n", path, strerror(errno)); return -1; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 84) { fprintf(stderr, "stlview: file too small to be an STL\n"); fclose(f); return -1; }

    /* Binary STL is exactly 84 + 50*ntri bytes. That size check is the only
       reliable way to tell binary from ASCII (ASCII files can start "solid"
       but so can some binary ones). */
    unsigned char header[84];
    if (fread(header, 1, 84, f) != 84) { fclose(f); return -1; }
    uint32_t ntri = rd_u32(header + 80);
    long expected = 84L + 50L * (long)ntri;

    if (expected == fsize) {
        /* ---- binary ---- */
        m->n = (int)ntri;
        m->tri = malloc(sizeof(float) * 9 * (size_t)ntri);
        if (!m->tri) { fclose(f); return -1; }
        unsigned char rec[50];
        for (uint32_t i = 0; i < ntri; i++) {
            if (fread(rec, 1, 50, f) != 50) { fprintf(stderr, "stlview: truncated binary STL\n"); fclose(f); free(m->tri); return -1; }
            /* rec[0..11] = normal (ignored, recomputed); rec[12..47] = 9 floats */
            memcpy(&m->tri[i*9], rec + 12, sizeof(float) * 9);
        }
        fclose(f);
        return 0;
    }

    /* ---- ASCII ---- : slurp file, scan for "vertex x y z" triples. */
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)fsize, f);
    buf[got] = '\0';
    fclose(f);

    int cap = 1024, cnt = 0;          /* count of vertices */
    float *v = malloc(sizeof(float) * 3 * (size_t)cap);
    if (!v) { free(buf); return -1; }

    char *p = buf;
    while ((p = strstr(p, "vertex")) != NULL) {
        p += 6;
        float a, b, c;
        if (sscanf(p, "%f %f %f", &a, &b, &c) == 3) {
            if (cnt >= cap) { cap *= 2; v = realloc(v, sizeof(float)*3*(size_t)cap); if(!v){free(buf);return -1;} }
            v[cnt*3+0] = a; v[cnt*3+1] = b; v[cnt*3+2] = c;
            cnt++;
        }
    }
    free(buf);
    if (cnt < 3 || cnt % 3 != 0) { fprintf(stderr, "stlview: no valid vertices found (not an STL?)\n"); free(v); return -1; }
    m->n = cnt / 3;
    m->tri = v;       /* already 9 floats per triangle */
    return 0;
}

/* Center on bounding-box midpoint, scale so the bounding sphere has radius 1.
   Using the sphere radius (not the box) means the model never clips as it
   rotates. */
static void normalize_mesh(Mesh *m) {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    int nf = m->n * 9;
    for (int i = 0; i < nf; i += 3) {
        for (int k = 0; k < 3; k++) {
            float val = m->tri[i+k];
            if (val < lo[k]) lo[k] = val;
            if (val > hi[k]) hi[k] = val;
        }
    }
    float cx = (lo[0]+hi[0])*0.5f, cy = (lo[1]+hi[1])*0.5f, cz = (lo[2]+hi[2])*0.5f;
    float r = 1e-9f;
    for (int i = 0; i < nf; i += 3) {
        float dx = m->tri[i+0]-cx, dy = m->tri[i+1]-cy, dz = m->tri[i+2]-cz;
        float d = sqrtf(dx*dx+dy*dy+dz*dz);
        if (d > r) r = d;
    }
    float inv = 1.0f / r;
    for (int i = 0; i < nf; i += 3) {
        m->tri[i+0] = (m->tri[i+0]-cx)*inv;
        m->tri[i+1] = (m->tri[i+1]-cy)*inv;
        m->tri[i+2] = (m->tri[i+2]-cz)*inv;
    }
}

/* ------------------------------------------------------------- terminal */

static struct termios g_orig_termios;
static int            g_raw_active = 0;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_resized = 1;   /* force initial size query */

static void term_restore(void) {
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_active = 0;
    }
    /* show cursor, leave alt screen, reset attributes */
    const char *s = "\x1b[?25h\x1b[?1049l\x1b[0m";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* ignore */ }
}

static void term_raw(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) < 0) return;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;     /* non-blocking read */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_active = 1;
    const char *s = "\x1b[?1049h\x1b[?25l\x1b[2J";  /* alt screen, hide cursor, clear */
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* ignore */ }
}

static void on_winch(int sig) { (void)sig; g_resized = 1; }
static void on_quit(int sig)  { (void)sig; g_running = 0; }

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------- output buffer */

typedef struct { char *buf; size_t len, cap; } OutBuf;

static void ob_reserve(OutBuf *o, size_t extra) {
    if (o->len + extra <= o->cap) return;
    size_t nc = o->cap ? o->cap : 4096;
    while (nc < o->len + extra) nc *= 2;
    o->buf = realloc(o->buf, nc);
    o->cap = nc;
}
static void ob_append(OutBuf *o, const char *s, size_t n) {
    ob_reserve(o, n);
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}
static void ob_puts(OutBuf *o, const char *s) { ob_append(o, s, strlen(s)); }
static void ob_clear(OutBuf *o) { o->len = 0; }

/* ----------------------------------------------------------- rendering */

typedef struct { unsigned char r, g, b, set; } Pixel;

typedef struct {
    int W, H;             /* terminal cells */
    int pw, ph;           /* solid-mode pixel grid: pw=W, ph=2H */
    Pixel *fb;            /* pw*ph framebuffer */
    float *zb;            /* pw*ph depth buffer */
    unsigned char *dots;  /* braille dot grid (2W) x (4H) */
    int dw, dh;
} Canvas;

static void canvas_alloc(Canvas *c, int W, int H) {
    free(c->fb); free(c->zb); free(c->dots);
    c->W = W; c->H = H;
    c->pw = W; c->ph = 2*H;
    c->dw = 2*W; c->dh = 4*H;
    c->fb   = malloc(sizeof(Pixel) * (size_t)c->pw * (size_t)c->ph);
    c->zb   = malloc(sizeof(float) * (size_t)c->pw * (size_t)c->ph);
    c->dots = malloc((size_t)c->dw * (size_t)c->dh);
}

/* Rotate a model-space point by angles (ay around Y, then ax around X). */
static inline Vec3 rotate(Vec3 v, float say, float cay, float sax, float cax) {
    float x1 =  v.x*cay + v.z*say;
    float z1 = -v.x*say + v.z*cay;
    float y1 =  v.y;
    float y2 =  y1*cax - z1*sax;
    float z2 =  y1*sax + z1*cax;
    return (Vec3){ x1, y2, z2 };
}

/* Project a rotated model-space point to screen pixels.
 *  - orthographic: x,y just scaled; depth ignored for position.
 *  - perspective : divide by depth-from-camera (the classic x/z, y/z trick).
 *    The camera sits at +cam on the Z axis looking toward -Z, so the depth in
 *    front of it is (cam - z); we scale by cam/zc so that a point at the model
 *    center (z=0) lands exactly where orthographic would — perspective only
 *    bends things nearer/farther around that center. zc is clamped > 0 so the
 *    division never blows up. Depth ordering is unchanged (larger z = nearer),
 *    so the z-buffer test in the callers needs no modification. */
static inline void project(Vec3 r, float ox, float oy, float scale,
                           int persp, float cam, float *sx, float *sy) {
    if (persp) {
        float zc = cam - r.z;
        if (zc < 0.05f) zc = 0.05f;
        float fac = cam / zc;
        *sx = ox + r.x * fac * scale;
        *sy = oy - r.y * fac * scale;
    } else {
        *sx = ox + r.x * scale;
        *sy = oy - r.y * scale;
    }
}

/* Lighting + base material. */
static const Vec3 LIGHT_DIR = { 0.40f, 0.55f, 0.75f };  /* normalized below */
static const float BASE_R = 232, BASE_G = 150, BASE_B = 64;   /* warm "3D-print" orange */
static const float AMBIENT = 0.18f;

static void render_solid(Canvas *c, Mesh *m, float ay, float ax, float zoom,
                         int persp, float cam) {
    int N = c->pw * c->ph;
    for (int i = 0; i < N; i++) { c->fb[i].set = 0; c->zb[i] = -1e30f; }

    float say = sinf(ay), cay = cosf(ay), sax = sinf(ax), cax = cosf(ax);
    Vec3 L = vnorm(LIGHT_DIR);
    float scale = (c->pw < c->ph ? c->pw : c->ph) * 0.45f * zoom;
    float ox = c->pw * 0.5f, oy = c->ph * 0.5f;

    for (int t = 0; t < m->n; t++) {
        Vec3 r0 = rotate((Vec3){m->tri[t*9+0], m->tri[t*9+1], m->tri[t*9+2]}, say,cay,sax,cax);
        Vec3 r1 = rotate((Vec3){m->tri[t*9+3], m->tri[t*9+4], m->tri[t*9+5]}, say,cay,sax,cax);
        Vec3 r2 = rotate((Vec3){m->tri[t*9+6], m->tri[t*9+7], m->tri[t*9+8]}, say,cay,sax,cax);

        Vec3 nrm = vnorm(vcross(vsub(r1,r0), vsub(r2,r0)));
        float ndl = fabsf(vdot(nrm, L));         /* two-sided: winding-agnostic */
        float intensity = AMBIENT + (1.0f - AMBIENT) * ndl;
        unsigned char cr = (unsigned char)fminf(255.0f, BASE_R * intensity);
        unsigned char cg = (unsigned char)fminf(255.0f, BASE_G * intensity);
        unsigned char cb = (unsigned char)fminf(255.0f, BASE_B * intensity);

        /* screen-space coords (y flipped) */
        float ax0, ay0, ax1, ay1, ax2, ay2;
        project(r0, ox, oy, scale, persp, cam, &ax0, &ay0);
        project(r1, ox, oy, scale, persp, cam, &ax1, &ay1);
        project(r2, ox, oy, scale, persp, cam, &ax2, &ay2);

        float area = (ax1-ax0)*(ay2-ay0) - (ax2-ax0)*(ay1-ay0);
        if (fabsf(area) < 1e-6f) continue;
        float invarea = 1.0f / area;

        int minx = (int)floorf(fminf(ax0, fminf(ax1, ax2)));
        int maxx = (int)ceilf (fmaxf(ax0, fmaxf(ax1, ax2)));
        int miny = (int)floorf(fminf(ay0, fminf(ay1, ay2)));
        int maxy = (int)ceilf (fmaxf(ay0, fmaxf(ay1, ay2)));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= c->pw) maxx = c->pw - 1;
        if (maxy >= c->ph) maxy = c->ph - 1;

        for (int py = miny; py <= maxy; py++) {
            float fy = py + 0.5f;
            for (int px = minx; px <= maxx; px++) {
                float fx = px + 0.5f;
                float w0 = ((ax1-fx)*(ay2-fy) - (ax2-fx)*(ay1-fy)) * invarea;
                float w1 = ((ax2-fx)*(ay0-fy) - (ax0-fx)*(ay2-fy)) * invarea;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0*r0.z + w1*r1.z + w2*r2.z;
                int idx = py*c->pw + px;
                if (z > c->zb[idx]) {            /* nearer to viewer (+Z) */
                    c->zb[idx] = z;
                    c->fb[idx].r = cr; c->fb[idx].g = cg; c->fb[idx].b = cb; c->fb[idx].set = 1;
                }
            }
        }
    }
}

/* Background color for empty pixels. */
#define BG_R 14
#define BG_G 14
#define BG_B 20

static void emit_solid(Canvas *c, OutBuf *o) {
    char tmp[64];
    ob_puts(o, "\x1b[H");
    for (int y = 0; y < c->H; y++) {
        int len = snprintf(tmp, sizeof tmp, "\x1b[%d;1H", y + 1);
        ob_append(o, tmp, (size_t)len);
        int last_fr=-1,last_fg=-1,last_fb=-1,last_br=-1,last_bg=-1,last_bb=-1;
        for (int x = 0; x < c->W; x++) {
            Pixel *top = &c->fb[(2*y)*c->pw + x];
            Pixel *bot = &c->fb[(2*y+1)*c->pw + x];
            int fr = top->set ? top->r : BG_R, fg = top->set ? top->g : BG_G, fb = top->set ? top->b : BG_B;
            int br = bot->set ? bot->r : BG_R, bg = bot->set ? bot->g : BG_G, bb = bot->set ? bot->b : BG_B;
            if (fr!=last_fr || fg!=last_fg || fb!=last_fb) {
                len = snprintf(tmp, sizeof tmp, "\x1b[38;2;%d;%d;%dm", fr, fg, fb);
                ob_append(o, tmp, (size_t)len);
                last_fr=fr; last_fg=fg; last_fb=fb;
            }
            if (br!=last_br || bg!=last_bg || bb!=last_bb) {
                len = snprintf(tmp, sizeof tmp, "\x1b[48;2;%d;%d;%dm", br, bg, bb);
                ob_append(o, tmp, (size_t)len);
                last_br=br; last_bg=bg; last_bb=bb;
            }
            ob_puts(o, "\xe2\x96\x80");   /* U+2580 UPPER HALF BLOCK */
        }
        ob_puts(o, "\x1b[0m");
    }
}

/* ---- braille wireframe ---- */

static void plot_dot(Canvas *c, int x, int y) {
    if (x < 0 || y < 0 || x >= c->dw || y >= c->dh) return;
    c->dots[y*c->dw + x] = 1;
}
static void draw_line(Canvas *c, int x0, int y0, int x1, int y1) {
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx+dy;
    for (;;) {
        plot_dot(c, x0, y0);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void render_wire(Canvas *c, Mesh *m, float ay, float ax, float zoom,
                        int persp, float cam) {
    memset(c->dots, 0, (size_t)c->dw * (size_t)c->dh);
    float say = sinf(ay), cay = cosf(ay), sax = sinf(ax), cax = cosf(ax);
    float scale = (c->dw < c->dh ? c->dw : c->dh) * 0.45f * zoom;
    float ox = c->dw * 0.5f, oy = c->dh * 0.5f;

    for (int t = 0; t < m->n; t++) {
        Vec3 r[3];
        for (int k = 0; k < 3; k++)
            r[k] = rotate((Vec3){m->tri[t*9+k*3], m->tri[t*9+k*3+1], m->tri[t*9+k*3+2]}, say,cay,sax,cax);
        int sx[3], sy[3];
        for (int k = 0; k < 3; k++) {
            float fx, fy;
            project(r[k], ox, oy, scale, persp, cam, &fx, &fy);
            sx[k] = (int)lrintf(fx);
            sy[k] = (int)lrintf(fy);
        }
        draw_line(c, sx[0],sy[0], sx[1],sy[1]);
        draw_line(c, sx[1],sy[1], sx[2],sy[2]);
        draw_line(c, sx[2],sy[2], sx[0],sy[0]);
    }
}

static void emit_wire(Canvas *c, OutBuf *o) {
    /* dot (col,row)->bit value within a cell, per the Unicode braille layout */
    static const int BIT[4][2] = {
        {0x01,0x08}, {0x02,0x10}, {0x04,0x20}, {0x40,0x80}
    };
    char tmp[16];
    ob_puts(o, "\x1b[H\x1b[38;2;120;230;160m");   /* phosphor green */
    for (int y = 0; y < c->H; y++) {
        int len = snprintf(tmp, sizeof tmp, "\x1b[%d;1H", y + 1);
        ob_append(o, tmp, (size_t)len);
        for (int x = 0; x < c->W; x++) {
            int bits = 0;
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 2; dx++)
                    if (c->dots[(4*y+dy)*c->dw + (2*x+dx)]) bits |= BIT[dy][dx];
            int cp = 0x2800 + bits;
            char u[3] = { (char)(0xE0 | (cp>>12)), (char)(0x80 | ((cp>>6)&0x3F)), (char)(0x80 | (cp&0x3F)) };
            ob_append(o, u, 3);
        }
    }
    ob_puts(o, "\x1b[0m");
}

/* --------------------------------------------------------------- input */

/* Returns 0 if nothing read. Handles single bytes and escape sequences. */
static int read_key(void) {
    unsigned char buf[8];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) return 0;
    if (buf[0] == '\x1b' && n >= 3 && buf[1] == '[') {
        switch (buf[2]) {
            case 'A': return 1000;   /* up    */
            case 'B': return 1001;   /* down  */
            case 'C': return 1002;   /* right */
            case 'D': return 1003;   /* left  */
        }
        return 0;
    }
    return buf[0];
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *path = NULL;
    double fps = 30.0;
    int wireframe = 0;
    int persp_start = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--wireframe") || !strcmp(argv[i], "-w")) wireframe = 1;
        else if (!strcmp(argv[i], "--perspective") || !strcmp(argv[i], "-p")) persp_start = 1;
        else if (!strcmp(argv[i], "--fps") && i+1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: stlview [options] model.stl\n"
                   "  -w, --wireframe     start in braille-wireframe mode\n"
                   "  -p, --perspective   start with perspective projection\n"
                   "      --fps N         target frame rate (default 30)\n"
                   "controls: q quit | space pause | arrows rotate | +/- zoom\n"
                   "          w wireframe | p perspective | [ ] focal length | r reset\n");
            return 0;
        }
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: stlview [options] model.stl  (try --help)\n"); return 1; }
    if (fps < 1) fps = 1;
    if (fps > 120) fps = 120;

    Mesh mesh = {0};
    if (load_stl(path, &mesh) != 0) return 1;
    if (mesh.n == 0) { fprintf(stderr, "stlview: empty mesh\n"); return 1; }
    normalize_mesh(&mesh);

    if (!isatty(STDOUT_FILENO)) { fprintf(stderr, "stlview: stdout is not a terminal\n"); return 1; }

    atexit(term_restore);
    signal(SIGINT,  on_quit);
    signal(SIGTERM, on_quit);
    signal(SIGWINCH, on_winch);
    term_raw();

    Canvas canvas = {0};
    OutBuf out = {0};

    float ay = 0.6f, ax = -0.45f;   /* a pleasant 3/4 starting view */
    float zoom = 1.0f;
    float spin = 0.7f;              /* radians / second */
    int paused = 0;
    int persp = persp_start;       /* orthographic by default */
    float cam = 3.0f;              /* camera distance / focal length */
    const float ay0 = ay, ax0 = ax;

    double frame_dt = 1.0 / fps;
    double prev = now_sec();

    while (g_running) {
        if (g_resized) {
            struct winsize ws;
            int W = 80, H = 24;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
                W = ws.ws_col; H = ws.ws_row;
            }
            canvas_alloc(&canvas, W, H);
            g_resized = 0;
            ob_clear(&out); ob_puts(&out, "\x1b[2J");
            ssize_t wr = write(STDOUT_FILENO, out.buf, out.len); (void)wr;
        }

        /* input (drain any pending keys this frame) */
        int k;
        while ((k = read_key()) != 0) {
            switch (k) {
                case 'q': case 'Q': g_running = 0; break;
                case ' ': paused = !paused; break;
                case 'w': case 'W': wireframe = !wireframe; break;
                case 'r': case 'R': ay = ay0; ax = ax0; zoom = 1.0f; persp = 0; cam = 3.0f; break;
                case 'p': case 'P': persp = !persp; break;
                case '[': cam /= 1.1f; if (cam < 1.5f) cam = 1.5f; break;  /* stronger perspective */
                case ']': cam *= 1.1f; if (cam > 20.0f) cam = 20.0f; break; /* weaker / flatter   */
                case '+': case '=': zoom *= 1.1f; if (zoom > 8) zoom = 8; break;
                case '-': case '_': zoom /= 1.1f; if (zoom < 0.2f) zoom = 0.2f; break;
                case 1000: ax -= 0.15f; break;
                case 1001: ax += 0.15f; break;
                case 1002: ay += 0.15f; break;
                case 1003: ay -= 0.15f; break;
            }
        }

        double t = now_sec();
        double dt = t - prev;
        prev = t;
        if (!paused) ay += spin * (float)dt;

        if (wireframe) { render_wire(&canvas, &mesh, ay, ax, zoom, persp, cam); ob_clear(&out); emit_wire(&canvas, &out); }
        else           { render_solid(&canvas, &mesh, ay, ax, zoom, persp, cam); ob_clear(&out); emit_solid(&canvas, &out); }

        ssize_t wr = write(STDOUT_FILENO, out.buf, out.len); (void)wr;

        /* frame cap */
        double elapsed = now_sec() - t;
        double sleep_s = frame_dt - elapsed;
        if (sleep_s > 0) {
            struct timespec ts = { (time_t)sleep_s, (long)((sleep_s - (time_t)sleep_s) * 1e9) };
            nanosleep(&ts, NULL);
        }
    }

    term_restore();
    free(mesh.tri); free(canvas.fb); free(canvas.zb); free(canvas.dots); free(out.buf);
    return 0;
}
