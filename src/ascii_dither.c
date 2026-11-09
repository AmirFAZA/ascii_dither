/*
 * ascii_dither.c - JPEG/PNG -> ASCII art in 16 ANSI colors.
 *
 * Pipeline: load image -> average it into a grid of character cells ->
 * Floyd-Steinberg dithering against the 16-color ANSI palette ->
 * Sobel edges pick directional glyphs -> glyphs are drawn with a TTF
 * monospace font -> JPEG (quality 95).
 *
 * Single-file build with CLI and Win32 GUI (GUI is Windows-only).
 * C port of ascii_dither.py (see legacy/).
 *
 * Dependencies (single-header, public domain / MIT): stb_image*.h and
 * stb_truetype.h, kept in src/.
 *
 * Build (MinGW-w64 / GCC):
 *   gcc -O2 -std=c99 -mwindows -static -static-libgcc -Isrc ^
 *       -o dist/ascii_dither.exe src/ascii_dither.c ^
 *       -lcomctl32 -lgdi32 -lcomdlg32 -luser32 -lshell32 -lm
 * or just run build.bat.
 */

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <float.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
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
/* Config / logging */
/* ------------------------------------------------------------------ */
typedef struct { unsigned char r,g,b; } Rgb8;

typedef enum { BG_AUTO=-1, BG_BLACK=0, BG_WHITE=1, BG_PALETTE=2 } BgMode;

typedef struct {
    const char *input;
    const char *output;
    int font_size;          /* <=0 => auto */
    double jitter;
    double edge_threshold;
    int no_edges;
    double scale;
    BgMode bg_mode;
    int bg_palette_index;   /* when BG_PALETTE */
    unsigned int seed;
    int quiet;
    int threads;            /* <=0 => auto */
    int max_dim;            /* >0: cap max(W2,H2) for preview speed */
} Config;

typedef void (*log_fn)(void *ud, const char *msg);
static log_fn g_log_cb = NULL;
static void *g_log_ud = NULL;
static int g_quiet = 0;
static double g_t0 = 0.0;
#ifdef _WIN32
static CRITICAL_SECTION g_log_cs;
static int g_log_cs_init = 0;
#endif

static double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq; static int init=0; LARGE_INTEGER c;
    if(!init){QueryPerformanceFrequency(&freq);init=1;}
    QueryPerformanceCounter(&c); return (double)c.QuadPart/(double)freq.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec/1e9;
#endif
}

static void log_emit(const char *msg) {
    char buf[1200];
    snprintf(buf,sizeof(buf),"[%6.2fs] %s", now_sec()-g_t0, msg);
#ifdef _WIN32
    if(g_log_cs_init) EnterCriticalSection(&g_log_cs);
#endif
    if (g_log_cb) g_log_cb(g_log_ud, buf);
    else if (!g_quiet) { fprintf(stderr, "%s\n", buf); fflush(stderr); }
#ifdef _WIN32
    if(g_log_cs_init) LeaveCriticalSection(&g_log_cs);
#endif
}

static void xlogf(const char *fmt, ...) {
    char msg[1024]; va_list ap;
    if (g_quiet && !g_log_cb) return;
    va_start(ap,fmt); vsnprintf(msg,sizeof(msg),fmt,ap); va_end(ap);
    log_emit(msg);
}

/* progress: keep simple (stderr only in CLI; GUI gets coarse messages) */
static void progress(const char *label, int i, int total, int step_pct) {
    static char last_label[64]; static int last_pct = -100;
    int pct, filled, k; char bar[24];
    if (g_quiet || g_log_cb) return;
    if(i>total)i=total; pct = total>0 ? (int)((long long)i*100/total) : 100;
    if (strcmp(label,last_label)!=0){ strncpy(last_label,label,63); last_label[63]=0; last_pct=-100; }
    if (pct < last_pct + step_pct && i < total) return;
    last_pct = pct; filled = 20*pct/100;
    for(k=0;k<20;k++) bar[k]=k<filled?'#':'.'; bar[20]=0;
    fprintf(stderr,"\r  %s: [%s] %3d%%", label, bar, pct);
    if(i>=total) fprintf(stderr,"\n");
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Palette / auto background */
/* ------------------------------------------------------------------ */
static const Rgb8 PALETTE[16] = {
    {0,0,0},{128,0,0},{0,128,0},{128,128,0},
    {0,0,128},{128,0,128},{0,128,128},{192,192,192},
    {128,128,128},{255,0,0},{0,255,0},{255,255,0},
    {0,0,255},{255,0,255},{0,255,255},{255,255,255}
};
static const char *PALETTE_NAMES[16] = {
    "black","maroon","green","olive","navy","purple","teal","silver",
    "gray","red","lime","yellow","blue","fuchsia","aqua","white"
};

static float color_dist2f(float r1,float g1,float b1,float r2,float g2,float b2){
    float dr=r1-r2,dg=g1-g2,db=b1-b2; return 0.299f*dr*dr+0.587f*dg*dg+0.114f*db*db;
}

static int pick_auto_bg(const unsigned char *img, int w, int h) {
    /* average color of image, then nearest ANSI palette entry */
    long long sr=0,sg=0,sb=0; int i,n=w*h, best=0; float bestd=1e30f, ar,ag,ab;
    if(n<=0) return 0;
    for(i=0;i<n;i++){ sr+=img[i*3]; sg+=img[i*3+1]; sb+=img[i*3+2]; }
    ar=(float)sr/n; ag=(float)sg/n; ab=(float)sb/n;
    for(i=0;i<16;i++){
        float d=color_dist2f(ar,ag,ab,(float)PALETTE[i].r,(float)PALETTE[i].g,(float)PALETTE[i].b);
        if(d<bestd){bestd=d;best=i;}
    }
    return best;
}

static Rgb8 resolve_bg(const Config *cfg, const unsigned char *small, int cw, int ch) {
    if (cfg->bg_mode == BG_BLACK) return (Rgb8){0,0,0};
    if (cfg->bg_mode == BG_WHITE) return (Rgb8){255,255,255};
    if (cfg->bg_mode == BG_PALETTE) {
        int idx = cfg->bg_palette_index; if(idx<0)idx=0; if(idx>15)idx=15; return PALETTE[idx];
    }
    /* auto */
    {
        int idx = pick_auto_bg(small,cw,ch);
        xlogf("  auto background: %s (ANSI %d, rgb %d,%d,%d)", PALETTE_NAMES[idx], idx, PALETTE[idx].r,PALETTE[idx].g,PALETTE[idx].b);
        return PALETTE[idx];
    }
}

/* ------------------------------------------------------------------ */
/* Dither options */
/* ------------------------------------------------------------------ */
typedef struct { char g; float cov; } GlyphCov;
static const GlyphCov GLYPHS[] = {
    {' ',0.00f},{'.',0.04f},{':',0.08f},{'-',0.12f},{'=',0.18f},
    {'+',0.20f},{'*',0.22f},{'#',0.30f},{'%',0.38f},{'M',0.40f},{'@',0.50f}
};
#define NGLYPHS ((int)(sizeof(GLYPHS)/sizeof(GLYPHS[0])))

typedef struct { int pi; char glyph; int k; float pr,pg,pb; } Option;
typedef struct { unsigned char pi; char glyph; unsigned char k; } Choice;

static int build_options(Rgb8 bg, Option *out, int max_out) {
    int n=0, pi, gi, kk;
    for(pi=0;pi<16;pi++) for(gi=0;gi<NGLYPHS;gi++){
        float c=GLYPHS[gi].cov; if(c<0.02f) continue;
        for(kk=1;kk<=2;kk++){
            float eff=c*kk*0.85f; if(eff>0.90f) eff=0.90f;
            if(n>=max_out) return n;
            out[n].pi=pi; out[n].glyph=GLYPHS[gi].g; out[n].k=kk;
            out[n].pr=eff*PALETTE[pi].r+(1.0f-eff)*bg.r;
            out[n].pg=eff*PALETTE[pi].g+(1.0f-eff)*bg.g;
            out[n].pb=eff*PALETTE[pi].b+(1.0f-eff)*bg.b;
            n++;
        }
    }
    return n;
}

/* Floyd-Steinberg is sequential by nature; keep single-threaded but tighter */
static void dither_image(const unsigned char *small, int w, int h, Rgb8 bg, Choice *res) {
    Option opts[256]; int nopts = build_options(bg, opts, 256);
    float (*err)[3] = (float(*)[3])calloc((size_t)w*h, sizeof(float[3]));
    int x,y,oi;
    if(!err){xlogf("out of memory"); return;}
    for(y=0;y<h;y++){
        for(x=0;x<w;x++){
            const unsigned char *p = small + (y*w+x)*3;
            float tr=p[0]+err[y*w+x][0], tg=p[1]+err[y*w+x][1], tb=p[2]+err[y*w+x][2];
            if(tr<0)tr=0; if(tr>255)tr=255; if(tg<0)tg=0; if(tg>255)tg=255; if(tb<0)tb=0; if(tb>255)tb=255;
            float tl=(0.299f*tr+0.587f*tg+0.114f*tb)/255.0f;
            int max_k = (tl>0.35f)?2:1, best=0; float best_d=FLT_MAX;
            for(oi=0;oi<nopts;oi++){
                float d; if(opts[oi].k>max_k) continue;
                d=color_dist2f(tr,tg,tb,opts[oi].pr,opts[oi].pg,opts[oi].pb);
                if(d<best_d){best_d=d;best=oi;}
            }
            res[y*w+x].pi=(unsigned char)opts[best].pi; res[y*w+x].glyph=opts[best].glyph; res[y*w+x].k=(unsigned char)opts[best].k;
            {
                float dr=tr-opts[best].pr,dg=tg-opts[best].pg,db=tb-opts[best].pb;
                if(x+1<w){ err[y*w+x+1][0]+=dr*(7.f/16.f); err[y*w+x+1][1]+=dg*(7.f/16.f); err[y*w+x+1][2]+=db*(7.f/16.f); }
                if(y+1<h){
                    if(x>0){ err[(y+1)*w+x-1][0]+=dr*(3.f/16.f); err[(y+1)*w+x-1][1]+=dg*(3.f/16.f); err[(y+1)*w+x-1][2]+=db*(3.f/16.f); }
                    err[(y+1)*w+x][0]+=dr*(5.f/16.f); err[(y+1)*w+x][1]+=dg*(5.f/16.f); err[(y+1)*w+x][2]+=db*(5.f/16.f);
                    if(x+1<w){ err[(y+1)*w+x+1][0]+=dr*(1.f/16.f); err[(y+1)*w+x+1][1]+=dg*(1.f/16.f); err[(y+1)*w+x+1][2]+=db*(1.f/16.f); }
                }
            }
        }
        progress("dithering", y+1, h, 10);
    }
    free(err);
}

/* ------------------------------------------------------------------ */
/* Sobel (parallel) */
/* ------------------------------------------------------------------ */
typedef struct { const unsigned char *small; float *gray; int w,h,y0,y1; } GrayJob;
#ifdef _WIN32
static unsigned __stdcall gray_worker(void *p){ GrayJob *j=(GrayJob*)p; int x,y;
    for(y=j->y0;y<j->y1;y++) for(x=0;x<j->w;x++){ const unsigned char *q=j->small+(y*j->w+x)*3; j->gray[y*j->w+x]=(299.f*q[0]+587.f*q[1]+114.f*q[2])/1000.f; }
    return 0; }
#else
static void *gray_worker(void *p){ GrayJob *j=(GrayJob*)p; int x,y;
    for(y=j->y0;y<j->y1;y++) for(x=0;x<j->w;x++){ const unsigned char *q=j->small+(y*j->w+x)*3; j->gray[y*j->w+x]=(299.f*q[0]+587.f*q[1]+114.f*q[2])/1000.f; }
    return NULL; }
#endif

typedef struct { const float *gray; float *mag,*ang; int w,h,y0,y1; } SobelJob;
#ifdef _WIN32
static unsigned __stdcall sobel_worker(void *p){ SobelJob *j=(SobelJob*)p; int x,y;
#else
static void *sobel_worker(void *p){ SobelJob *j=(SobelJob*)p; int x,y;
#endif
    for(y=j->y0;y<j->y1;y++){
        if(y>=1 && y<j->h-1){
            for(x=1;x<j->w-1;x++){
                float gx=(j->gray[(y-1)*j->w+x+1]+2.f*j->gray[y*j->w+x+1]+j->gray[(y+1)*j->w+x+1]
                         -j->gray[(y-1)*j->w+x-1]-2.f*j->gray[y*j->w+x-1]-j->gray[(y+1)*j->w+x-1]);
                float gy=(j->gray[(y+1)*j->w+x-1]+2.f*j->gray[(y+1)*j->w+x]+j->gray[(y+1)*j->w+x+1]
                         -j->gray[(y-1)*j->w+x-1]-2.f*j->gray[(y-1)*j->w+x]-j->gray[(y-1)*j->w+x+1]);
                j->mag[y*j->w+x]=sqrtf(gx*gx+gy*gy);
                { float a=atan2f(gy,gx)*180.f/3.14159265358979323846f; a=fmodf(a,180.f); if(a<0)a+=180.f; j->ang[y*j->w+x]=a; }
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int get_nthreads(int requested) {
    if (requested > 0) return requested;
#ifdef _WIN32
    { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1; }
#else
    { long n=sysconf(_SC_NPROCESSORS_ONLN); return n>0?(int)n:1; }
#endif
}

static void run_jobs(void *(*fn)(void*), void *jobs, int job_size, int n, int nthreads) {
#ifdef _WIN32
    /* use _beginthreadex signature wrapper via cast pattern: jobs already match; we spawn threads */
    HANDLE th[64]; unsigned tids[64]; int i, cnt=n<nthreads?n:nthreads; if(cnt>64)cnt=64; if(cnt<1)cnt=1;
    /* if more jobs than threads, do simple batching by reusing? easier: cap jobs to threads by merging - but jobs are pre-split */
    for(i=0;i<n && i<64;i++) th[i]=(HANDLE)_beginthreadex(NULL,0,(unsigned(__stdcall*)(void*))fn,(char*)jobs+i*job_size,0,&tids[i]);
    for(i=0;i<n && i<64;i++){ if(th[i]){ WaitForSingleObject(th[i],INFINITE); CloseHandle(th[i]); } }
#else
    pthread_t th[64]; int i; for(i=0;i<n && i<64;i++) pthread_create(&th[i],NULL,fn,(char*)jobs+i*job_size);
    for(i=0;i<n && i<64;i++) pthread_join(th[i],NULL);
#endif
    (void)nthreads;
}

static void detect_edges_mt(const unsigned char *small, int w, int h, float *mag, float *ang, int nthreads) {
    float *gray = (float*)calloc((size_t)w*h,sizeof(float));
    int i,nj = nthreads<1?1:nthreads; if(nj>64)nj=64; if(nj>h)nj=h;
    if(!gray){xlogf("out of memory");return;}
    {
        GrayJob gj[64]; SobelJob sj[64];
        for(i=0;i<nj;i++){ gj[i].small=small; gj[i].gray=gray; gj[i].w=w; gj[i].h=h; gj[i].y0=i*h/nj; gj[i].y1=(i+1)*h/nj; }
        run_jobs((void*(*)(void*))gray_worker, gj, sizeof(GrayJob), nj, nj);
        for(i=0;i<nj;i++){ sj[i].gray=gray; sj[i].mag=mag; sj[i].ang=ang; sj[i].w=w; sj[i].h=h; sj[i].y0=i*h/nj; sj[i].y1=(i+1)*h/nj; }
        run_jobs((void*(*)(void*))sobel_worker, sj, sizeof(SobelJob), nj, nj);
    }
    xlogf("  edges (Sobel, %d threads): done", nj);
    free(gray);
}

static int edge_glyph(float angle_deg, float strength, char *out) {
    float a=angle_deg;
    if(a<22.5f||a>=157.5f){out[0]='|';out[1]=0;return 1;}
    if(a<67.5f){out[0]='/';out[1]=0;return 1;}
    if(a<112.5f){ if(strength>1.6f){out[0]='_';out[1]='_';out[2]=0;} else {out[0]='-';out[1]='-';out[2]=0;} return 1; }
    out[0]='\\'; out[1]=0; return 1;
}

/* ------------------------------------------------------------------ */
/* Font / render (parallel, deterministic jitter) */
/* ------------------------------------------------------------------ */
static unsigned char *read_file_all(const char *path, size_t *sz){
    FILE *f=fopen(path,"rb"); long n; unsigned char *buf;
    if(!f) return NULL; fseek(f,0,SEEK_END); n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<=0){fclose(f);return NULL;} buf=(unsigned char*)malloc((size_t)n);
    if(!buf){fclose(f);return NULL;} if(fread(buf,1,(size_t)n,f)!=(size_t)n){free(buf);fclose(f);return NULL;}
    fclose(f); if(sz)*sz=(size_t)n; return buf;
}

static int load_font(stbtt_fontinfo *info, unsigned char **font_buf) {
    const char *cands[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/consola.ttf","C:/Windows/Fonts/cour.ttf","C:/Windows/Fonts/lucon.ttf","C:/Windows/Fonts/DejaVuSansMono.ttf",
#endif
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf","/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf","/System/Library/Fonts/Menlo.ttc","/System/Library/Fonts/Monaco.ttf",
        "DejaVuSansMono.ttf","arial.ttf",NULL
    };
    int i; for(i=0;cands[i];i++){
        unsigned char *b=read_file_all(cands[i],NULL);
        if(b && stbtt_InitFont(info,b,stbtt_GetFontOffsetForIndex(b,0))){ *font_buf=b; xlogf("  font: %s", cands[i]); return 1; }
        free(b);
    }
    xlogf("  font: monospace TrueType font not found"); return 0;
}

typedef struct { unsigned char *bmp; int w,h,xoff,yoff; } GlyphBmp;

static inline void blit_glyph(unsigned char *canvas, int W2,int H2, const GlyphBmp *g, int px,int py, Rgb8 col){
    int y,x; if(!g||!g->bmp) return;
    for(y=0;y<g->h;y++){
        int cy=py+g->yoff+y; if(cy<0||cy>=H2) continue;
        for(x=0;x<g->w;x++){
            int cx=px+g->xoff+x; unsigned char a=g->bmp[y*g->w+x];
            if(a && cx>=0 && cx<W2){
                unsigned char *d=canvas+(cy*W2+cx)*3; unsigned int ia=255-a;
                d[0]=(unsigned char)((a*col.r+ia*d[0]+127)/255);
                d[1]=(unsigned char)((a*col.g+ia*d[1]+127)/255);
                d[2]=(unsigned char)((a*col.b+ia*d[2]+127)/255);
            }
        }
    }
}

/* deterministic jitter: xorshift32 seeded from (seed,cy,cx,rep) */
static uint32_t hash_u32(uint32_t x){ x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
static float djitter(unsigned int seed,int cy,int cx,int rep,int axis){
    uint32_t v=hash_u32((uint32_t)seed ^ hash_u32((uint32_t)cy*374761393u + (uint32_t)cx*668265263u + (uint32_t)rep*2246822519u + (uint32_t)axis*3266489917u));
    return (v / 4294967296.0f); /* [0,1) */
}
static float djitter_sym(unsigned int seed,int cy,int cx,int rep,int axis){ return djitter(seed,cy,cx,rep,axis)*2.0f - 1.0f; }

typedef struct {
    unsigned char *canvas; int W2,H2,cw,ch,cell_w,cell_h;
    const Choice *choices; const float *norm_mag; const float *ang;
    float edge_threshold,jitter; int use_edges; unsigned int seed;
    const GlyphBmp *cache; int y0,y1;
} RenderJob;

#ifdef _WIN32
static unsigned __stdcall render_worker(void *p){
#else
static void *render_worker(void *p){
#endif
    RenderJob *j=(RenderJob*)p; int cy,cx;
    for(cy=j->y0;cy<j->y1;cy++) for(cx=0;cx<j->cw;cx++){
        const Choice *chc=&j->choices[cy*j->cw+cx]; Rgb8 color=PALETTE[chc->pi];
        int bx=cx*j->cell_w, by=cy*j->cell_h;
        if(j->use_edges && j->norm_mag[cy*j->cw+cx] > j->edge_threshold){
            char eg[4]; edge_glyph(j->ang[cy*j->cw+cx], j->norm_mag[cy*j->cw+cx], eg);
            int dx=(int)(djitter_sym(j->seed,cy,cx,99,0)*0.10f*j->cell_w);
            int dy=(int)(djitter_sym(j->seed,cy,cx,99,1)*0.10f*j->cell_h);
            if(eg[0]=='_') dy -= j->cell_h/3;
            { int ci; for(ci=0;eg[ci];ci++) blit_glyph(j->canvas,j->W2,j->H2,&j->cache[(unsigned char)eg[ci]], bx+dx+ci*(j->cell_w/2), by+dy, color); }
            continue;
        }
        if(chc->glyph==' ') continue;
        { int rep; for(rep=0;rep<chc->k;rep++){
            int dx=(int)(djitter_sym(j->seed,cy,cx,rep,0)*j->jitter*j->cell_w);
            int dy=(int)(djitter_sym(j->seed,cy,cx,rep,1)*j->jitter*j->cell_h);
            blit_glyph(j->canvas,j->W2,j->H2,&j->cache[(unsigned char)chc->glyph], bx+dx, by+dy, color);
        } }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Core processing */
/* ------------------------------------------------------------------ */
static int render_to_buffer(const Config *cfg, unsigned char **out_rgb, int *out_w, int *out_h,
                            int *out_exp_w, int *out_exp_h, char *errbuf, size_t errsz) {
    int W,H,n,W2,H2,cell_h,cell_w,cw,ch,x,y,nthreads;
    unsigned char *src,*small,*canvas=NULL; Choice *choices=NULL;
    float *mag=NULL,*ang=NULL,*norm_mag=NULL; Rgb8 bg;
    stbtt_fontinfo font; unsigned char *font_buf=NULL; GlyphBmp cache[128]; int ascent,descent,lineGap;
    double scale = cfg->scale;

    *out_rgb=NULL; *out_w=0; *out_h=0;
    if(out_exp_w) *out_exp_w=0; if(out_exp_h) *out_exp_h=0;
    nthreads = get_nthreads(cfg->threads);
    g_quiet = cfg->quiet;
    if(cfg->max_dim <= 0) g_t0 = now_sec();
    xlogf("input: %s", cfg->input ? cfg->input : "(null)");

    src = stbi_load(cfg->input,&W,&H,&n,3);
    if(!src){ snprintf(errbuf,errsz,"cannot open/decode image: %s", cfg->input); return 0; }
    xlogf("  source: %dx%d", W,H);

    W2=(int)(W*scale); H2=(int)(H*scale); if(W2<1)W2=1; if(H2<1)H2=1;
    if(out_exp_w) *out_exp_w=W2; if(out_exp_h) *out_exp_h=H2;
    if(cfg->max_dim > 0 && (W2>cfg->max_dim || H2>cfg->max_dim)){
        double s = (double)cfg->max_dim / (W2>H2?W2:H2);
        W2=(int)(W2*s); H2=(int)(H2*s); if(W2<1)W2=1; if(H2<1)H2=1;
    }
    cell_h = cfg->font_size>0 ? cfg->font_size : ((W<H?W:H)/40); if(cell_h<8)cell_h=8;
    if(cfg->max_dim > 0){ /* scale font to preview: more scale -> denser cells */
        double s2 = (double)W2 / (W*scale>0?W*scale:W); if(s2>0 && s2<1){ cell_h=(int)(cell_h*s2); if(cell_h<4)cell_h=4; }
    }
    cell_w = (int)floorf(cell_h*0.6f+0.5f); if(cell_w<2)cell_w=2;
    cw=W2/cell_w; if(cw<1)cw=1; ch=H2/cell_h; if(ch<1)ch=1;
    xlogf("  canvas %dx%d, cells %dx%d (%dx%d px), threads %d", W2,H2,cw,ch,cell_w,cell_h,nthreads);
    if(cfg->max_dim > 0 && out_exp_w && *out_exp_w>0)
        xlogf("  export size: %dx%d (scale %.2f)", *out_exp_w,*out_exp_h,scale);

    small=(unsigned char*)malloc((size_t)cw*ch*3); if(!small){snprintf(errbuf,errsz,"out of memory"); stbi_image_free(src); return 0;}
    stbir_resize_uint8(src,W,H,0,small,cw,ch,0,3); stbi_image_free(src);

    bg = resolve_bg(cfg, small, cw, ch);
    choices=(Choice*)malloc((size_t)cw*ch*sizeof(Choice)); if(!choices){snprintf(errbuf,errsz,"out of memory"); free(small); return 0;}
    xlogf("  dithering...");
    dither_image(small,cw,ch,bg,choices);

    if(!cfg->no_edges){
        float max_mag=0.f; mag=(float*)calloc((size_t)cw*ch,sizeof(float)); ang=(float*)calloc((size_t)cw*ch,sizeof(float)); norm_mag=(float*)calloc((size_t)cw*ch,sizeof(float));
        if(!mag||!ang||!norm_mag){snprintf(errbuf,errsz,"out of memory"); goto fail;}
        detect_edges_mt(small,cw,ch,mag,ang,nthreads);
        for(y=0;y<ch;y++) for(x=0;x<cw;x++) if(mag[y*cw+x]>max_mag) max_mag=mag[y*cw+x];
        if(max_mag<=0.f) max_mag=1.f;
        for(y=0;y<ch;y++) for(x=0;x<cw;x++) norm_mag[y*cw+x]=mag[y*cw+x]/max_mag;
    }
    free(small); small=NULL;

    canvas=(unsigned char*)malloc((size_t)W2*H2*3); if(!canvas){snprintf(errbuf,errsz,"out of memory"); goto fail;}
    for(y=0;y<H2;y++) for(x=0;x<W2;x++){ unsigned char *d=canvas+(y*W2+x)*3; d[0]=bg.r; d[1]=bg.g; d[2]=bg.b; }

    memset(cache,0,sizeof(cache));
    if(!load_font(&font,&font_buf)){ snprintf(errbuf,errsz,"no TrueType monospace font found"); goto fail; }
    { float sc=stbtt_ScaleForPixelHeight(&font,(float)cell_h); const char *need=" .:-=+*#%M@|/\\-_"; int i;
      stbtt_GetFontVMetrics(&font,&ascent,&descent,&lineGap); (void)descent;(void)lineGap;
      for(i=0;need[i];i++){ int cp=(unsigned char)need[i]; GlyphBmp *gb=&cache[cp]; gb->bmp=stbtt_GetCodepointBitmap(&font,0,sc,cp,&gb->w,&gb->h,&gb->xoff,&gb->yoff); gb->yoff += (int)(ascent*sc); } }

    xlogf("  rendering...");
    { RenderJob rj[64]; int nj=nthreads; if(nj>64)nj=64; if(nj>ch)nj=ch; if(nj<1)nj=1;
      for(y=0;y<nj;y++){ rj[y].canvas=canvas; rj[y].W2=W2; rj[y].H2=H2; rj[y].cw=cw; rj[y].ch=ch; rj[y].cell_w=cell_w; rj[y].cell_h=cell_h;
        rj[y].choices=choices; rj[y].norm_mag=norm_mag; rj[y].ang=ang; rj[y].edge_threshold=(float)cfg->edge_threshold;
        rj[y].jitter=(float)cfg->jitter; rj[y].use_edges=!cfg->no_edges; rj[y].seed=cfg->seed; rj[y].cache=cache; rj[y].y0=y*ch/nj; rj[y].y1=(y+1)*ch/nj; }
      run_jobs((void*(*)(void*))render_worker, rj, sizeof(RenderJob), nj, nj); }

    { int i; for(i=0;i<128;i++) if(cache[i].bmp) stbtt_FreeBitmap(cache[i].bmp,NULL); }
    free(font_buf); free(choices); free(mag); free(ang); free(norm_mag);
    *out_rgb=canvas; *out_w=W2; *out_h=H2; xlogf("  render done %dx%d", W2,H2); return 1;
fail:
    { int i; for(i=0;i<128;i++) if(cache[i].bmp) stbtt_FreeBitmap(cache[i].bmp,NULL); }
    free(font_buf); free(canvas); free(choices); free(small); free(mag); free(ang); free(norm_mag); return 0;
}

static int process_image(const Config *cfg, char *errbuf, size_t errsz) {
    unsigned char *rgb=NULL; int w=0,h=0;
    if(!render_to_buffer(cfg,&rgb,&w,&h,NULL,NULL,errbuf,errsz)) return 0;
    xlogf("  saving %s (JPEG q95)...", cfg->output);
    if(!stbi_write_jpg(cfg->output,w,h,3,rgb,95)){ snprintf(errbuf,errsz,"failed to write: %s", cfg->output); free(rgb); return 0; }
    xlogf("done: %s", cfg->output); free(rgb); return 1;
}

/* ------------------------------------------------------------------ */
/* CLI */
/* ------------------------------------------------------------------ */
static void usage(const char *a0){
    fprintf(stderr,
        "Usage: %s <input> <output> [options]\n"
        "       %s --gui              (open GUI)\n"
        "  -f, --font-size N       font size px (default auto)\n"
        "  -j, --jitter F          jitter 0..1 (0.40)\n"
        "  -e, --edge-threshold F  edge threshold 0..1 (0.12)\n"
        "  --no-edges              disable edges\n"
        "  --scale K               output scale (1.0)\n"
        "  --bg MODE               auto|black|white|0-15 (default auto)\n"
        "  -s, --seed N            random seed (42)\n"
        "  -t, --threads N         threads (default auto)\n"
        "  -q, --quiet             suppress logs\n"
        "  -h, --help              help\n", a0,a0);
}

static int parse_bg(const char *s, Config *c){
    if(!s) return 0;
    if(strcmp(s,"auto")==0){ c->bg_mode=BG_AUTO; return 1; }
    if(strcmp(s,"black")==0){ c->bg_mode=BG_BLACK; return 1; }
    if(strcmp(s,"white")==0){ c->bg_mode=BG_WHITE; return 1; }
    if(s[0]>='0'&&s[0]<='9'){ c->bg_mode=BG_PALETTE; c->bg_palette_index=atoi(s); return 1; }
    return 0;
}

static int run_cli(int argc, char **argv) {
    Config cfg; char err[512]; int i;
    memset(&cfg,0,sizeof(cfg)); cfg.jitter=0.40; cfg.edge_threshold=0.12; cfg.scale=1.0; cfg.bg_mode=BG_AUTO; cfg.seed=42;
    for(i=1;i<argc;i++){
        const char *a=argv[i];
        if(!strcmp(a,"-h")||!strcmp(a,"--help")){usage(argv[0]); return 0;}
        else if(!strcmp(a,"--gui")) return -2; /* signal GUI */
        else if(!strcmp(a,"-q")||!strcmp(a,"--quiet")) cfg.quiet=1;
        else if(!strcmp(a,"--no-edges")) cfg.no_edges=1;
        else if((!strcmp(a,"-f")||!strcmp(a,"--font-size"))&&i+1<argc) cfg.font_size=atoi(argv[++i]);
        else if((!strcmp(a,"-j")||!strcmp(a,"--jitter"))&&i+1<argc) cfg.jitter=atof(argv[++i]);
        else if((!strcmp(a,"-e")||!strcmp(a,"--edge-threshold"))&&i+1<argc) cfg.edge_threshold=atof(argv[++i]);
        else if(!strcmp(a,"--scale")&&i+1<argc) cfg.scale=atof(argv[++i]);
        else if((!strcmp(a,"-s")||!strcmp(a,"--seed"))&&i+1<argc) cfg.seed=(unsigned int)strtoul(argv[++i],NULL,10);
        else if((!strcmp(a,"-t")||!strcmp(a,"--threads"))&&i+1<argc) cfg.threads=atoi(argv[++i]);
        else if(!strcmp(a,"--bg")&&i+1<argc){ if(!parse_bg(argv[++i],&cfg)){ fprintf(stderr,"bad --bg\n"); return 1; } }
        else if(a[0]=='-'){ fprintf(stderr,"Unknown option: %s\n",a); usage(argv[0]); return 1; }
        else if(!cfg.input) cfg.input=a; else if(!cfg.output) cfg.output=a;
        else { fprintf(stderr,"Unexpected arg: %s\n",a); return 1; }
    }
    if(!cfg.input||!cfg.output){ usage(argv[0]); return 1; }
    g_quiet = cfg.quiet; g_t0 = now_sec();
    log_emit("============================================================");
    xlogf("ascii_dither: %s -> %s", cfg.input,cfg.output);
    log_emit("============================================================");
    if(!process_image(&cfg,err,sizeof(err))){ fprintf(stderr,"Error: %s\n",err); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* GUI v5: settings right, source thumb above settings, fixed result   */
/* preview (correct resolution) with hover zoom; auto-render/export.   */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#define RPW 300
#define THUMBH 160
#define PREVW 800
#define PREVH 660

enum { ID_IN=101,ID_OUT,ID_BIN,ID_BOUT,ID_BG,ID_NOEDGE,ID_EXPORT,ID_LOG,ID_PV_SRC,ID_PV_RES,
       ID_SL_FONT=120,ID_SL_JIT,ID_SL_EDGE,ID_SL_SCALE,ID_SL_SEED,ID_SL_THR,
       ID_ED_FONT=130,ID_ED_JIT,ID_ED_EDGE,ID_ED_SCALE,ID_ED_SEED,ID_ED_THR, IDT_DEBOUNCE=1 };
enum { WM_APPLOG=WM_APP+1, WM_APPPREV=WM_APP+2, WM_APPEXPORT=WM_APP+3 };

static HWND g_hWnd,g_hIn,g_hOut,g_hLog,g_hBg,g_hNoEdge,g_hExport,g_pvSrc,g_pvRes;
static HWND g_sl[6],g_ed[6];
static unsigned char *g_srcBgr=NULL,*g_resBgr=NULL;
static int g_srcW=0,g_srcH=0,g_resW=0,g_resH=0;
static int g_resExpW=0,g_resExpH=0;
static char g_srcPath[MAX_PATH]="";
static int g_upd=0, g_hover=0, g_hx=0, g_hy=0;

static void to_bgr(const unsigned char *rgb,unsigned char *bgr,int n){ int i; for(i=0;i<n;i++){ bgr[i*3]=rgb[i*3+2]; bgr[i*3+1]=rgb[i*3+1]; bgr[i*3+2]=rgb[i*3+0]; } }

static void blit_bgr(HDC dc,int dx,int dy,int dw,int dh,const unsigned char *bgr,int sw,int sh,int sx0,int sy0,int sw0,int sh0){
    /* DIB scanlines must be DWORD-aligned. Source buffer is packed (stride=sw*3). */
    BITMAPINFO bi; int src_stride,dib_stride,dib_w,dib_h,y; unsigned char *dib;
    if(!bgr||sw<=0||sh<=0||dw<=0||dh<=0) return;
    if(sw0<=0) sw0=sw-sx0; if(sh0<=0) sh0=sh-sy0;
    if(sx0<0)sx0=0; if(sy0<0)sy0=0; if(sx0+sw0>sw) sw0=sw-sx0; if(sy0+sh0>sh) sh0=sh-sy0; if(sw0<=0||sh0<=0) return;
    dib_w=sw0; dib_h=sh0;
    src_stride=sw*3;
    dib_stride=((dib_w*3 + 3) & ~3);
    dib=(unsigned char*)malloc((size_t)dib_stride*dib_h);
    if(!dib) return;
    for(y=0;y<dib_h;y++){
        memcpy(dib + (size_t)y*dib_stride, bgr + (size_t)(sy0+y)*src_stride + sx0*3, (size_t)dib_w*3);
        if(dib_stride > dib_w*3) memset(dib + (size_t)y*dib_stride + dib_w*3, 0, (size_t)(dib_stride - dib_w*3));
    }
    ZeroMemory(&bi,sizeof(bi)); bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=dib_w; bi.bmiHeader.biHeight=-dib_h; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=24; bi.bmiHeader.biCompression=BI_RGB;
    SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc,0,0,NULL);
    StretchDIBits(dc,dx,dy,dw,dh,0,0,dib_w,dib_h,dib,&bi,DIB_RGB_COLORS,SRCCOPY);
    free(dib);
}

static void fit_rect(int pw,int ph,int sw,int sh,int *dw,int *dh,int *ox,int *oy){
    double a=sh>0?(double)sw/sh:1.0, ap=ph>0?(double)pw/ph:1.0;
    if(a>ap){ *dw=pw; *dh=(int)(pw/a+0.5); } else { *dh=ph; *dw=(int)(ph*a+0.5); }
    if(*dw<1)*dw=1; if(*dh<1)*dh=1; *ox=(pw-*dw)/2; *oy=(ph-*dh)/2;
}

static void draw_thumb(HDC h,const RECT *rc,const unsigned char *bgr,int sw,int sh){
    HBRUSH bg=CreateSolidBrush(RGB(36,36,40)); RECT fr=*rc; FillRect(h,rc,bg); DeleteObject(bg);
    InflateRect(&fr,-1,-1);
    if(bgr&&sw>0&&sh>0){ int dw,dh,ox,oy,pw=fr.right-fr.left,ph=fr.bottom-fr.top; fit_rect(pw,ph,sw,sh,&dw,&dh,&ox,&oy); blit_bgr(h,fr.left+ox,fr.top+oy,dw,dh,bgr,sw,sh,0,0,sw,sh); }
    { HPEN pen=CreatePen(PS_SOLID,1,RGB(80,80,90)); HGDIOBJ op=SelectObject(h,pen), ob=SelectObject(h,GetStockObject(NULL_BRUSH)); Rectangle(h,rc->left,rc->top,rc->right,rc->bottom); SelectObject(h,op); SelectObject(h,ob); DeleteObject(pen); }
    SetBkMode(h,TRANSPARENT); SetTextColor(h,RGB(190,190,200)); TextOutA(h,rc->left+5,rc->top+4,"Source",6);
}

static void draw_result(HDC h,const RECT *rc,const unsigned char *bgr,int sw,int sh,int hover,int mx,int my){
    HBRUSH bg=CreateSolidBrush(RGB(24,24,28)); FillRect(h,rc,bg); DeleteObject(bg);
    if(!bgr||sw<=0||sh<=0){ SetBkMode(h,TRANSPARENT); SetTextColor(h,RGB(140,140,150)); TextOutA(h,rc->left+12,rc->top+12,"Result preview",14); return; }
    {
        /* fixed display box centered in rc */
        int pw=rc->right-rc->left, ph=rc->bottom-rc->top;
        int boxw=PREVW, boxh=PREVH, bx,by,dw,dh,ox,oy;
        if(boxw>pw-20) boxw=pw-20; if(boxh>ph-20) boxh=ph-20; if(boxw<50) boxw=pw; if(boxh<50) boxh=ph;
        bx=rc->left+(pw-boxw)/2; by=rc->top+(ph-boxh)/2;
        /* draw frame */
        { HBRUSH fb=CreateSolidBrush(RGB(18,18,20)); RECT br={bx,by,bx+boxw,by+boxh}; FillRect(h,&br,fb); DeleteObject(fb); }
        /* fit image into box at correct resolution (1:1 if fits, else fit) */
        if(sw<=boxw && sh<=boxh){ dw=sw; dh=sh; } else fit_rect(boxw,boxh,sw,sh,&dw,&dh,&ox,&oy);
        if(!(sw<=boxw && sh<=boxh)){ fit_rect(boxw,boxh,sw,sh,&dw,&dh,&ox,&oy); } else { ox=(boxw-dw)/2; oy=(boxh-dh)/2; }
        if(hover && mx>=bx+ox && mx<bx+ox+dw && my>=by+oy && my<by+oy+dh){
            double z=2.8, sx=(double)sw/dw, sy=(double)sh/dh;
            int vw=(int)(dw/(z)); int vh=(int)(dh/(z)); /* in dest pixels -> map to src */
            int svw=(int)(boxw*sx/z+0.5), svh=(int)(boxh*sy/z+0.5);
            if(svw<8) svw=8; if(svh<8) svh=8; if(svw>sw) svw=sw; if(svh>sh) svh=sh;
            { int cx=(int)((mx-(bx+ox))*sx+0.5), cy=(int)((my-(by+oy))*sy+0.5);
              int x0=cx-svw/2, y0=cy-svh/2;
              if(x0<0)x0=0; if(y0<0)y0=0; if(x0+svw>sw) x0=sw-svw; if(y0+svh>sh) y0=sh-svh;
              blit_bgr(h,bx,by,boxw,boxh,bgr,sw,sh,x0,y0,svw,svh); }
            SetBkMode(h,TRANSPARENT); SetTextColor(h,RGB(120,255,140)); TextOutA(h,bx+6,by+5,"Result (zoom)",13);
        } else {
            blit_bgr(h,bx+ox,by+oy,dw,dh,bgr,sw,sh,0,0,sw,sh);
            SetBkMode(h,TRANSPARENT); SetTextColor(h,RGB(190,190,200)); TextOutA(h,bx+6,by+5,"Result",6);
        }
        { HPEN pen=CreatePen(PS_SOLID,1,RGB(90,90,100)); HGDIOBJ op=SelectObject(h,pen), ob=SelectObject(h,GetStockObject(NULL_BRUSH)); Rectangle(h,bx,by,bx+boxw,by+boxh); SelectObject(h,op); SelectObject(h,ob); DeleteObject(pen); }
        { char info[96]; SIZE sz; if(g_resExpW>0 && (g_resExpW!=sw||g_resExpH!=sh)) sprintf(info,"preview %dx%d   export %dx%d",sw,sh,g_resExpW,g_resExpH); else sprintf(info,"%dx%d",sw,sh);
          SetBkMode(h,TRANSPARENT); SetTextColor(h,RGB(150,150,160));
          if(GetTextExtentPoint32A(h,info,lstrlenA(info),&sz)){ HBRUSH tb=CreateSolidBrush(RGB(18,18,20)); RECT tr={bx+2,by+boxh-19,bx+10+sz.cx,by+boxh-1}; FillRect(h,&tr,tb); DeleteObject(tb); }
          TextOutA(h,bx+6,by+boxh-16,info,lstrlenA(info)); }
    }
}

static LRESULT CALLBACK PvProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
    if(hwnd==g_pvRes){
        if(msg==WM_MOUSEMOVE){ TRACKMOUSEEVENT t; g_hx=(short)LOWORD(l); g_hy=(short)HIWORD(l); g_hover=1; t.cbSize=sizeof(t); t.dwFlags=TME_LEAVE; t.hwndTrack=hwnd; TrackMouseEvent(&t); InvalidateRect(hwnd,NULL,FALSE); return 0; }
        if(msg==WM_MOUSELEAVE){ g_hover=0; InvalidateRect(hwnd,NULL,FALSE); return 0; }
    }
    if(msg==WM_ERASEBKGND) return 1;
    if(msg==WM_PAINT){
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps); RECT rc; int w,h;
        GetClientRect(hwnd,&rc); w=rc.right-rc.left; h=rc.bottom-rc.top;
        if(w>0 && h>0){
            HDC mem=CreateCompatibleDC(hdc);
            HBITMAP bmp=CreateCompatibleBitmap(hdc,w,h);
            HGDIOBJ old=SelectObject(mem,bmp);
            if(hwnd==g_pvSrc) draw_thumb(mem,&rc,g_srcBgr,g_srcW,g_srcH);
            else draw_result(mem,&rc,g_resBgr,g_resW,g_resH,g_hover,g_hx,g_hy);
            BitBlt(hdc,0,0,w,h,mem,0,0,SRCCOPY);
            SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
        }
        EndPaint(hwnd,&ps); return 0;
    }
    return DefWindowProcA(hwnd,msg,w,l);
}

static void gui_log(void *ud,const char *msg){ HWND hw=(HWND)ud; char *s; int n; if(!hw)return; n=lstrlenA(msg)+3; s=(char*)malloc(n); if(s){lstrcpyA(s,msg);lstrcatA(s,"\r\n"); PostMessageA(hw,WM_APPLOG,0,(LPARAM)s);} }

static void sl2ed(int i){ if(g_upd)return; g_upd=1; { int v=(int)SendMessage(g_sl[i],TBM_GETPOS,0,0); char b[32]; if(i==1||i==2) sprintf(b,"%.2f",v/100.0); else sprintf(b,"%d",v); SetWindowTextA(g_ed[i],b);} g_upd=0; }
static void ed2sl(int i){ if(g_upd)return; g_upd=1; { char b[32]; int v,mn,mx; GetWindowTextA(g_ed[i],b,sizeof(b)); mn=(int)SendMessage(g_sl[i],TBM_GETRANGEMIN,0,0); mx=(int)SendMessage(g_sl[i],TBM_GETRANGEMAX,0,0); if(i==1||i==2) v=(int)(atof(b)*100+0.5); else v=atoi(b); if(v<mn)v=mn; if(v>mx)v=mx; SendMessage(g_sl[i],TBM_SETPOS,TRUE,v);} g_upd=0; }

static void read_cfg(Config *c,int export_mode){
    memset(c,0,sizeof(*c)); c->font_size=(int)SendMessage(g_sl[0],TBM_GETPOS,0,0); c->jitter=SendMessage(g_sl[1],TBM_GETPOS,0,0)/100.0; c->edge_threshold=SendMessage(g_sl[2],TBM_GETPOS,0,0)/100.0; c->scale=(double)SendMessage(g_sl[3],TBM_GETPOS,0,0); c->seed=(unsigned)SendMessage(g_sl[4],TBM_GETPOS,0,0); c->threads=(int)SendMessage(g_sl[5],TBM_GETPOS,0,0); c->no_edges=(SendMessage(g_hNoEdge,BM_GETCHECK,0,0)==BST_CHECKED); {int b=(int)SendMessage(g_hBg,CB_GETCURSEL,0,0); c->bg_mode=b==1?BG_BLACK:b==2?BG_WHITE:BG_AUTO;} c->max_dim=export_mode?0:900;
}
typedef struct { Config cfg; char in[MAX_PATH],out[MAX_PATH]; HWND hwnd; int ex; } Job;
typedef struct { unsigned char *rgb,*bgr; int w,h,ex,expW,expH; char msg[256]; } Res;
static DWORD WINAPI worker(LPVOID p){ Job *j=(Job*)p; char err[512]=""; Res *r=(Res*)calloc(1,sizeof(Res)); g_log_cb=gui_log; g_log_ud=j->hwnd; g_quiet=0; if(!g_log_cs_init){InitializeCriticalSection(&g_log_cs);g_log_cs_init=1;} g_t0=now_sec(); j->cfg.input=j->in; r->ex=j->ex; xlogf("%s",j->ex?"Export":"Preview"); if(render_to_buffer(&j->cfg,&r->rgb,&r->w,&r->h,&r->expW,&r->expH,err,sizeof(err))){ r->bgr=(unsigned char*)malloc((size_t)r->w*r->h*3); if(r->bgr) to_bgr(r->rgb,r->bgr,r->w*r->h); sprintf(r->msg,"OK %dx%d",r->w,r->h); if(j->ex&&j->out[0]){ if(stbi_write_jpg(j->out,r->w,r->h,3,r->rgb,95)) xlogf("Exported: %s",j->out); else xlogf("ERROR write"); } } else sprintf(r->msg,"ERR: %s",err); PostMessageA(j->hwnd,j->ex?WM_APPEXPORT:WM_APPPREV,0,(LPARAM)r); free(j); return 0; }

static void load_src(void){
    char p[MAX_PATH]; int w,h,n,ow,oh; unsigned char *rgb,*bgr,*tmp=NULL;
    GetWindowTextA(g_hIn,p,MAX_PATH); if(!p[0])return;
    rgb=stbi_load(p,&w,&h,&n,3); if(!rgb){ gui_log(g_hWnd,"Cannot load source"); return; }
    ow=w; oh=h;
    if(w>1400 || h>1400){
        int tw,th; double s = 1400.0 / (w>h?w:h);
        tw=(int)(w*s); th=(int)(h*s); if(tw<1)tw=1; if(th<1)th=1;
        tmp=(unsigned char*)malloc((size_t)tw*th*3);
        if(tmp){ stbir_resize_uint8(rgb,w,h,0,tmp,tw,th,0,3); stbi_image_free(rgb); rgb=tmp; w=tw; h=th; }
    }
    bgr=(unsigned char*)malloc((size_t)w*h*3); if(!bgr){ stbi_image_free(rgb); return; }
    to_bgr(rgb,bgr,w*h); if(g_srcBgr) free(g_srcBgr); g_srcBgr=bgr; g_srcW=w; g_srcH=h; stbi_image_free(rgb);
    lstrcpynA(g_srcPath,p,MAX_PATH);
    { char b[160]; if(w!=ow||h!=oh) sprintf(b,"Loaded source: %dx%d (thumb %dx%d)",ow,oh,w,h); else sprintf(b,"Loaded source: %dx%d",ow,oh); gui_log(g_hWnd,b); }
    InvalidateRect(g_pvSrc,NULL,TRUE);
}
static void sched(void){ if(g_hWnd) { KillTimer(g_hWnd,IDT_DEBOUNCE); SetTimer(g_hWnd,IDT_DEBOUNCE,450,NULL); } }
static void go(int ex){ char in[MAX_PATH]; Job *j; GetWindowTextA(g_hIn,in,MAX_PATH); if(!in[0]){ if(ex) gui_log(g_hWnd,"Set input file"); return; } j=(Job*)calloc(1,sizeof(Job)); if(!j)return; lstrcpyA(j->in,in); GetWindowTextA(g_hOut,j->out,MAX_PATH); j->hwnd=g_hWnd; j->ex=ex; read_cfg(&j->cfg,ex); if(ex&&!j->out[0]){ gui_log(g_hWnd,"Set output path for Export"); free(j); return; } if(ex) EnableWindow(g_hExport,FALSE); CloseHandle(CreateThread(NULL,0,worker,j,0,NULL)); }

/* all right-panel children in order for layout */
static HWND g_rp[64]; static int g_rpn=0;
static void rp_add(HWND h){ if(h&&g_rpn<64) g_rp[g_rpn++]=h; }
static void layout_right(int cw,int ch){
    int rx=cw-RPW, y=THUMBH+8, x=rx+8, w=RPW-16, sw=w-62, i;
    if(rx<200) rx=200;
    if(g_pvSrc) MoveWindow(g_pvSrc, rx, 0, RPW, THUMBH, TRUE);
    /* walk known controls by id; simpler: reposition by stored handles in creation order */
    /* Input label + edit + btn */
    { HWND h;
      h=GetDlgItem(g_hWnd, 9001); if(h) MoveWindow(h,x,y,w,14,TRUE); y+=14;
      if(g_hIn) MoveWindow(g_hIn,x,y,w-34,22,TRUE); h=GetDlgItem(g_hWnd,ID_BIN); if(h) MoveWindow(h,x+w-32,y,32,22,TRUE); y+=26;
      h=GetDlgItem(g_hWnd, 9002); if(h) MoveWindow(h,x,y,w,14,TRUE); y+=14;
      if(g_hOut) MoveWindow(g_hOut,x,y,w-34,22,TRUE); h=GetDlgItem(g_hWnd,ID_BOUT); if(h) MoveWindow(h,x+w-32,y,32,22,TRUE); y+=26;
      for(i=0;i<6;i++){ h=GetDlgItem(g_hWnd, 9010+i); if(h) MoveWindow(h,x,y,w,14,TRUE); y+=14; if(g_sl[i]) MoveWindow(g_sl[i],x,y,sw,22,TRUE); if(g_ed[i]) MoveWindow(g_ed[i],x+sw+4,y,58,22,TRUE); y+=24; }
      h=GetDlgItem(g_hWnd,9020); if(h) MoveWindow(h,x,y,22,16,TRUE); if(g_hBg) MoveWindow(g_hBg,x+24,y-2,96,120,TRUE); if(g_hNoEdge) MoveWindow(g_hNoEdge,x+124,y-2,90,18,TRUE); y+=24;
      if(g_hExport) MoveWindow(g_hExport,x,y,w,28,TRUE); y+=32;
      h=GetDlgItem(g_hWnd,9021); if(h) MoveWindow(h,x,y,w,14,TRUE); y+=14;
      if(g_hLog){ int lh=ch-y-6; if(lh<60) lh=60; MoveWindow(g_hLog,x,y,w,lh,TRUE); }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
    switch(msg){
    case WM_HSCROLL:{int i; for(i=0;i<6;i++) if((HWND)l==g_sl[i]){sl2ed(i);sched();} return 0;}
    case WM_COMMAND:{ int id=LOWORD(w),c=HIWORD(w);
        if(id==ID_BIN){OPENFILENAMEA o; char b[MAX_PATH]=""; ZeroMemory(&o,sizeof(o)); o.lStructSize=sizeof(o); o.hwndOwner=hwnd; o.lpstrFile=b; o.nMaxFile=MAX_PATH; o.lpstrFilter="Images\0*.jpg;*.jpeg;*.png;*.bmp\0All\0*.*\0"; o.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR; if(GetOpenFileNameA(&o)){SetWindowTextA(g_hIn,b); load_src(); sched();}}
        else if(id==ID_BOUT){OPENFILENAMEA o; char b[MAX_PATH]="out.jpg"; ZeroMemory(&o,sizeof(o)); o.lStructSize=sizeof(o); o.hwndOwner=hwnd; o.lpstrFile=b; o.nMaxFile=MAX_PATH; o.lpstrFilter="JPEG\0*.jpg\0All\0*.*\0"; o.lpstrDefExt="jpg"; o.Flags=OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR; if(GetSaveFileNameA(&o)) SetWindowTextA(g_hOut,b);}
        else if(id==ID_EXPORT) go(1);
        else if(id==ID_IN && c==EN_KILLFOCUS){ load_src(); sched(); }
        else if(id==ID_BG&&c==CBN_SELCHANGE) sched();
        else if(id==ID_NOEDGE) sched();
        else if(id>=ID_ED_FONT&&id<=ID_ED_THR && c==EN_KILLFOCUS){ ed2sl(id-ID_ED_FONT); sched(); }
        return 0;}
    case WM_TIMER: if(w==IDT_DEBOUNCE){KillTimer(hwnd,w); {char in[MAX_PATH]; GetWindowTextA(g_hIn,in,MAX_PATH); if(in[0]){ if(!g_srcBgr || lstrcmpiA(in,g_srcPath)!=0) load_src(); go(0);} }} return 0;
    case WM_APPLOG:{char *s=(char*)l; if(s){int n=GetWindowTextLengthA(g_hLog); SendMessageA(g_hLog,EM_SETSEL,(WPARAM)n,(LPARAM)n); SendMessageA(g_hLog,EM_REPLACESEL,FALSE,(LPARAM)s); free(s);} return 0;}
    case WM_APPPREV: case WM_APPEXPORT:{Res *r=(Res*)l; if(r){ gui_log(hwnd,r->msg); if(r->ex){if(r->rgb)free(r->rgb); if(r->bgr)free(r->bgr); EnableWindow(g_hExport,TRUE);} else {if(g_resBgr)free(g_resBgr); g_resBgr=r->bgr; g_resW=r->w; g_resH=r->h; g_resExpW=r->expW; g_resExpH=r->expH; if(r->rgb)free(r->rgb); InvalidateRect(g_pvRes,NULL,FALSE);} free(r);} return 0;}
    case WM_SIZE:{int cw=LOWORD(l),ch=HIWORD(l); if(cw>400&&g_pvRes){ MoveWindow(g_pvRes,0,0,cw-RPW>100?cw-RPW:400,ch,TRUE); layout_right(cw,ch);} return 0;}
    case WM_CLOSE: DestroyWindow(hwnd); return 0; case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcA(hwnd,msg,w,l);
    }
}

static HWND mkst(HWND p,HINSTANCE hi,int id,const char *t,int x,int y,int w,int h){ HWND hnd=CreateWindowA("STATIC",t,WS_CHILD|WS_VISIBLE,x,y,w,h,p,(HMENU)(INT_PTR)id,hi,0); return hnd; }
static void ui(HWND hwnd,HINSTANCE hi){
    HFONT hf=(HFONT)GetStockObject(DEFAULT_GUI_FONT); int x=0,y=THUMBH+8,w=RPW-16,sw=w-62,i;
    const char *lb[6]={"Font (0=auto)","Jitter 0..1","Edge 0..1","Scale 1..4","Seed","Threads (0=auto)"}; int mn[6]={0,0,0,1,0,0},mx[6]={72,100,50,4,9999,16},df[6]={0,40,12,1,42,0};
    /* result fills left, source thumb + settings on right */
    g_pvRes=CreateWindowExA(0,"AsciiPv","",WS_CHILD|WS_VISIBLE,0,0,600,600,hwnd,(HMENU)(INT_PTR)ID_PV_RES,hi,0);
    g_pvSrc=CreateWindowExA(0,"AsciiPv","",WS_CHILD|WS_VISIBLE,600,0,RPW,THUMBH,hwnd,(HMENU)(INT_PTR)ID_PV_SRC,hi,0);
    x=608; y=THUMBH+8; w=RPW-16; sw=w-62;
    mkst(hwnd,hi,9001,"Input image",x,y,w,14); y+=14;
    g_hIn=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,x,y,w-34,22,hwnd,(HMENU)(INT_PTR)ID_IN,hi,0);
    CreateWindowA("BUTTON","..",WS_CHILD|WS_VISIBLE|WS_TABSTOP,x+w-32,y,32,22,hwnd,(HMENU)(INT_PTR)ID_BIN,hi,0); y+=26;
    mkst(hwnd,hi,9002,"Output (Export)",x,y,w,14); y+=14;
    g_hOut=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","out.jpg",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,x,y,w-34,22,hwnd,(HMENU)(INT_PTR)ID_OUT,hi,0);
    CreateWindowA("BUTTON","..",WS_CHILD|WS_VISIBLE|WS_TABSTOP,x+w-32,y,32,22,hwnd,(HMENU)(INT_PTR)ID_BOUT,hi,0); y+=26;
    for(i=0;i<6;i++){ mkst(hwnd,hi,9010+i,lb[i],x,y,w,14); y+=14;
      g_sl[i]=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_HORZ,x,y,sw,22,hwnd,(HMENU)(INT_PTR)(ID_SL_FONT+i),hi,0);
      SendMessage(g_sl[i],TBM_SETRANGE,TRUE,MAKELONG(mn[i],mx[i])); SendMessage(g_sl[i],TBM_SETPOS,TRUE,df[i]); SendMessage(g_sl[i],TBM_SETTICFREQ,(i==4?2000:(i==3?1:(i==1||i==2?10:8))),0);
      g_ed[i]=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL|ES_RIGHT,x+sw+4,y,58,22,hwnd,(HMENU)(INT_PTR)(ID_ED_FONT+i),hi,0); sl2ed(i); y+=24; }
    mkst(hwnd,hi,9020,"BG",x,y,22,16);
    g_hBg=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,x+24,y-2,96,120,hwnd,(HMENU)(INT_PTR)ID_BG,hi,0);
    SendMessageA(g_hBg,CB_ADDSTRING,0,(LPARAM)"auto (ANSI)"); SendMessageA(g_hBg,CB_ADDSTRING,0,(LPARAM)"black"); SendMessageA(g_hBg,CB_ADDSTRING,0,(LPARAM)"white"); SendMessageA(g_hBg,CB_SETCURSEL,0,0);
    g_hNoEdge=CreateWindowA("BUTTON","No edges",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,x+124,y-2,90,18,hwnd,(HMENU)(INT_PTR)ID_NOEDGE,hi,0); y+=24;
    g_hExport=CreateWindowA("BUTTON","Export",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,x,y,w,28,hwnd,(HMENU)(INT_PTR)ID_EXPORT,hi,0); y+=32;
    mkst(hwnd,hi,9021,"Log",x,y,w,14); y+=14;
    g_hLog=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,x,y,w,180,hwnd,(HMENU)(INT_PTR)ID_LOG,hi,0);
    {HWND st; for(st=GetWindow(hwnd,GW_CHILD);st;st=GetWindow(st,GW_HWNDNEXT)) SendMessage(st,WM_SETFONT,(WPARAM)hf,TRUE);}
    { RECT rc; GetClientRect(hwnd,&rc); SendMessage(hwnd,WM_SIZE,0,MAKELONG(rc.right,rc.bottom)); }
}

static int run_gui(HINSTANCE hi){
    INITCOMMONCONTROLSEX icc; WNDCLASSEXA wc,pv; HWND hwnd; MSG msg; RECT rc; int W=1120,H=720,sx,sy;
    icc.dwSize=sizeof(icc); icc.dwICC=ICC_BAR_CLASSES|ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES|ICC_COOL_CLASSES; InitCommonControlsEx(&icc);
    ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.lpfnWndProc=WndProc; wc.hInstance=hi; wc.lpszClassName="AsciiDitherV5"; wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
    if(!RegisterClassExA(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){MessageBoxA(NULL,"RegisterClass failed","err",MB_ICONERROR);return 1;}
    ZeroMemory(&pv,sizeof(pv)); pv.cbSize=sizeof(pv); pv.lpfnWndProc=PvProc; pv.hInstance=hi; pv.lpszClassName="AsciiPv"; pv.hCursor=LoadCursor(NULL,IDC_ARROW); pv.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    if(!RegisterClassExA(&pv)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){MessageBoxA(NULL,"RegisterClass pv failed","err",MB_ICONERROR);return 1;}
    rc.left=0;rc.top=0;rc.right=W;rc.bottom=H; AdjustWindowRectEx(&rc,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_APPWINDOW); W=rc.right-rc.left;H=rc.bottom-rc.top;
    { RECT wa; int aw,ah;
      if(SystemParametersInfoA(SPI_GETWORKAREA,0,&wa,0)){ aw=wa.right-wa.left; ah=wa.bottom-wa.top;
        if(W>aw-20) W=aw-20; if(H>ah-20) H=ah-20;
        sx=wa.left+(aw-W)/2; sy=wa.top+(ah-H)/2;
      } else { sx=(GetSystemMetrics(SM_CXSCREEN)-W)/2; sy=(GetSystemMetrics(SM_CYSCREEN)-H)/2; if(sx<0)sx=0; if(sy<0)sy=0; } }
    hwnd=CreateWindowExA(WS_EX_APPWINDOW,wc.lpszClassName,"ascii_dither v7",WS_OVERLAPPEDWINDOW,sx>0?sx:CW_USEDEFAULT,sy>0?sy:CW_USEDEFAULT,W,H,NULL,NULL,hi,NULL);
    if(!hwnd){char b[80]; sprintf(b,"CreateWindow failed (%lu)",GetLastError()); MessageBoxA(NULL,b,"ascii_dither",MB_ICONERROR); return 1;}
    g_hWnd=hwnd; ui(hwnd,hi); ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    while(GetMessage(&msg,NULL,0,0)>0){ if(!IsDialogMessage(hwnd,&msg)){TranslateMessage(&msg);DispatchMessage(&msg);} }
    if(g_srcBgr)free(g_srcBgr); if(g_resBgr)free(g_resBgr); return (int)msg.wParam;
}
static int win_entry(void){ int argc; LPWSTR *wa=CommandLineToArgvW(GetCommandLineW(),&argc); char **av; int i,wg=0; HINSTANCE hi=GetModuleHandle(NULL); av=(char**)malloc(sizeof(char*)*(argc+1)); for(i=0;i<argc;i++){int n=WideCharToMultiByte(CP_UTF8,0,wa[i],-1,NULL,0,NULL,NULL); av[i]=(char*)malloc(n); WideCharToMultiByte(CP_UTF8,0,wa[i],-1,av[i],n,NULL,NULL);} av[argc]=NULL; if(argc>1){for(i=1;i<argc;i++) if(strcmp(av[i],"--gui")==0) wg=1; if(!wg){ if(AttachConsole(ATTACH_PARENT_PROCESS)){FILE*f;freopen_s(&f,"CONOUT$","w",stdout);freopen_s(&f,"CONOUT$","w",stderr);} {int r=run_cli(argc,av); LocalFree(wa); return r;}}} LocalFree(wa); return run_gui(hi); }
int WINAPI WinMain(HINSTANCE a,HINSTANCE b,LPSTR c,int d){(void)a;(void)b;(void)c;(void)d;return win_entry();}
int main(int a,char**v){(void)a;(void)v;return win_entry();}
#else
int main(int argc,char**argv){ if(argc<=1){usage(argv[0]);return 1;} {int r=run_cli(argc,argv); return r==-2?1:r;} }
#endif

