/*
 * ascii_dither.c — JPEG/PNG → JPEG из ASCII-символов 16 ANSI-цветов
 *                  с контурной обводкой и логами прогресса.
 * Порт на C оригинального ascii_dither.py.
 *
 * Зависимости (single-header, public domain, лежат рядом):
 *   stb_image.h, stb_image_resize.h, stb_image_write.h, stb_truetype.h
 *
 * Сборка (MinGW/GCC/Clang):  gcc -O2 -std=c99 -o ascii_dither.exe ascii_dither.c -lm
 * Сборка (MSVC):             cl /O2 /std:c11 ascii_dither.c
 */

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ------------------------------------------------------------------ */
/* Логирование */
/* ------------------------------------------------------------------ */
static double g_t0 = 0.0;
static int g_quiet = 0;

static double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int init = 0;
    LARGE_INTEGER c;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void logmsg(const char *msg) {
    if (g_quiet) return;
    fprintf(stderr, "[%6.2fs] %s\n", now_sec() - g_t0, msg);
    fflush(stderr);
}

static void xlogf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    if (g_quiet) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logmsg(buf);
}

static int g_prog_last[8];
static const char *g_prog_labels[8];
static int g_prog_n = 0;

static int prog_id(const char *label) {
    int i;
    for (i = 0; i < g_prog_n; i++) if (strcmp(g_prog_labels[i], label) == 0) return i;
    if (g_prog_n < 8) { g_prog_labels[g_prog_n] = label; g_prog_last[g_prog_n] = -100; return g_prog_n++; }
    return 0;
}

static void progress(const char *label, int i, int total, int step_pct) {
    int pct, filled, nbar = 20, k, id;
    char bar[64];
    if (g_quiet) return;
    if (i > total) i = total;
    pct = (int)((long long)i * 100 / (total > 0 ? total : 1));
    id = prog_id(label);
    if (pct < g_prog_last[id] + step_pct && i < total) return;
    g_prog_last[id] = pct;
    filled = nbar * pct / 100;
    for (k = 0; k < nbar; k++) bar[k] = (k < filled) ? '#' : '.';
    bar[nbar] = 0;
    fprintf(stderr, "\r  %s: [%s] %3d%%", label, bar, pct);
    fflush(stderr);
    if (i >= total) fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Палитра / опции */
/* ------------------------------------------------------------------ */
typedef struct { unsigned char r,g,b; } RGB;

static const RGB PALETTE[16] = {
    {0,0,0},{128,0,0},{0,128,0},{128,128,0},
    {0,0,128},{128,0,128},{0,128,128},{192,192,192},
    {128,128,128},{255,0,0},{0,255,0},{255,255,0},
    {0,0,255},{255,0,255},{0,255,255},{255,255,255}
};

typedef struct { char g; double cov; } GlyphCov;
static const GlyphCov GLYPHS[] = {
    {' ',0.00},{'.',0.04},{':',0.08},{'-',0.12},{'=',0.18},
    {'+',0.20},{'*',0.22},{'#',0.30},{'%',0.38},{'M',0.40},{'@',0.50}
};
#define NGLYPHS ((int)(sizeof(GLYPHS)/sizeof(GLYPHS[0])))

typedef struct {
    int pi;
    char glyph;
    int k;
    double pr, pg, pb;
} Option;

static double color_dist2(double r1,double g1,double b1, double r2,double g2,double b2) {
    double dr=r1-r2, dg=g1-g2, db=b1-b2;
    return 0.299*dr*dr + 0.587*dg*dg + 0.114*db*db;
}

static int build_options(RGB bg, Option *out, int max_out) {
    int n = 0, pi, gi, kk;
    for (pi = 0; pi < 16; pi++) {
        for (gi = 0; gi < NGLYPHS; gi++) {
            double c = GLYPHS[gi].cov;
            if (c < 0.02) continue;
            for (kk = 1; kk <= 2; kk++) {
                double eff = c * kk * 0.85;
                if (eff > 0.90) eff = 0.90;
                if (n >= max_out) return n;
                out[n].pi = pi;
                out[n].glyph = GLYPHS[gi].g;
                out[n].k = kk;
                out[n].pr = eff*PALETTE[pi].r + (1.0-eff)*bg.r;
                out[n].pg = eff*PALETTE[pi].g + (1.0-eff)*bg.g;
                out[n].pb = eff*PALETTE[pi].b + (1.0-eff)*bg.b;
                n++;
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Дизеринг Флойда–Штейнберга */
/* ------------------------------------------------------------------ */
typedef struct { int pi; char glyph; int k; } Choice;

static void dither_image(const unsigned char *small, int w, int h, RGB bg, Choice *res) {
    Option opts[256];
    int nopts = build_options(bg, opts, 256);
    double (*err)[3] = (double(*)[3])calloc((size_t)w*h, sizeof(double[3]));
    int x, y, oi;
    if (!err) { fprintf(stderr, "out of memory\n"); exit(1); }
    (void)bg;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const unsigned char *p = small + (y*w + x)*3;
            double tr = p[0] + err[y*w+x][0];
            double tg = p[1] + err[y*w+x][1];
            double tb = p[2] + err[y*w+x][2];
            if (tr<0) tr=0; if (tr>255) tr=255;
            if (tg<0) tg=0; if (tg>255) tg=255;
            if (tb<0) tb=0; if (tb>255) tb=255;
            double tl = (0.299*tr + 0.587*tg + 0.114*tb) / 255.0;
            int max_k = (tl > 0.35) ? 2 : 1;
            int best = 0; double best_d = 1e30;
            for (oi = 0; oi < nopts; oi++) {
                double d;
                if (opts[oi].k > max_k) continue;
                d = color_dist2(tr,tg,tb, opts[oi].pr, opts[oi].pg, opts[oi].pb);
                if (d < best_d) { best_d = d; best = oi; }
            }
            res[y*w+x].pi = opts[best].pi;
            res[y*w+x].glyph = opts[best].glyph;
            res[y*w+x].k = opts[best].k;
            {
                double dr = tr - opts[best].pr;
                double dg = tg - opts[best].pg;
                double db = tb - opts[best].pb;
                const int dxv[4]={1,-1,0,1}, dyv[4]={0,1,1,1}, kv[4]={7,3,5,1};
                int t;
                for (t=0;t<4;t++){
                    int nx=x+dxv[t], ny=y+dyv[t];
                    if (nx>=0 && nx<w && ny>=0 && ny<h){
                        double f=kv[t]/16.0;
                        err[ny*w+nx][0]+=dr*f; err[ny*w+nx][1]+=dg*f; err[ny*w+nx][2]+=db*f;
                    }
                }
            }
        }
        progress("dithering", y + 1, h, 10);
    }
    free(err);
}

/* ------------------------------------------------------------------ */
/* Детекция краёв (Sobel) */
/* ------------------------------------------------------------------ */
static void detect_edges(const unsigned char *small, int w, int h, double *mag, double *ang) {
    double *gray = (double*)malloc((size_t)w*h*sizeof(double));
    int x, y;
    if (!gray) { fprintf(stderr, "out of memory\n"); exit(1); }
    for (y=0;y<h;y++) for (x=0;x<w;x++) {
        const unsigned char *p = small + (y*w+x)*3;
        gray[y*w+x] = (299.0*p[0] + 587.0*p[1] + 114.0*p[2]) / 1000.0;
    }
    for (y=0;y<h;y++) {
        if (y>=1 && y<h-1) {
            for (x=1;x<w-1;x++) {
                double gx = (gray[(y-1)*w+x+1] + 2*gray[y*w+x+1] + gray[(y+1)*w+x+1]
                           - gray[(y-1)*w+x-1] - 2*gray[y*w+x-1] - gray[(y+1)*w+x-1]);
                double gy = (gray[(y+1)*w+x-1] + 2*gray[(y+1)*w+x] + gray[(y+1)*w+x+1]
                           - gray[(y-1)*w+x-1] - 2*gray[(y-1)*w+x] - gray[(y-1)*w+x+1]);
                mag[y*w+x] = sqrt(gx*gx + gy*gy);
                { double a = atan2(gy,gx)*180.0/3.14159265358979323846; a=fmod(a,180.0); if(a<0)a+=180.0; ang[y*w+x]=a; }
            }
        }
        progress("Sobel edges", y + 1, h, 20);
    }
    free(gray);
}

static int edge_glyph(double angle_deg, double strength, char *out) {
    double a = angle_deg;
    if (a < 22.5 || a >= 157.5) { out[0]='|'; out[1]=0; return 1; }
    if (a < 67.5) { out[0]='/'; out[1]=0; return 1; }
    if (a < 112.5) { if (strength>1.6){out[0]='_';out[1]='_';out[2]=0;} else {out[0]='-';out[1]='-';out[2]=0;} return 1; }
    out[0]='\\'; out[1]=0; return 1;
}

/* ------------------------------------------------------------------ */
/* Шрифты / рендер текста (stb_truetype) */
/* ------------------------------------------------------------------ */
static unsigned char *read_file_all(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb");
    long n; unsigned char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf,1,(size_t)n,f)!=(size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f); if(sz) *sz=(size_t)n; return buf;
}

static unsigned char *g_font_buf = NULL;

static const char *font_candidates(void) {
    return NULL; /* handled in find_font */
}

static int load_font(stbtt_fontinfo *info, int font_size) {
    const char *cands[] = {
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        "C:/Windows/Fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf",
        "DejaVuSansMono.ttf",
        "arial.ttf",
        NULL
    };
    int i;
    for (i=0;cands[i];i++) {
        unsigned char *b = read_file_all(cands[i], NULL);
        if (b && stbtt_InitFont(info, b, stbtt_GetFontOffsetForIndex(b,0))) {
            g_font_buf = b;
            xlogf("  font: %s", cands[i]);
            (void)font_size;
            return 1;
        }
        free(b);
    }
    logmsg("  font: monospace font not found; trying stb_easy_font fallback not available, aborting text render setup");
    return 0;
}

typedef struct { unsigned char *bmp; int w,h,xoff,yoff; } GlyphBmp;

static void blit_glyph(unsigned char *canvas, int W2,int H2, const GlyphBmp *g, int px, int py, RGB col) {
    int y,x;
    if (!g || !g->bmp) return;
    for (y=0;y<g->h;y++) for (x=0;x<g->w;x++) {
        int cx = px + g->xoff + x;
        int cy = py + g->yoff + y;
        unsigned char a = g->bmp[y*g->w + x];
        if (a==0 || cx<0 || cy<0 || cx>=W2 || cy>=H2) continue;
        {
            unsigned char *d = canvas + (cy*W2 + cx)*3;
            /* alpha blend: out = a*col + (255-a)*dst */
            unsigned int ia = 255 - a;
            d[0] = (unsigned char)((a*col.r + ia*d[0] + 127)/255);
            d[1] = (unsigned char)((a*col.g + ia*d[1] + 127)/255);
            d[2] = (unsigned char)((a*col.b + ia*d[2] + 127)/255);
        }
    }
}

static double frand(double a, double b) { return a + (b-a) * ((double)rand() / ((double)RAND_MAX + 1.0)); }

/* ------------------------------------------------------------------ */
/* render */
/* ------------------------------------------------------------------ */
static void render_image(const char *inp, const char *outp, int font_size, double jitter,
                         double scale, int bg_is_white, unsigned int seed,
                         int use_edges, double edge_threshold) {
    int W,H,n, W2,H2, cell_h,cell_w, cw,ch, x,y;
    unsigned char *src = stbi_load(inp, &W, &H, &n, 3);
    unsigned char *small, *canvas;
    Choice *choices;
    RGB bg = bg_is_white ? (RGB){255,255,255} : (RGB){0,0,0};
    double *mag=NULL,*ang=NULL,*norm_mag=NULL;
    stbtt_fontinfo font;
    int have_font;
    /* glyph cache for ascii 32..126 */
    GlyphBmp cache[128];
    int ascent, descent, lineGap;

    srand(seed);
    xlogf("input file: %s", inp);
    if (!src) { xlogf("error: cannot open/decode image: %s", inp); exit(1); }
    xlogf("  source size: %dx%d", W, H);

    W2 = (int)(W*scale); if(W2<1) W2=1;
    H2 = (int)(H*scale); if(H2<1) H2=1;
    cell_h = font_size;
    cell_w = (int)floor(font_size * 0.6 + 0.5); if(cell_w<3) cell_w=3;
    cw = W2 / cell_w; if(cw<1) cw=1;
    ch = H2 / cell_h; if(ch<1) ch=1;
    xlogf("  canvas: %dx%d, cells: %dx%d (%dx%d px/cell, font %dpx, scale %.3f)", W2,H2,cw,ch,cell_w,cell_h,font_size,scale);
    if ((long long)W2*H2 > 25000000LL) xlogf("  ! large canvas %dx%d, render may take a while", W2,H2);

    small = (unsigned char*)malloc((size_t)cw*ch*3);
    if (!small) { fprintf(stderr,"out of memory\n"); exit(1); }
    stbir_resize_uint8(src, W,H, 0, small, cw,ch, 0, 3);
    logmsg("  image resized to cell grid");
    stbi_image_free(src);

    {
        Option opts[256]; int nopts = build_options(bg, opts, 256);
        xlogf("  palette colors: 16, paint variants: %d", nopts);
    }
    logmsg("  Floyd-Steinberg dithering...");
    choices = (Choice*)malloc((size_t)cw*ch*sizeof(Choice));
    if (!choices) { fprintf(stderr,"out of memory\n"); exit(1); }
    dither_image(small, cw, ch, bg, choices);

    if (use_edges) {
        int nedges = 0; double max_mag = 0.0;
        logmsg("  edge detection (Sobel)...");
        mag = (double*)calloc((size_t)cw*ch,sizeof(double));
        ang = (double*)calloc((size_t)cw*ch,sizeof(double));
        norm_mag = (double*)calloc((size_t)cw*ch,sizeof(double));
        if(!mag||!ang||!norm_mag){fprintf(stderr,"out of memory\n"); exit(1);}
        detect_edges(small,cw,ch,mag,ang);
        for (y=0;y<ch;y++) for(x=0;x<cw;x++) if(mag[y*cw+x]>max_mag) max_mag=mag[y*cw+x];
        if (max_mag <= 0.0) max_mag = 1.0;
        for (y=0;y<ch;y++) for(x=0;x<cw;x++) { norm_mag[y*cw+x]=mag[y*cw+x]/max_mag; if(norm_mag[y*cw+x]>edge_threshold) nedges++; }
        xlogf("  edge cells: %d (%.1f%%, threshold %.2f)", nedges, 100.0*nedges/(cw*ch>0?cw*ch:1), edge_threshold);
    } else {
        logmsg("  edges disabled (--no-edges)");
    }

    xlogf("  rendering glyphs (jitter=%.2f)...", jitter);
    canvas = (unsigned char*)malloc((size_t)W2*H2*3);
    if(!canvas){fprintf(stderr,"out of memory\n"); exit(1);}
    for (y=0;y<H2;y++) for(x=0;x<W2;x++){ unsigned char *d=canvas+(y*W2+x)*3; d[0]=bg.r; d[1]=bg.g; d[2]=bg.b; }

    memset(cache, 0, sizeof(cache));
    have_font = load_font(&font, font_size);
    if (have_font) {
        float sc = stbtt_ScaleForPixelHeight(&font, (float)font_size);
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
        /* pre-render needed glyphs */
        const char *need = " .:-=+*#%M@|/\\-_";
        int i;
        for (i=0; need[i]; i++) {
            int cp = (unsigned char)need[i]; GlyphBmp *gb = &cache[cp];
            if (gb->bmp) continue;
            gb->bmp = stbtt_GetCodepointBitmap(&font, 0, sc, cp, &gb->w,&gb->h,&gb->xoff,&gb->yoff);
            /* adjust yoff relative to baseline */
            gb->yoff += (int)(ascent * sc);
        }
        (void)lineGap; (void)descent;
    } else {
        fprintf(stderr, "Error: no TrueType font found. Install a monospace font (e.g. Consolas on Windows, DejaVu Sans Mono on Linux).\n");
        exit(1);
    }

    for (y=0;y<ch;y++) {
        for (x=0;x<cw;x++) {
            Choice *chc = &choices[y*cw+x];
            RGB color = PALETTE[chc->pi];
            int bx = x*cell_w, by = y*cell_h;
            if (use_edges && norm_mag[y*cw+x] > edge_threshold) {
                char eg[4]; edge_glyph(ang[y*cw+x], norm_mag[y*cw+x], eg);
                int dx = (int)(frand(-0.10,0.10)*cell_w);
                int dy = (int)(frand(-0.10,0.10)*cell_h);
                if (eg[0]=='_') dy -= cell_h/3;
                {
                    int ci; for (ci=0; eg[ci]; ci++) {
                        int cp = (unsigned char)eg[ci];
                        blit_glyph(canvas,W2,H2,&cache[cp], bx+dx + ci*(cell_w/2), by+dy, color);
                    }
                }
                continue;
            }
            if (chc->glyph == ' ') continue;
            {
                int rep; for (rep=0; rep<chc->k; rep++) {
                    int dx = (int)(frand(-jitter,jitter)*cell_w);
                    int dy = (int)(frand(-jitter,jitter)*cell_h);
                    blit_glyph(canvas,W2,H2,&cache[(unsigned char)chc->glyph], bx+dx, by+dy, color);
                }
            }
        }
        progress("render", y + 1, ch, 5);
    }

    {
        int i; for(i=0;i<128;i++) if(cache[i].bmp) stbtt_FreeBitmap(cache[i].bmp, NULL);
    }
    if (g_font_buf) { free(g_font_buf); g_font_buf=NULL; }

    xlogf("  saving to %s (JPEG quality=95)...", outp);
    if (!stbi_write_jpg(outp, W2,H2,3,canvas,95)) {
        fprintf(stderr, "Error: failed to write %s\n", outp); exit(1);
    }
    xlogf("done, %s", outp);

    free(canvas); free(choices); free(small);
    free(mag); free(ang); free(norm_mag);
}

/* ------------------------------------------------------------------ */
/* CLI */
/* ------------------------------------------------------------------ */
static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s <input> <output> [options]\n"
        "  -f, --font-size N       font size px (default: ~min(W,H)/40)\n"
        "  -j, --jitter F          position jitter 0..1 (0.40)\n"
        "  -e, --edge-threshold F  edge threshold 0..1 (0.12)\n"
        "  --no-edges              disable edge outlines\n"
        "  --scale K               output scale (1.0)\n"
        "  --bg {black,white}      background (black)\n"
        "  -s, --seed N            random seed (42)\n"
        "  -q, --quiet             suppress logs\n"
        "  -h, --help              help\n", argv0);
}

int main(int argc, char **argv) {
    const char *inp=NULL,*outp=NULL,*bg="black";
    int font_size = -1, no_edges=0, bg_white=0;
    double jitter=0.40, edge_threshold=0.12, scale=1.0;
    unsigned int seed=42;
    int i, W,H,n;
    g_t0 = now_sec();
    memset(g_prog_last, 0, sizeof(g_prog_last));

    for (i=1;i<argc;i++) {
        const char *a=argv[i];
        if (!strcmp(a,"-h")||!strcmp(a,"--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(a,"-q")||!strcmp(a,"--quiet")) g_quiet=1;
        else if (!strcmp(a,"--no-edges")) no_edges=1;
        else if ((!strcmp(a,"-f")||!strcmp(a,"--font-size")) && i+1<argc) font_size=atoi(argv[++i]);
        else if ((!strcmp(a,"-j")||!strcmp(a,"--jitter")) && i+1<argc) jitter=atof(argv[++i]);
        else if ((!strcmp(a,"-e")||!strcmp(a,"--edge-threshold")) && i+1<argc) edge_threshold=atof(argv[++i]);
        else if (!strcmp(a,"--scale") && i+1<argc) scale=atof(argv[++i]);
        else if ((!strcmp(a,"-s")||!strcmp(a,"--seed")) && i+1<argc) seed=(unsigned int)strtoul(argv[++i],NULL,10);
        else if (!strcmp(a,"--bg") && i+1<argc) bg=argv[++i];
        else if (a[0]=='-') { fprintf(stderr,"Unknown option: %s\n",a); usage(argv[0]); return 1; }
        else if (!inp) inp=a;
        else if (!outp) outp=a;
        else { fprintf(stderr,"Unexpected argument: %s\n",a); return 1; }
    }
    if (!inp || !outp) { usage(argv[0]); return 1; }
    bg_white = (strcmp(bg,"white")==0);

    if (!stbi_info(inp,&W,&H,&n)) { /* will load anyway, but try to get size for default font */ W=400; H=400; }
    if (font_size <= 0) { int m = W<H?W:H; font_size = m/40; if (font_size<8) font_size=8; }

    logmsg("============================================================");
    xlogf("ascii_dither: %s -> %s", inp, outp);
    xlogf("  font=%dpx, jitter=%.2f, edges=%s, scale=%.2f, bg=%s, seed=%u",
         font_size, jitter, no_edges?"off":"on(threshold)", scale, bg_white?"white":"black", seed);
    logmsg("============================================================");

    render_image(inp,outp,font_size,jitter,scale,bg_white,seed,!no_edges,edge_threshold);
    return 0;
}
