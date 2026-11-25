// libSceAvPlayer: movie playback.
//
// Two modes, chosen at build time:
//
//   Without JSTARS_HAS_FFMPEG this is a diagnostic stub: it validates arguments, records the player
//   state the game builds up (init data, sources), inspects each source file named by
//   sceAvPlayerAddSource (size and MP4 'ftyp' brand) and then reports honestly that no stream exists:
//   StreamCount is 0, IsActive is false, GetVideoDataEx/GetAudioData return no frame. A movie therefore
//   ends immediately and the game skips it. Every call is logged on its first use (and AddSource every
//   time) so the log shows exactly which movies the game asked for. Audit status: diagnostic-stub.
//
//   With -DJSTARS_HAS_FFMPEG (libavformat/libavcodec/libavutil, see docs/MOVIES.md) this demuxes and
//   decodes the source for real: StreamCount/StreamInfo report the file's streams, Start begins
//   playback, GetVideoDataEx returns NV12 frames and GetAudioData returns S16 PCM. Audit status: partial
//   (no timed text, no HTTP streaming, no trick modes).
//
// The packed Sony ABI layouts below match the PS4 SDK as used by fpPS4's ps4_libsceavplayer (the
// reference open-source implementation this backend follows). Event ids: STOP=1 READY=2 PLAY=3 PAUSE=4
// BUFFERING=5. Stream types: video=0 audio=1 timedtext=2 unknown=3.
#define _GNU_SOURCE
#include "jstars_rt.h"
#include "jstars_os.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <malloc.h>
#endif
#ifdef JSTARS_HAS_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#endif

// mingw GCC 13 (x86_64-w64-mingw32-gcc-posix 13, Ubuntu cross) dies with a backend
// ICE ("in choose_baseaddr", config/i386/i386.cc) on this TU at -O2 once the FFmpeg
// backend is enabled. Build this file at -O0; the hot loops live in the FFmpeg
// DLLs anyway. Re-test at default -O2 when the toolchain moves past GCC 13.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O0")
#endif

typedef struct { const char* name; const char* lib; void* fn; int is_data; } HostSymbol;

enum { AVP_ERR_INVALID_PARAMS = (int)0x806A0001, AVP_ERR_OPERATION_FAILED = (int)0x806A0002 };
enum { AVP_EVENT_STOP = 1, AVP_EVENT_READY = 2, AVP_EVENT_PLAY = 3, AVP_EVENT_PAUSE = 4, AVP_EVENT_BUFFERING = 5 };
enum { AVP_STREAM_VIDEO = 0, AVP_STREAM_AUDIO = 1, AVP_STREAM_TIMEDTEXT = 2, AVP_STREAM_UNKNOWN = 3 };
#define AVP_MAX 8
#define AVP_MAX_STREAMS 8
#define AVP_HANDLE_BASE 0x41560000u   // opaque, non-pointer handle values: the game must not dereference them

typedef void (GUEST_ABI* AvpEventCb)(void* obj, int event, int source_id, void* data);
typedef void* (GUEST_ABI* AvpAllocFn)(void* obj, uint32_t align, uint32_t size);
typedef void (GUEST_ABI* AvpFreeFn)(void* obj, void* mem);
typedef int (GUEST_ABI* AvpFileOpenFn)(void* obj, const char* name);
typedef int (GUEST_ABI* AvpFileCloseFn)(void* obj);
typedef int (GUEST_ABI* AvpFileReadFn)(void* obj, uint8_t* buf, uint64_t pos, uint32_t len);
typedef uint64_t (GUEST_ABI* AvpFileSizeFn)(void* obj);

// ---- Sony ABI (packed; sizes asserted in the self-test) -----------------------------------------------
typedef struct __attribute__((packed)) {
    void* obj; AvpAllocFn alloc; AvpFreeFn dealloc; AvpAllocFn alloc_tex; AvpFreeFn dealloc_tex;
} AvpMemAlloc;
typedef struct __attribute__((packed)) {
    void* obj; AvpFileOpenFn open; AvpFileCloseFn close; AvpFileReadFn read; AvpFileSizeFn size;
} AvpFileRep;
typedef struct __attribute__((packed)) {
    void* obj; AvpEventCb cb;
} AvpEventRep;
typedef struct __attribute__((packed)) {
    AvpMemAlloc mem; AvpFileRep file; AvpEventRep ev;
    uint32_t debug_level, base_prio; int32_t num_tex_bufs; uint8_t autostart, rsv[3]; const char* lang;
} AvpInitData; // 40+40+16+4+4+4+1+3+8 = 120
typedef struct __attribute__((packed)) {
    uint64_t this_size;
    AvpMemAlloc mem; AvpFileRep file; AvpEventRep ev; const char* lang;
    uint32_t debug_level, audio_prio, audio_aff, video_prio, video_aff, demux_prio, demux_aff,
        ctrl_prio, ctrl_aff, http_prio, http_aff, file_prio, file_aff;
    int32_t num_tex_bufs; uint8_t autostart, rsv[3];
    uint32_t audio_stack, video_stack, demux_stack, ctrl_stack, http_stack, file_stack;
} AvpInitDataEx; // 8+40+40+16+8+52+4+1+3+24 = 196
typedef struct __attribute__((packed)) {
    uint16_t ch; uint8_t rsv[2]; uint32_t rate, size; char lang[4];
} AvpAudio; // 16
typedef struct __attribute__((packed)) {
    uint32_t w, h; float aspect; char lang[4];
} AvpVideo; // 16
typedef struct __attribute__((packed)) {
    uint16_t top, left, bottom, right;
} AvpTextPos; // 8
typedef struct __attribute__((packed)) {
    char lang[4]; uint16_t text_size, font_size; AvpTextPos pos;
} AvpTimedText; // 16
typedef union __attribute__((packed)) {
    uint8_t raw[16]; AvpAudio audio; AvpVideo video; AvpTimedText subs;
} AvpDetails; // 16
typedef struct __attribute__((packed)) {
    uint32_t type, align; AvpDetails det; uint64_t duration, start;
} AvpStreamInfo; // 4+4+16+8+8 = 40
typedef struct __attribute__((packed)) {
    uint8_t* data; uint32_t rsv, align; uint64_t ts; AvpDetails det;
} AvpFrameInfo; // 8+4+4+8+16 = 40
typedef struct __attribute__((packed)) {
    uint16_t ch; uint8_t rsv[2]; uint32_t rate, size; char lang[4]; uint8_t rsv1[64];
} AvpAudioEx; // 80
typedef struct __attribute__((packed)) {
    uint32_t w, h; float aspect; char lang[4]; uint32_t framerate;
    uint32_t crop_l, crop_r, crop_t, crop_b, pitch;
    uint8_t luma_bits, chroma_bits, full_range; uint8_t rsv[37];
} AvpVideoEx; // 4*5+16+4+1+1+1+37 = 80
typedef struct __attribute__((packed)) {
    char lang[4]; uint8_t rsv[76];
} AvpTimedTextEx; // 80
typedef union __attribute__((packed)) {
    uint8_t raw[80]; AvpAudioEx audio; AvpVideoEx video; AvpTimedTextEx subs;
} AvpDetailsEx; // 80
typedef struct __attribute__((packed)) {
    uint8_t* data; uint32_t rsv, align; uint64_t ts; AvpDetailsEx det;
} AvpFrameInfoEx; // 8+4+4+8+80 = 104
typedef struct __attribute__((packed)) {
    const char* name; uint32_t len, align;
} AvpUri; // 16
typedef struct __attribute__((packed)) {
    AvpUri uri; uint8_t rsv[64]; int32_t source_type; uint8_t rsv2[44];
} AvpSourceDetails; // 16+64+4+44 = 128

// ---- player state (host side, never exposed to the guest) ----------------------------------------------
typedef struct {
    uint32_t type; // AVP_STREAM_* of the Sony ABI
    uint32_t w, h; float aspect; char lang[4];
    uint32_t ch, rate, frame_bytes;
    uint64_t duration_ms, start_ms;
} AvpStreamDesc;
typedef struct {
    int used;
    void* event_obj; AvpEventCb event_cb;
    void* mem_obj; AvpAllocFn alloc, alloc_tex; AvpFreeFn dealloc, dealloc_tex;
    void* file_obj; AvpFileOpenFn f_open; AvpFileCloseFn f_close; AvpFileReadFn f_read; AvpFileSizeFn f_size;
    int sources, started, looping, paused, finished;
    int stop_sent;
    char path[512];
    uint64_t file_size; char brand[5];
    int n_streams; AvpStreamDesc streams[AVP_MAX_STREAMS];
    uint64_t duration_ms;
    uint64_t calls_active, calls_video, calls_audio;
    uint64_t frames_video, frames_audio;
    void* city_tex; void* city_tex_raw; size_t city_tex_sz; int city_tex_game;   // last video frame (city_tex = aligned address given to the game, city_tex_raw = what the allocator returned), game-allocated when possible
    void* city_pcm; size_t city_pcm_sz; int city_pcm_game;   // last audio frame, game-allocated when possible
    void* be; // decoder backend context (FFmpeg), or NULL
} AvpPlayer;
static AvpPlayer g_avp[AVP_MAX];
static pthread_mutex_t g_avp_lock = PTHREAD_MUTEX_INITIALIZER;

static AvpPlayer* avp_get(uint64_t h) {
    if (h <= AVP_HANDLE_BASE || h > AVP_HANDLE_BASE + AVP_MAX) return NULL;
    AvpPlayer* p = &g_avp[h - AVP_HANDLE_BASE - 1];
    return p->used ? p : NULL;
}

// ---- presentation experiments (env overrides; all logged at first frame) ------------------------------
// Env (same console the game is launched from):
//   JSTARS_AV_SYSALLOC=1 / JSTARS_AV_PITCH=N / JSTARS_AV_ASPECT=F
//   JSTARS_AV_XOFF=N (pixels, + moves content right) / JSTARS_AV_YOFF=N / JSTARS_AV_SWAPUV=1
//   JSTARS_AV_TEST=red|black|white|bars (replaces the picture: bypass/pattern test)
// File fallback (no console needed, same values, one per file):
//   <user>/avplayer_dump/SYSALLOC.txt PITCH.txt ASPECT.txt XOFF.txt YOFF.txt SWAPUV.txt TEST.txt
// A stale .exe shows a first-frame line WITHOUT the trailing "xoff ..." fields: rebuild.
static int avp_cfg_text(const char* name, char* out, size_t outsz) {
    if (!name || !out || outsz < 2) return 0;
    out[0] = 0;
    char env[64];
    snprintf(env, sizeof env, "JSTARS_AV_%s", name);
    const char* e = getenv(env);
    if (e && e[0]) { snprintf(out, outsz, "%s", e); return 1; }
    char path[1600];
    snprintf(path, sizeof path, "%s/avplayer_dump/%s.txt", rt_fs_user_root(), name);
    FILE* f = os_fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, outsz - 1, f);
    fclose(f);
    out[n < outsz ? n : outsz - 1] = 0;
    // trim leading/trailing whitespace (spaces, CR, LF, tabs)
    size_t a = 0;
    while (out[a] && (out[a] == ' ' || out[a] == '\t' || out[a] == '\r' || out[a] == '\n')) a++;
    size_t b = strlen(out);
    while (b > a && (out[b - 1] == ' ' || out[b - 1] == '\t' || out[b - 1] == '\r' || out[b - 1] == '\n')) b--;
    out[b] = 0;
    if (a) memmove(out, out + a, b - a + 1);
    // strip a UTF-8 BOM (Notepad "UTF-8" writes one; atoi/strcmp would fail on it)
    if ((unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        memmove(out, out + 3, strlen(out + 3) + 1);
    return out[0] ? 1 : 0;
}
// Numeric parse that skips junk (BOM leftovers, labels like "XOFF=512").
static int avp_num(const char* s) {
    if (!s) return 0;
    while (*s && *s != '-' && (*s < '0' || *s > '9')) s++;
    return atoi(s);
}
// Boolean: on if it contains '1' or the words true/yes/on (any case), off for "0"/empty.
static int avp_bool(const char* s) {
    if (!s || !s[0]) return 0;
    for (const char* q = s; *q; ++q) if (*q == '1') return 1;
    char low[32];
    size_t n = strlen(s) < sizeof low - 1 ? strlen(s) : sizeof low - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        low[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    low[n] = 0;
    if (strstr(low, "true") || strstr(low, "yes") || strstr(low, "on")) return 1;
    return 0;
}
static int avp_force_sysalloc(void) {
    static int v = -1;
    if (v < 0) {
        char b[32];
        v = (avp_cfg_text("SYSALLOC", b, sizeof b) && b[0]) ? avp_bool(b) : 0;
    }
    return v;
}
static uint32_t avp_pitch_ovr(uint32_t w) {
    char b[32];
    long n = (avp_cfg_text("PITCH", b, sizeof b) && b[0]) ? (long)avp_num(b) : 0;
    if (n < (long)w) return w;
    if (n > 4096) n = 4096;
    return (uint32_t)n;
}
static float avp_aspect_ovr(uint32_t w, uint32_t h) {
    char b[32];
    double f = (avp_cfg_text("ASPECT", b, sizeof b) && b[0]) ? atof(b) : 0.0;
    if (f > 0.0) return (float)f;
    return h ? (float)w / (float)h : 16.0f / 9.0f;
}
static int avp_xoff(void) {
    char b[32];
    return (avp_cfg_text("XOFF", b, sizeof b) && b[0]) ? avp_num(b) : 0;
}
static int avp_yoff(void) {
    char b[32];
    return (avp_cfg_text("YOFF", b, sizeof b) && b[0]) ? avp_num(b) : 0;
}
static int avp_swapuv(void) {
    char b[32];
    return (avp_cfg_text("SWAPUV", b, sizeof b) && b[0]) ? avp_bool(b) : 0;
}
// 0=off 1=red 2=black 3=white 4=bars(vertical grayscale stripes: shift ruler)
static int avp_testmode(void) {
    char b[32];
    if (!avp_cfg_text("TEST", b, sizeof b) || !b[0]) return 0;
    for (char* q = b; *q; ++q) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
    if (strstr(b, "red")) return 1;
    if (strstr(b, "black")) return 2;
    if (strstr(b, "white")) return 3;
    if (strstr(b, "bars")) return 4;
    return 0;
}
static void avp_fill_test(uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride, int mode) {
    if (!dst || !mode || !w || !h || stride < w) return;
    uint8_t Yv = 16, Uv = 128, Vv = 128;
    if (mode == 1) { Yv = 81; Uv = 90; Vv = 240; }        // red
    else if (mode == 2) { Yv = 16; Uv = 128; Vv = 128; }  // black
    else if (mode == 3) { Yv = 235; Uv = 128; Vv = 128; } // white
    if (mode == 4) {
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = dst + (size_t)y * stride;
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t i = x * 8 / w; // 8 stripes 0..7 -> Y = 16 + i*31
                row[x] = (uint8_t)(16 + i * 31);
            }
        }
        uint8_t* uv = dst + (size_t)stride * h;
        for (uint32_t y = 0; y < h / 2; ++y) {
            uint8_t* row = uv + (size_t)y * stride;
            for (uint32_t x = 0; x < w; x += 2) { row[x] = 128; row[x + 1] = 128; }
        }
        return;
    }
    for (uint32_t y = 0; y < h; ++y) memset(dst + (size_t)y * stride, Yv, w);
    uint8_t* uv = dst + (size_t)stride * h;
    for (uint32_t y = 0; y < h / 2; ++y) {
        uint8_t* row = uv + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x += 2) { row[x] = Uv; row[x + 1] = Vv; }
    }
}
// In-place horizontal/vertical rotation of a packed NV12 frame (stride bytes per row).
// xoff>0 moves content right (last pixels wrap to the left), yoff>0 moves content down.
static void avp_apply_shift(uint8_t* dst, uint32_t w, uint32_t h, uint32_t stride,
                            int xoff, int yoff, int swapuv) {
    if (!dst || !w || !h || stride < w) return;
    if (swapuv) {
        uint8_t* uv = dst + (size_t)stride * h;
        for (uint32_t y = 0; y < h / 2; ++y) {
            uint8_t* row = uv + (size_t)y * stride;
            for (uint32_t x = 0; x < w; x += 2) {
                uint8_t t = row[x];
                row[x] = row[x + 1];
                row[x + 1] = t;
            }
        }
    }
    if (xoff != 0 && w > 1) {
        int xo = xoff % (int)w;
        if (xo < 0) xo += w;
        if (xo != 0) {
            uint8_t* tmp = (uint8_t*)malloc(w ? w : 1);
            if (tmp) {
                for (uint32_t y = 0; y < h; ++y) {
                    uint8_t* row = dst + (size_t)y * stride;
                    memcpy(tmp, row, w);
                    for (uint32_t x = 0; x < w; ++x)
                        row[x] = tmp[(x - (uint32_t)xo + w) % w];
                }
                // Chroma moves at half resolution (pairs stay paired; odd shifts round).
                uint32_t uvw = w; // bytes per UV row
                int cxo = (xo / 2) * 2;
                if (cxo != 0) {
                    uint8_t* uv = dst + (size_t)stride * h;
                    uint8_t* ctmp = (uint8_t*)malloc(uvw ? uvw : 1);
                    if (ctmp) {
                        for (uint32_t y = 0; y < h / 2; ++y) {
                            uint8_t* row = uv + (size_t)y * stride;
                            memcpy(ctmp, row, uvw);
                            for (uint32_t x = 0; x < uvw; ++x)
                                row[x] = ctmp[(x - (uint32_t)cxo + uvw) % uvw];
                        }
                        free(ctmp);
                    }
                }
                free(tmp);
            }
        }
    }
    if (yoff != 0 && h > 1) {
        int yo = yoff % (int)h;
        if (yo < 0) yo += h;
        if (yo != 0) {
            uint8_t* rowsrc = (uint8_t*)malloc((size_t)stride * h);
            if (rowsrc) {
                // Luma plane vertical rotate.
                memcpy(rowsrc, dst, (size_t)stride * h);
                for (uint32_t y = 0; y < h; ++y) {
                    uint32_t sy = (y - (uint32_t)yo + h) % h;
                    memcpy(dst + (size_t)y * stride, rowsrc + (size_t)sy * stride, stride);
                }
                // Chroma plane vertical rotate (half height).
                uint8_t* uv = dst + (size_t)stride * h;
                uint8_t* uvsrc = rowsrc + (size_t)stride * h;
                uint32_t chh = h / 2;
                int cy = (yo / 2) % (int)chh;
                if (cy < 0) cy += (int)chh;
                if (cy != 0) {
                    for (uint32_t y = 0; y < chh; ++y) {
                        uint32_t sy = (y - (uint32_t)cy + chh) % chh;
                        memcpy(uv + (size_t)y * stride, uvsrc + (size_t)sy * stride, stride);
                    }
                }
                free(rowsrc);
            }
        }
    }
}

static void avp_lock(void) { pthread_mutex_lock(&g_avp_lock); }
static void avp_unlock(void) { pthread_mutex_unlock(&g_avp_lock); }

static void hexdump(const char* tag, const void* base, size_t n) {
    uint8_t buf[256]; if (n > sizeof buf) n = sizeof buf;
    size_t got = os_safe_read(buf, base, n);
    char line[160];
    for (size_t o = 0; o < got; o += 32) {
        size_t l = 0;
        for (size_t i = o; i < o + 32 && i < got; i += 8) {
            uint64_t v = 0; memcpy(&v, buf + i, got - i >= 8 ? 8 : got - i);
            l += (size_t)snprintf(line + l, sizeof line - l, " %016" PRIx64, v);
        }
        rt_logf("%s +%03zx:%s", tag, o, line);
    }
    if (got < n) rt_logf("%s: only %zu of %zu bytes readable", tag, got, n);
}

// Game-allocator-first buffer: uses the game's allocateTexture/allocate when the game provided them,
// malloc otherwise. `is_game` records which free to use. Reused across frames (freed on Stop/Close).
// Only the FFmpeg backend allocates these, so the stub build marks them used to stay warning-free.
static void* avp_buf_alloc(AvpPlayer* p, uint32_t size, int tex, int* is_game) __attribute__((unused));
static void* avp_buf_alloc(AvpPlayer* p, uint32_t size, int tex, int* is_game) {
    if (size == 0) size = 1;
    if (!avp_force_sysalloc()) {
        if (tex && p->alloc_tex) { void* m = p->alloc_tex(p->mem_obj, 32, size); if (m) { *is_game = 1; return m; } }
        if (!tex && p->alloc) { void* m = p->alloc(p->mem_obj, 32, size); if (m) { *is_game = 1; return m; } }
    }
    *is_game = 0;
#ifdef _WIN32
    return _aligned_malloc(size, 32);
#else
    void* m = NULL;
    if (posix_memalign(&m, 32, size)) return NULL;
    return m;
#endif
}
static void avp_buf_free(AvpPlayer* p, void* mem, int tex, int is_game) __attribute__((unused));
static void avp_buf_free(AvpPlayer* p, void* mem, int tex, int is_game) {
    if (!mem) return;
    if (is_game) { (tex ? p->dealloc_tex : p->dealloc)(p->mem_obj, mem); return; }
#ifdef _WIN32
    _aligned_free(mem);
#else
    free(mem);
#endif
}
static void avp_buffers_drop(AvpPlayer* p) __attribute__((unused));
static void avp_buffers_drop(AvpPlayer* p) {
    avp_buf_free(p, p->city_tex_raw, 1, p->city_tex_game); p->city_tex = NULL; p->city_tex_raw = NULL; p->city_tex_sz = 0;
    avp_buf_free(p, p->city_pcm, 0, p->city_pcm_game); p->city_pcm = NULL; p->city_pcm_sz = 0;
}
static void* avp_buf_ensure(AvpPlayer* p, void** slot, size_t* cur, int* is_game, size_t need, int tex) __attribute__((unused));
static void* avp_buf_ensure(AvpPlayer* p, void** slot, size_t* cur, int* is_game, size_t need, int tex) {
    if (*slot && *cur >= need) return *slot;
    avp_buf_free(p, *slot, tex, *is_game); *slot = NULL; *cur = 0;
    void* m = avp_buf_alloc(p, (uint32_t)need, tex, is_game);
    if (m) { *slot = m; *cur = need; }
    return m;
}

// ---- video frame buffer (aligned) ------------------------------------------------------------------------
// The GPU reads a texture from a base address that is a multiple of 256 (the Gnm image descriptor stores
// address >> 8). The game's allocateTexture only honours the alignment it is asked for, and the frame used to be
// requested with 32, so a frame could start at +32/+64/... bytes into a 256 block and be sampled that many pixels
// shifted, with the end of the previous row wrapping around to the left edge. Every video frame now starts on a
// multiple of JSTARS_AV_ALIGN (default 256, power of two, 32..65536); a game allocator that ignores the request is
// handled by over-allocating and aligning inside the block.
#define AVP_TEX_ALIGN_DEFAULT 256u
static uint32_t avp_tex_align(void) __attribute__((unused));
static uint32_t avp_tex_align(void) {
    static uint32_t v = 0;
    if (!v) {
        char b[32];
        long n = (avp_cfg_text("ALIGN", b, sizeof b) && b[0]) ? (long)avp_num(b) : 0;
        if (n < 32 || n > 65536 || (n & (n - 1))) n = AVP_TEX_ALIGN_DEFAULT;
        v = (uint32_t)n;
    }
    return v;
}
// Returns the aligned frame buffer (>= need bytes), allocating or growing it when necessary.
static uint8_t* avp_tex_ensure(AvpPlayer* p, size_t need) __attribute__((unused));
static uint8_t* avp_tex_ensure(AvpPlayer* p, size_t need) {
    if (need == 0) need = 1;
    if (p->city_tex && p->city_tex_sz >= need) return (uint8_t*)p->city_tex;
    avp_buf_free(p, p->city_tex_raw, 1, p->city_tex_game);
    p->city_tex = NULL; p->city_tex_raw = NULL; p->city_tex_sz = 0;
    const uintptr_t al = (uintptr_t)avp_tex_align();
    void* raw = NULL; int game = 0;
    if (!avp_force_sysalloc() && p->alloc_tex) {
        raw = p->alloc_tex(p->mem_obj, (uint32_t)al, (uint32_t)need);
        if (raw && ((uintptr_t)raw & (al - 1))) {
            // the game's allocator did not honour the alignment: take a bigger block and align inside it
            if (p->dealloc_tex) p->dealloc_tex(p->mem_obj, raw);
            raw = p->alloc_tex(p->mem_obj, (uint32_t)al, (uint32_t)(need + al));
        }
        if (raw) game = 1;
    }
    if (!raw) {
#ifdef _WIN32
        raw = _aligned_malloc(need, (size_t)al);
#else
        if (posix_memalign(&raw, (size_t)al, need)) raw = NULL;
#endif
        game = 0;
    }
    if (!raw) return NULL;
    p->city_tex_game = game;
    p->city_tex_raw = raw;
    p->city_tex = (void*)(((uintptr_t)raw + al - 1) & ~(al - 1));
    p->city_tex_sz = need;
    return (uint8_t*)p->city_tex;
}

// ---- events -------------------------------------------------------------------------------------------
// Every event runs on its own short-lived guest thread, as the real library's controller thread does.
static void* GUEST_ABI avp_event_thread(void* arg) __attribute__((unused));
static void* GUEST_ABI avp_event_thread(void* arg) {
    uint64_t packed = (uint64_t)(uintptr_t)arg;
    uint64_t h = packed >> 8; int ev = (int)(packed & 0xff);
    if (ev == AVP_EVENT_READY) { struct timespec ts = {0, 20 * 1000 * 1000}; nanosleep(&ts, NULL); }
    AvpPlayer* p = avp_get(h);
    if (!p || !p->event_cb) return NULL;
    const char* nm = ev == 1 ? "STOP" : ev == 2 ? "READY" : ev == 3 ? "PLAY" : ev == 4 ? "PAUSE" : ev == 5 ? "BUFFERING" : "?";
    rt_logf("sceAvPlayer: event %s (%d) for '%s'", nm, ev, p->path);
    p->event_cb(p->event_obj, ev, 0, NULL);
    rt_logf("sceAvPlayer: event %s (%d) callback returned", nm, ev);
    return NULL;
}
static void avp_emit(uint64_t h, int ev) __attribute__((unused));
static void avp_emit(uint64_t h, int ev) {
    void* arg = (void*)(uintptr_t)((h << 8) | (uint64_t)(ev & 0xff));
    if (rt_spawn_guest_thread(avp_event_thread, arg, "AvPlayer Event"))
        rt_logf("sceAvPlayer: could not start the event thread; event %d will not be delivered", ev);
}
// READY is preceded by BUFFERING on the same thread, in order, like the console (AddSource returns
// before the asynchronous open completes).
static void* GUEST_ABI avp_ready_thread(void* arg) {
    uint64_t h = (uint64_t)(uintptr_t)arg;
    struct timespec ts = {0, 10 * 1000 * 1000}; nanosleep(&ts, NULL);
    AvpPlayer* p = avp_get(h);
    if (!p || !p->event_cb) return NULL;
    rt_logf("sceAvPlayer: event BUFFERING (5) for '%s'", p->path);
    p->event_cb(p->event_obj, AVP_EVENT_BUFFERING, 0, NULL);
    ts.tv_nsec = 20 * 1000 * 1000; nanosleep(&ts, NULL);
    p = avp_get(h);
    if (!p || !p->event_cb) return NULL;
    rt_logf("sceAvPlayer: event READY (2) for '%s' (%d stream(s))", p->path, p->n_streams);
    p->event_cb(p->event_obj, AVP_EVENT_READY, 0, NULL);
    rt_logf("sceAvPlayer: the game's READY callback returned");
    return NULL;
}

// ---- decoder backend ----------------------------------------------------------------------------------
// be_available() is 1 with -DJSTARS_HAS_FFMPEG, 0 otherwise (diagnostic stub: movies are skipped).
#ifdef JSTARS_HAS_FFMPEG
static int be_ff_logged;
static void be_ff_log_versions(void) {
    if (be_ff_logged) return; be_ff_logged = 1;
    rt_logf("sceAvPlayer: FFmpeg backend: %s (avformat %u, avcodec %u, avutil %u)", av_version_info(),
            avformat_version(), avcodec_version(), avutil_version());
}
static int be_available(void) { be_ff_log_versions(); return 1; }

typedef struct {
    AVFormatContext* fmt;
    AVCodecContext* vctx, * actx;
    int v_idx, a_idx;
    AVPacket** vq; size_t vq_n, vq_cap;   // queued packets for the video stream
    AVPacket** aq; size_t aq_n, aq_cap;   // queued packets for the audio stream
    int v_eof, a_eof;
    int64_t last_v_us, last_a_us;         // last decoded timestamps (AV_TIME_BASE), -1 = none yet
    int64_t last_ret_v_us;                // last timestamp handed to the game (monotonic guard)
    int16_t* pcm; size_t pcm_cap;         // staged audio frame (S16 interleaved)
    uint32_t cur_ch, cur_rate, cur_samples; int64_t cur_us; int have_frame;
    int width, height;
    int warned_vfmt, warned_afmt;
} AvpBe;

static void be_queue_push(AVPacket*** q, size_t* n, size_t* cap, AVPacket* pkt) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        AVPacket** nq = (AVPacket**)realloc(*q, nc * sizeof *nq);
        if (!nq) { av_packet_free(&pkt); return; }
        *q = nq; *cap = nc;
    }
    (*q)[(*n)++] = pkt;
}
static AVPacket* be_queue_pop(AVPacket*** q, size_t* n) {
    if (!*n) return NULL;
    AVPacket* pkt = (*q)[0];
    memmove(*q, *q + 1, (*n - 1) * sizeof **q);
    (*n)--;
    return pkt;
}
static void be_queue_drop(AVPacket*** q, size_t* n, size_t* cap) {
    while (*n) { AVPacket* pkt = be_queue_pop(q, n); av_packet_free(&pkt); }
    free(*q); *q = NULL; *cap = 0;
}
static int64_t be_rescale(int64_t ts, AVRational tb) {
    AVRational base = {1, 1000000};
    return av_rescale_q(ts, tb, base);
}
#if LIBAVCODEC_VERSION_MAJOR >= 59
static uint32_t be_par_channels(const AVCodecParameters* par) { return (uint32_t)par->ch_layout.nb_channels; }
#else
static uint32_t be_par_channels(const AVCodecParameters* par) { return (uint32_t)par->channels; }
#endif
#if LIBAVUTIL_VERSION_MAJOR >= 57
static uint32_t be_frame_channels(const AVFrame* f) { return (uint32_t)f->ch_layout.nb_channels; }
#else
static uint32_t be_frame_channels(const AVFrame* f) { return (uint32_t)f->channels; }
#endif

// Feeds one packet of stream `idx` to its decoder, reading ahead and queuing the other stream's
// packets. Returns 1 when a packet was sent, 0 when the file is exhausted, -1 on hard error.
static int be_send_one(AvpBe* b, int idx, AVCodecContext* ctx, AVPacket*** oq, size_t* on, size_t* ocap,
                       AVPacket*** qq, size_t* qn, size_t* qcap, int* eof) {
    AVPacket* pkt = be_queue_pop(qq, qn);
    if (pkt) { int r = avcodec_send_packet(ctx, pkt); av_packet_free(&pkt); return r == 0 ? 1 : (r == AVERROR(EAGAIN) ? 1 : -1); }
    for (;;) {
        pkt = av_packet_alloc();
        if (!pkt) return -1;
        int r = av_read_frame(b->fmt, pkt);
        if (r < 0) { av_packet_free(&pkt); *eof = 1; return 0; }
        if (pkt->stream_index == idx) {
            r = avcodec_send_packet(ctx, pkt); av_packet_free(&pkt);
            if (r == AVERROR(EAGAIN)) return 1;
            return r == 0 ? 1 : -1;
        }
        be_queue_push(oq, on, ocap, pkt);
    }
}
// Decodes the next audio frame into the staging area. Returns 1 on a frame, 0 at end of stream.
static int be_fill_audio(AvpBe* b) {
    if (b->a_idx < 0) return 0;
    AVStream* st = b->fmt->streams[b->a_idx];
    AVFrame* f = av_frame_alloc();
    if (!f) return 0;
    int got = 0;
    for (;;) {
        int r = avcodec_receive_frame(b->actx, f);
        if (r == 0) { got = 1; break; }
        if (r != AVERROR(EAGAIN)) break; // EOF or error: the stream is done
        int s = be_send_one(b, b->a_idx, b->actx, &b->vq, &b->vq_n, &b->vq_cap, &b->aq, &b->aq_n, &b->aq_cap, &b->a_eof);
        if (s <= 0) break;
    }
    if (!got) { av_frame_free(&f); return 0; }
    uint32_t ch = be_frame_channels(f);
    if (ch == 0 || ch > 8) ch = 2;
    size_t need = (size_t)f->nb_samples * ch * sizeof(int16_t);
    if (need > b->pcm_cap) { int16_t* np = (int16_t*)realloc(b->pcm, need); if (!np) { av_frame_free(&f); return 0; } b->pcm = np; b->pcm_cap = need; }
    if (!b->warned_afmt && f->format != AV_SAMPLE_FMT_FLTP && f->format != AV_SAMPLE_FMT_S16 &&
        f->format != AV_SAMPLE_FMT_S16P && f->format != AV_SAMPLE_FMT_FLT) {
        b->warned_afmt = 1;
        rt_logf("sceAvPlayer: unsupported audio sample format %d (only FLTP/S16/S16P/FLT); audio frames skipped", f->format);
    }
    if (f->format == AV_SAMPLE_FMT_FLTP) {
        for (int i = 0; i < f->nb_samples; ++i) for (uint32_t j = 0; j < ch; ++j) {
            float v = ((const float*)f->data[j])[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            b->pcm[i * ch + j] = (int16_t)(v * 32767.0f);
        }
    } else if (f->format == AV_SAMPLE_FMT_S16P) {
        for (int i = 0; i < f->nb_samples; ++i) for (uint32_t j = 0; j < ch; ++j)
            b->pcm[i * ch + j] = ((const int16_t*)f->data[j])[i];
    } else if (f->format == AV_SAMPLE_FMT_S16) {
        memcpy(b->pcm, f->data[0], need);
    } else if (f->format == AV_SAMPLE_FMT_FLT) {
        const float* s = (const float*)f->data[0];
        for (int i = 0; i < f->nb_samples * (int)ch; ++i) {
            float v = s[i]; if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            b->pcm[i] = (int16_t)(v * 32767.0f);
        }
    } else { av_frame_free(&f); return 0; }
    b->cur_ch = ch; b->cur_rate = (uint32_t)f->sample_rate; b->cur_samples = (uint32_t)f->nb_samples;
    b->cur_us = be_rescale(f->best_effort_timestamp, st->time_base);
    b->last_a_us = b->cur_us; b->have_frame = 1;
    av_frame_free(&f);
    return 1;
}
// Opens the file for real. Fills the player's stream table. Returns 0 on success.
static int be_open(AvpPlayer* p, const char* host_path) {
    AvpBe* b = (AvpBe*)calloc(1, sizeof *b);
    if (!b) return -1;
    b->v_idx = b->a_idx = -1; b->last_v_us = b->last_a_us = b->last_ret_v_us = -1;
    b->fmt = avformat_alloc_context();
    if (!b->fmt) { free(b); return -1; }
    if (avformat_open_input(&b->fmt, host_path, NULL, NULL) < 0) {
        avformat_close_input(&b->fmt); free(b); return -1;
    }
    avformat_find_stream_info(b->fmt, NULL); // best effort: improves duration/stream probing
    const AVCodec* vc = NULL; const AVCodec* ac = NULL;
    b->v_idx = av_find_best_stream(b->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vc, 0);
    b->a_idx = av_find_best_stream(b->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &ac, 0);
    if (b->v_idx >= 0 && vc) {
        AVCodecParameters* par = b->fmt->streams[b->v_idx]->codecpar;
        b->vctx = avcodec_alloc_context3(vc);
        if (b->vctx && avcodec_parameters_to_context(b->vctx, par) == 0 && avcodec_open2(b->vctx, vc, NULL) == 0) {
            b->width = b->vctx->width; b->height = b->vctx->height;
            rt_logf("sceAvPlayer: video stream %d: coded %dx%d fmt %d, decoding as %dx%d", b->v_idx,
                    par->width, par->height, par->format, b->width, b->height);
        } else {
            if (b->vctx) avcodec_free_context(&b->vctx);
            b->v_idx = -1;
        }
    }
    if (b->a_idx >= 0 && ac) {
        AVCodecParameters* par = b->fmt->streams[b->a_idx]->codecpar;
        b->actx = avcodec_alloc_context3(ac);
        if (b->actx && avcodec_parameters_to_context(b->actx, par) == 0 && avcodec_open2(b->actx, ac, NULL) == 0) {
            be_fill_audio(b); // pre-roll: learn the real channel count / sample rate for StreamInfo
        } else {
            if (b->actx) avcodec_free_context(&b->actx);
            b->a_idx = -1;
        }
    }
    if (b->v_idx < 0 && b->a_idx < 0) {
        avformat_close_input(&b->fmt); free(b->pcm); free(b); return -1;
    }
    if (b->fmt->duration > 0) p->duration_ms = (uint64_t)(b->fmt->duration / 1000);
    p->n_streams = 0;
    if (b->v_idx >= 0 && p->n_streams < AVP_MAX_STREAMS) {
        AvpStreamDesc* s = &p->streams[p->n_streams++];
        s->type = AVP_STREAM_VIDEO; s->w = (uint32_t)b->width; s->h = (uint32_t)b->height;
        s->aspect = avp_aspect_ovr((uint32_t)b->width, (uint32_t)b->height);
        memcpy(s->lang, "ENG", 4); s->ch = s->rate = s->frame_bytes = 0;
        s->duration_ms = p->duration_ms; s->start_ms = 0;
    }
    if (b->a_idx >= 0 && p->n_streams < AVP_MAX_STREAMS) {
        const AVCodecParameters* par = b->fmt->streams[b->a_idx]->codecpar;
        AvpStreamDesc* s = &p->streams[p->n_streams++];
        s->type = AVP_STREAM_AUDIO; s->w = s->h = 0; s->aspect = 0;
        memcpy(s->lang, "ENG", 4);
        s->ch = b->cur_ch ? b->cur_ch : be_par_channels(par);
        s->rate = b->cur_rate ? b->cur_rate : (uint32_t)par->sample_rate;
        s->frame_bytes = 0; // varies per frame; reported per GetAudioData
        s->duration_ms = p->duration_ms; s->start_ms = 0;
    }
    // Any further streams (subtitles, extras) are reported as unknown so the index space matches
    // the file's stream count, like the reference implementation.
    for (int i = p->n_streams; i < (int)b->fmt->nb_streams && i < AVP_MAX_STREAMS; ++i) {
        p->streams[p->n_streams++].type = AVP_STREAM_UNKNOWN;
    }
    p->be = b;
    return 0;
}
static void be_close(AvpPlayer* p) {
    AvpBe* b = (AvpBe*)p->be;
    if (!b) return;
    be_queue_drop(&b->vq, &b->vq_n, &b->vq_cap);
    be_queue_drop(&b->aq, &b->aq_n, &b->aq_cap);
    if (b->vctx) avcodec_free_context(&b->vctx);
    if (b->actx) avcodec_free_context(&b->actx);
    if (b->fmt) avformat_close_input(&b->fmt);
    free(b->pcm);
    free(b);
    p->be = NULL;
}
// Decodes the next video frame whose timestamp passes the audio clock (A/V sync, as the reference
// implementation does). On success fills *ts_us and copies NV12 (Y + interleaved UV) into `dst`
// of `dst_sz` bytes, each plane row padded to `stride` (JSTARS_AV_PITCH; default = width).
// Returns 1 on a frame, 0 when the video stream is exhausted.
static int be_next_video(AvpBe* b, uint8_t* dst, size_t dst_sz, uint32_t stride, int64_t* ts_us) {
    if (b->v_idx < 0) return 0;
    AVStream* st = b->fmt->streams[b->v_idx];
    if (stride < (uint32_t)b->width) stride = (uint32_t)b->width;
    size_t need = (size_t)stride * (size_t)b->height * 3 / 2;
    if (dst_sz < need || b->width <= 0 || b->height <= 0) return 0;
    AVFrame* f = av_frame_alloc();
    if (!f) return 0;
    int got = 0;
    for (;;) {
        int r = avcodec_receive_frame(b->vctx, f);
        if (r == 0) {
            int64_t ts = be_rescale(f->best_effort_timestamp, st->time_base);
            if (f->format != AV_PIX_FMT_YUV420P) {
                if (!b->warned_vfmt) {
                    b->warned_vfmt = 1;
                    rt_logf("sceAvPlayer: unsupported video pixel format %d (only YUV420P); video frames skipped", f->format);
                }
                continue;
            }
            // A/V sync: let the video catch up with the audio clock, never go backwards.
            if ((b->a_idx < 0 || ts >= b->last_a_us) && ts > b->last_ret_v_us) {
                uint8_t* d = dst;
                for (int y = 0; y < f->height; ++y) { memcpy(d, f->data[0] + (size_t)f->linesize[0] * (size_t)y, (size_t)f->width); d += stride; }
                for (int y = 0; y < f->height / 2; ++y) {
                    for (int x = 0; x < f->width / 2; ++x) {
                        *d++ = f->data[1][(size_t)f->linesize[1] * (size_t)y + (size_t)x];
                        *d++ = f->data[2][(size_t)f->linesize[2] * (size_t)y + (size_t)x];
                    }
                    d += stride - (uint32_t)f->width;
                }
                b->last_v_us = ts; b->last_ret_v_us = ts; *ts_us = ts;
                got = 1;
            } else if (ts <= b->last_ret_v_us && b->last_ret_v_us >= 0) {
                continue; // duplicate/old frame: decode ahead
            } else {
                continue; // video behind the audio clock: skip ahead
            }
            break;
        }
        if (r != AVERROR(EAGAIN)) break; // EOF or error
        int s = be_send_one(b, b->v_idx, b->vctx, &b->aq, &b->aq_n, &b->aq_cap, &b->vq, &b->vq_n, &b->vq_cap, &b->v_eof);
        if (s <= 0) break;
    }
    av_frame_free(&f);
    return got;
}
static int be_restart(AvpPlayer* p) {
    AvpBe* b = (AvpBe*)p->be;
    if (!b) return -1;
    if (av_seek_frame(b->fmt, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    if (b->vctx) avcodec_flush_buffers(b->vctx);
    if (b->actx) avcodec_flush_buffers(b->actx);
    be_queue_drop(&b->vq, &b->vq_n, &b->vq_cap);
    be_queue_drop(&b->aq, &b->aq_n, &b->aq_cap);
    b->v_eof = b->a_eof = 0; b->last_v_us = b->last_a_us = b->last_ret_v_us = -1;
    b->have_frame = 0;
    return 0;
}
#else
// ---- diagnostic-stub backend: no decoder ---------------------------------------------------------------
static int be_available(void) { return 0; }
#endif

static uint64_t GUEST_ABI avp_init(const void* init_data) {
    if (!init_data) { rt_logf("sceAvPlayerInit(NULL): rejected"); return 0; }
#ifdef JSTARS_HAS_FFMPEG
    rt_logf("sceAvPlayerInit(init data %p): FFmpeg backend (real demux/decode). Raw init data follows:", init_data);
#else
    rt_logf("sceAvPlayerInit(init data %p): NO DECODER - movies will not play (build without JSTARS_HAS_FFMPEG; see docs/MOVIES.md). Raw init data follows (layout from public SDK headers, unverified):", init_data);
#endif
    hexdump("sceAvPlayerInit data", init_data, 128);
    uint64_t w[16]; size_t got = os_safe_read(w, init_data, sizeof w);
    if (got >= sizeof w) {
        rt_logf("sceAvPlayerInit: allocate=%#" PRIx64 " deallocate=%#" PRIx64 " allocateTexture=%#" PRIx64 " deallocateTexture=%#" PRIx64
                " | file replacement: open=%#" PRIx64 " close=%#" PRIx64 " readOffset=%#" PRIx64 " size=%#" PRIx64
                " | event callback=%#" PRIx64, w[1], w[2], w[3], w[4], w[6], w[7], w[8], w[9], w[11]);
    }
    avp_lock();
    for (int i = 0; i < AVP_MAX; ++i) if (!g_avp[i].used) {
        memset(&g_avp[i], 0, sizeof g_avp[i]);
        g_avp[i].used = 1;
        if (got >= sizeof w) {
            g_avp[i].mem_obj = (void*)(uintptr_t)w[0];
            g_avp[i].alloc = (AvpAllocFn)(uintptr_t)w[1]; g_avp[i].dealloc = (AvpFreeFn)(uintptr_t)w[2];
            g_avp[i].alloc_tex = (AvpAllocFn)(uintptr_t)w[3]; g_avp[i].dealloc_tex = (AvpFreeFn)(uintptr_t)w[4];
            g_avp[i].file_obj = (void*)(uintptr_t)w[5];
            g_avp[i].f_open = (AvpFileOpenFn)(uintptr_t)w[6]; g_avp[i].f_close = (AvpFileCloseFn)(uintptr_t)w[7];
            g_avp[i].f_read = (AvpFileReadFn)(uintptr_t)w[8]; g_avp[i].f_size = (AvpFileSizeFn)(uintptr_t)w[9];
            g_avp[i].event_obj = (void*)(uintptr_t)w[10]; g_avp[i].event_cb = (AvpEventCb)(uintptr_t)w[11];
        }
        uint64_t h = AVP_HANDLE_BASE + (uint64_t)i + 1;
        rt_logf("sceAvPlayerInit -> player handle %#" PRIx64 " (opaque)", h);
        avp_unlock();
        return h;
    }
    avp_unlock();
    rt_logf("sceAvPlayerInit: all %d player slots are in use", AVP_MAX);
    return 0;
}

static int GUEST_ABI avp_init_ex(const void* init_ex, uint64_t* out_handle) {
    if (!init_ex || !out_handle) { rt_logf("sceAvPlayerInitEx(%p, %p): rejected (NULL)", init_ex, (void*)out_handle); return AVP_ERR_INVALID_PARAMS; }
    uint64_t w[32]; size_t got = os_safe_read(w, init_ex, sizeof w);
    // AvpInitDataEx field order differs from AvpInitData (this_size first, language earlier): pick the
    // allocator/event callbacks by their documented offsets (qwords 1..5, 6..10, 11..12).
    uint64_t h = 0;
    avp_lock();
    for (int i = 0; i < AVP_MAX && !h; ++i) if (!g_avp[i].used) {
        memset(&g_avp[i], 0, sizeof g_avp[i]);
        g_avp[i].used = 1;
        if (got >= 13 * sizeof(uint64_t)) {
            g_avp[i].mem_obj = (void*)(uintptr_t)w[1];
            g_avp[i].alloc = (AvpAllocFn)(uintptr_t)w[2]; g_avp[i].dealloc = (AvpFreeFn)(uintptr_t)w[3];
            g_avp[i].alloc_tex = (AvpAllocFn)(uintptr_t)w[4]; g_avp[i].dealloc_tex = (AvpFreeFn)(uintptr_t)w[5];
            g_avp[i].file_obj = (void*)(uintptr_t)w[6];
            g_avp[i].f_open = (AvpFileOpenFn)(uintptr_t)w[7]; g_avp[i].f_close = (AvpFileCloseFn)(uintptr_t)w[8];
            g_avp[i].f_read = (AvpFileReadFn)(uintptr_t)w[9]; g_avp[i].f_size = (AvpFileSizeFn)(uintptr_t)w[10];
            g_avp[i].event_obj = (void*)(uintptr_t)w[11]; g_avp[i].event_cb = (AvpEventCb)(uintptr_t)w[12];
        }
        h = AVP_HANDLE_BASE + (uint64_t)i + 1;
        rt_logf("sceAvPlayerInitEx -> player handle %#" PRIx64 " (opaque)", h);
    }
    avp_unlock();
    if (!h) { rt_logf("sceAvPlayerInitEx: all %d player slots are in use", AVP_MAX); return AVP_ERR_OPERATION_FAILED; }
    if (os_safe_write(out_handle, &h, sizeof h) != sizeof h) { avp_lock(); avp_get(h)->used = 0; avp_unlock(); return AVP_ERR_INVALID_PARAMS; }
    return 0;
}

static int GUEST_ABI avp_post_init(uint64_t h, const void* post) {
    (void)post;
    if (!avp_get(h)) return AVP_ERR_INVALID_PARAMS;
    rt_logf("sceAvPlayerPostInit(%#" PRIx64 "): accepted (no-op)", h);
    return 0;
}

static int GUEST_ABI avp_set_log_cb(const void* cb, void* user) {
    (void)cb; (void)user;
    rt_logf("sceAvPlayerSetLogCallback: accepted (player logs stay in the runtime log)");
    return 0;
}

// Adds a source by guest path. With the FFmpeg backend the file is opened for real (host path, or via
// the game's file-replacement callbacks when the path does not resolve under /app0); without it the
// source is accepted and logged exactly like before, with no streams.
static int GUEST_ABI avp_add_source(uint64_t h, const char* path) {
    AvpPlayer* p = avp_get(h);
    if (!p || !path) return AVP_ERR_INVALID_PARAMS;
    char name[512]; size_t got = os_safe_read(name, path, sizeof name - 1);
    name[got] = 0; name[sizeof name - 1] = 0;
    size_t len = strnlen(name, sizeof name - 1); name[len] = 0;
    avp_lock();
    snprintf(p->path, sizeof p->path, "%s", name);
    p->sources++; p->file_size = 0; p->brand[0] = 0;
#ifdef JSTARS_HAS_FFMPEG
    // A reused handle drops the previous movie first.
    be_close(p);
    avp_buffers_drop(p);
    p->n_streams = 0; p->duration_ms = 0;
    p->started = p->paused = p->finished = p->stop_sent = 0;
#endif
    char host[1536];
    int e = rt_fs_host_path(name, host, sizeof host);
    FILE* f = e ? NULL : os_fopen(host, "rb");
    if (f) {
        uint8_t hdr[16] = {0};
        size_t r = fread(hdr, 1, sizeof hdr, f);
        fseek(f, 0, SEEK_END); p->file_size = (uint64_t)ftell(f);
        fclose(f);
        if (r >= 12 && !memcmp(hdr + 4, "ftyp", 4)) { memcpy(p->brand, hdr + 8, 4); p->brand[4] = 0; }
    }
#ifdef JSTARS_HAS_FFMPEG
    int opened = 0;
    if (!e) {
        if (be_open(p, host) == 0) {
            opened = 1;
            rt_logf("sceAvPlayerAddSource('%s'): FFmpeg opened '%s', %" PRIu64 " bytes, MP4 brand '%s': %d stream(s), duration %.1f s",
                    name, host, p->file_size, p->brand[0] ? p->brand : "(not an MP4 ftyp header)",
                    p->n_streams, (double)p->duration_ms / 1000.0);
        } else {
            rt_logf("sceAvPlayerAddSource('%s'): the FFmpeg backend could not open '%s' (%" PRIu64 " bytes); reported with no streams",
                    name, host, p->file_size);
        }
    } else if (p->f_open && p->f_read && p->f_size) {
        // The game serves the bytes itself (encrypted/packed mounts): dump them through its callbacks
        // into the user's folder (like the reference implementation) and open the dump.
        char dump[1600]; const char* base = strrchr(name, '/');
        snprintf(dump, sizeof dump, "%s/avplayer_dump", rt_fs_user_root());
        os_mkdir(dump);
        uint64_t size = p->f_size(p->file_obj);
        snprintf(dump, sizeof dump, "%s/avplayer_dump/%llu_%s", rt_fs_user_root(),
                 (unsigned long long)size, base && base[1] ? base + 1 : "movie.mp4");
        if (p->f_open(p->file_obj, name) == 0 && size > 0 && size < (uint64_t)1 << 32) {
            FILE* out = os_fopen(dump, "wb");
            uint64_t off = 0; int fail = out == NULL;
            static uint8_t chunk[256 * 1024];
            while (!fail && off < size) {
                uint32_t want = (uint32_t)((size - off > sizeof chunk) ? sizeof chunk : size - off);
                int r = p->f_read(p->file_obj, chunk, off, want);
                if (r <= 0 || fwrite(chunk, 1, (size_t)r, out) != (size_t)r) { fail = 1; break; }
                off += (uint64_t)r;
                if ((uint32_t)r < want) break;
            }
            if (out) fclose(out);
            p->f_close(p->file_obj);
            if (!fail && be_open(p, dump) == 0) {
                opened = 1;
                rt_logf("sceAvPlayerAddSource('%s'): read %" PRIu64 " bytes through the game's file callbacks, FFmpeg opened the dump: %d stream(s)",
                        name, off, p->n_streams);
            } else if (!fail) {
                rt_logf("sceAvPlayerAddSource('%s'): dump written but FFmpeg could not open it; reported with no streams", name);
            } else {
                rt_logf("sceAvPlayerAddSource('%s'): the game's file callbacks failed; reported with no streams", name);
            }
        } else {
            rt_logf("sceAvPlayerAddSource('%s'): path does not resolve under /app0 and the game's file open failed; reported with no streams", name);
        }
    } else {
        rt_logf("sceAvPlayerAddSource('%s'): file cannot be opened (%s); a source is still accepted. NOT DECODED either way", name, e ? "path does not resolve under /app0" : "open failed");
    }
#else
    if (f) {
        rt_logf("sceAvPlayerAddSource('%s'): file exists, %" PRIu64 " bytes, MP4 brand '%s'. NOT DECODED: the movie is reported as having no streams and ends immediately",
                name, p->file_size, p->brand[0] ? p->brand : "(not an MP4 ftyp header)");
    } else {
        rt_logf("sceAvPlayerAddSource('%s'): file cannot be opened (%s); a source is still accepted. NOT DECODED either way", name, e ? "path does not resolve under /app0" : "open failed");
    }
#endif
    int has_cb = p->event_cb != NULL;
    avp_unlock();
#ifdef JSTARS_HAS_FFMPEG
    if (has_cb) {
        if (opened) {
            if (rt_spawn_guest_thread(avp_ready_thread, (void*)(uintptr_t)h, "AvPlayer Event"))
                rt_logf("sceAvPlayerAddSource: could not start the event thread; READY will not be delivered");
        } else {
            avp_emit(h, AVP_EVENT_READY); // no media: the game still expects READY, then sees no streams
        }
    } else rt_logf("sceAvPlayerAddSource: the game gave no event callback; no READY event is delivered");
#else
    if (has_cb) {
        if (rt_spawn_guest_thread(avp_ready_thread, (void*)(uintptr_t)h, "AvPlayer Event"))
            rt_logf("sceAvPlayerAddSource: could not start the event thread; READY will not be delivered");
    } else rt_logf("sceAvPlayerAddSource: the game gave no event callback; no READY event is delivered");
#endif
    return 0;
}

static int GUEST_ABI avp_add_source_ex(uint64_t h, int uri_type, const void* details) {
    (void)uri_type;
    if (!avp_get(h) || !details) return AVP_ERR_INVALID_PARAMS;
    AvpSourceDetails d; size_t got = os_safe_read(&d, details, sizeof d);
    if (got < sizeof d.uri) return AVP_ERR_INVALID_PARAMS;
    char name[512];
    uint32_t len = d.uri.len < sizeof name - 1 ? d.uri.len : (uint32_t)sizeof name - 1;
    size_t rn = os_safe_read(name, d.uri.name, len);
    name[rn] = 0;
    rt_logf("sceAvPlayerAddSourceEx(type %d, '%s')", uri_type, name);
    // The URI is a plain console path for local files (d.uri.name still points at guest memory);
    // reuse the AddSource path.
    return avp_add_source(h, d.uri.name);
}

static int GUEST_ABI avp_stream_count(uint64_t h) {
    AvpPlayer* p = avp_get(h);
    if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock(); int n = p->n_streams; avp_unlock();
    return n;
}

static int GUEST_ABI avp_get_stream_info(uint64_t h, uint32_t idx, void* info) {
    AvpPlayer* p = avp_get(h);
    if (!p || !info) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
    if (idx >= (uint32_t)p->n_streams) { avp_unlock(); return AVP_ERR_INVALID_PARAMS; }
    AvpStreamDesc* s = &p->streams[idx];
    AvpStreamInfo si; memset(&si, 0, sizeof si);
    si.type = s->type; si.align = 0;
    if (s->type == AVP_STREAM_VIDEO) {
        si.det.video.w = s->w; si.det.video.h = s->h; si.det.video.aspect = s->aspect;
        memcpy(si.det.video.lang, s->lang, 4);
        rt_logf("sceAvPlayerGetStreamInfo(%u): video %ux%u aspect %.3f", idx, s->w, s->h, s->aspect);
    } else if (s->type == AVP_STREAM_AUDIO) {
        si.det.audio.ch = (uint16_t)s->ch; si.det.audio.rate = s->rate;
        si.det.audio.size = s->ch * 1024 * (uint32_t)sizeof(int16_t); // typical frame; exact size comes per GetAudioData
        memcpy(si.det.audio.lang, s->lang, 4);
        rt_logf("sceAvPlayerGetStreamInfo(%u): audio %u ch %u Hz", idx, s->ch, s->rate);
    } else {
        si.type = AVP_STREAM_UNKNOWN;
    }
    si.duration = s->duration_ms; si.start = s->start_ms;
    AvpStreamInfo* dst = (AvpStreamInfo*)info;
    *dst = si;
    avp_unlock();
    return 0;
}

static int GUEST_ABI avp_enable_stream(uint64_t h, uint32_t idx) {
    (void)idx;
    AvpPlayer* p = avp_get(h);
    if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock(); int ok = p->n_streams > 0 && idx < (uint32_t)p->n_streams; avp_unlock();
    return ok ? 0 : AVP_ERR_INVALID_PARAMS;
}

static int GUEST_ABI avp_disable_stream(uint64_t h, uint32_t idx) {
    (void)idx;
    AvpPlayer* p = avp_get(h);
    if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock(); int ok = p->n_streams > 0 && idx < (uint32_t)p->n_streams; avp_unlock();
    return ok ? 0 : AVP_ERR_INVALID_PARAMS;
}

static int GUEST_ABI avp_start(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
#ifdef JSTARS_HAS_FFMPEG
    if (!p->be) {
        avp_unlock();
        rt_logf("sceAvPlayerStart('%s'): no media (source could not be opened); nothing plays", p->path);
        return AVP_ERR_OPERATION_FAILED;
    }
    p->started = 1; p->paused = 0; p->finished = 0; p->stop_sent = 0;
    avp_unlock();
    rt_logf("sceAvPlayerStart('%s'): playing (%d stream(s))", p->path, p->n_streams);
    avp_emit(h, AVP_EVENT_PLAY);
    return 0;
#else
    p->started = 1;
    avp_unlock();
    rt_logf("sceAvPlayerStart('%s'): nothing is played (no decoder)", p->path);
    return 0;
#endif
}

static int GUEST_ABI avp_stop(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
    p->started = 0;
#ifdef JSTARS_HAS_FFMPEG
    int had_media = p->be != NULL;
    be_close(p);
    avp_buffers_drop(p);
    p->finished = 0;
    avp_unlock();
    rt_logf("sceAvPlayerStop('%s'): stopped%s", p->path, had_media ? "" : " (no media was open)");
    if (had_media && !p->stop_sent) { p->stop_sent = 1; avp_emit(h, AVP_EVENT_STOP); }
#else
    avp_unlock();
#endif
    return 0;
}

static int GUEST_ABI avp_pause(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
#ifdef JSTARS_HAS_FFMPEG
    if (!p->be || !p->started) { avp_unlock(); return AVP_ERR_OPERATION_FAILED; }
    p->paused = 1;
    avp_unlock();
    avp_emit(h, AVP_EVENT_PAUSE);
#else
    avp_unlock();
    rt_logf("sceAvPlayerPause('%s'): recorded (no decoder)", p->path);
#endif
    return 0;
}

static int GUEST_ABI avp_resume(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
#ifdef JSTARS_HAS_FFMPEG
    if (!p->be || !p->started) { avp_unlock(); return AVP_ERR_OPERATION_FAILED; }
    p->paused = 0;
    avp_unlock();
    avp_emit(h, AVP_EVENT_PLAY);
#else
    avp_unlock();
    rt_logf("sceAvPlayerResume('%s'): recorded (no decoder)", p->path);
#endif
    return 0;
}

static int GUEST_ABI avp_set_looping(uint64_t h, int loop) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
    p->looping = loop & 1;
#ifdef JSTARS_HAS_FFMPEG
    avp_unlock();
    rt_logf("sceAvPlayerSetLooping(%d) for '%s'", p->looping, p->path);
#else
    avp_unlock();
    if (loop) rt_logf("sceAvPlayerSetLooping(true) for '%s': recorded; with no stream the player is still inactive", p->path);
#endif
    return 0;
}

static int GUEST_ABI avp_set_av_sync(uint64_t h, int mode) {
    if (!avp_get(h)) return AVP_ERR_INVALID_PARAMS;
    rt_logf("sceAvPlayerSetAvSyncMode(%d): accepted (audio-clock sync is always on)", mode);
    return 0;
}

static uint64_t GUEST_ABI avp_current_time(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return 0;
#ifdef JSTARS_HAS_FFMPEG
    avp_lock();
    AvpBe* b = (AvpBe*)p->be;
    uint64_t ms = 0;
    if (b && b->last_ret_v_us >= 0) ms = (uint64_t)(b->last_ret_v_us / 1000);
    else if (b && b->last_a_us >= 0) ms = (uint64_t)(b->last_a_us / 1000);
    avp_unlock();
    return ms;
#else
    (void)p;
    return 0;
#endif
}

static int GUEST_ABI avp_jump_to_time(uint64_t h, uint64_t ms) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
#ifdef JSTARS_HAS_FFMPEG
    avp_lock();
    AvpBe* b = (AvpBe*)p->be;
    int r = -1;
    if (b) {
        r = av_seek_frame(b->fmt, -1, (int64_t)ms * 1000, AVSEEK_FLAG_BACKWARD);
        if (r >= 0) {
            if (b->vctx) avcodec_flush_buffers(b->vctx);
            if (b->actx) avcodec_flush_buffers(b->actx);
            be_queue_drop(&b->vq, &b->vq_n, &b->vq_cap);
            be_queue_drop(&b->aq, &b->aq_n, &b->aq_cap);
            b->v_eof = b->a_eof = 0; b->have_frame = 0;
            b->last_ret_v_us = (int64_t)ms * 1000;
            rt_logf("sceAvPlayerJumpToTime(%" PRIu64 " ms) for '%s': done", ms, p->path);
        } else rt_logf("sceAvPlayerJumpToTime(%" PRIu64 " ms) for '%s': seek failed", ms, p->path);
    }
    avp_unlock();
    return r >= 0 ? 0 : AVP_ERR_OPERATION_FAILED;
#else
    (void)p; (void)ms;
    return 0;
#endif
}

static int GUEST_ABI avp_is_active(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return 0;
#ifdef JSTARS_HAS_FFMPEG
    avp_lock();
    int active = p->be && p->started && !p->paused && !p->finished;
    avp_unlock();
    return active;
#else
    if (p->calls_active++ == 0) rt_logf("sceAvPlayerIsActive('%s'): false (no decoder: movie treated as finished)", p->path);
    return 0;
#endif
}

// One-time debug dump: converts the first decoded NV12 frame to a 24-bit BMP in
// <user>/avplayer_dump so a rotated/torn picture can be bisected into "our buffer
// is wrong" (BMP shows the seam) vs "the game presents it shifted" (BMP is clean).
static void avp_dump_frame(AvpPlayer* p, const uint8_t* nv12, int w, int h, uint64_t n, uint32_t stride) {
    (void)p;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !nv12) return;
    if (stride < (uint32_t)w) stride = (uint32_t)w;
    char dir[1536], path[1600];
    snprintf(dir, sizeof dir, "%s/avplayer_dump", rt_fs_user_root());
    os_mkdir(dir);
    snprintf(path, sizeof path, "%s/frame%llu_%dx%d.bmp", dir, (unsigned long long)n, w, h);
    FILE* f = os_fopen(path, "wb");
    if (!f) { rt_logf("sceAvPlayer: could not write first-frame BMP '%s'", path); return; }
    uint32_t row = (uint32_t)(((w * 3 + 3) / 4) * 4);
    uint32_t img = row * (uint32_t)h;
    uint8_t hdr[54]; memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    { uint32_t fs = 54 + img; memcpy(hdr + 2, &fs, 4); }
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    const uint8_t* Y = nv12; const uint8_t* UV = nv12 + (size_t)stride * h;
    uint8_t* line = (uint8_t*)malloc(row ? row : 1);
    if (line) {
        for (int y = h - 1; y >= 0; --y) {
            for (int x = 0; x < w; ++x) {
                int c = (int)Y[(size_t)y * stride + x] - 16;
                int d = (int)UV[((size_t)(y / 2)) * stride + ((size_t)(x / 2)) * 2] - 128;
                int e = (int)UV[((size_t)(y / 2)) * stride + ((size_t)(x / 2)) * 2 + 1] - 128;
                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b2 = (298 * c + 516 * d + 128) >> 8;
                line[x * 3 + 0] = (uint8_t)(b2 < 0 ? 0 : b2 > 255 ? 255 : b2);
                line[x * 3 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
                line[x * 3 + 2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            }
            fwrite(line, 1, row, f);
        }
        free(line);
    }
    fclose(f);
    rt_logf("sceAvPlayer: wrote frame %llu BMP '%s' (%dx%d)", (unsigned long long)n, path, w, h);
}

// Returns 1 and fills the guest FrameInfoEx when a frame was produced, 0 when there is none yet.
static int GUEST_ABI avp_get_video_data_ex(uint64_t h, void* info) {
    AvpPlayer* p = avp_get(h); if (!p) return 0;
    if (!info) return 0;
#ifdef JSTARS_HAS_FFMPEG
    avp_lock();
    AvpBe* b = (AvpBe*)p->be;
    if (!b || !p->started || p->paused || p->finished) { avp_unlock(); return 0; }
    uint32_t pitch = avp_pitch_ovr((uint32_t)b->width);
    float aspect = avp_aspect_ovr((uint32_t)b->width, (uint32_t)b->height);
    size_t need = (size_t)pitch * (size_t)b->height * 3 / 2;
    uint8_t* dst = avp_tex_ensure(p, need ? need : 1);
    if (!dst) { avp_unlock(); return 0; }
    int64_t ts = -1;
    int got = be_next_video(b, dst, need, pitch, &ts);
    if (!got) {
        if (p->looping && be_restart(p) == 0) {
            dst = avp_tex_ensure(p, need ? need : 1);
            if (dst) got = be_next_video(b, dst, need, pitch, &ts);
        }
        if (!got) {
            p->finished = 1;
            avp_unlock();
            if (p->frames_video == 0) rt_logf("sceAvPlayerGetVideoDataEx('%s'): end of video immediately; movie treated as finished", p->path);
            else rt_logf("sceAvPlayerGetVideoDataEx('%s'): end of video after %" PRIu64 " frames; movie finished", p->path, p->frames_video);
            return 0;
        }
    }
    AvpFrameInfoEx* fi = (AvpFrameInfoEx*)info;
    AvpFrameInfoEx out; memset(&out, 0, sizeof out);
    out.data = dst; out.ts = (uint64_t)(ts / 1000);
    out.det.video.w = (uint32_t)b->width; out.det.video.h = (uint32_t)b->height;
    out.det.video.aspect = aspect;
    memcpy(out.det.video.lang, "ENG", 4);
    out.det.video.framerate = 0;
    out.det.video.pitch = pitch;
    out.det.video.luma_bits = 8; out.det.video.chroma_bits = 8; out.det.video.full_range = 0;
    *fi = out;
    {
        int xo = avp_xoff(), yo = avp_yoff(), sw = avp_swapuv(), tm = avp_testmode();
        if (tm) avp_fill_test(dst, (uint32_t)b->width, (uint32_t)b->height, pitch, tm);
        if (xo || yo || sw) avp_apply_shift(dst, (uint32_t)b->width, (uint32_t)b->height, pitch, xo, yo, sw);
        if (p->frames_video == 30 || p->frames_video == 150)
            avp_dump_frame(p, dst, b->width, b->height, p->frames_video, pitch);
        p->frames_video++;
        if (p->frames_video == 1) {
            rt_logf("sceAvPlayerGetVideoDataEx('%s'): first frame %dx%d pitch %u aspect %.3f alloc %s xoff %d yoff %d swapuv %d test %d", p->path, b->width, b->height, pitch, aspect, p->city_tex_game ? "game" : "own", xo, yo, sw, tm);
            rt_logf("sceAvPlayerGetVideoDataEx: frame buffer %#" PRIx64 " (raw %#" PRIx64 ", mod 256 = %u, requested alignment %u), chroma plane at +%" PRIu64 " (mod 256 = %u)",
                    (uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)p->city_tex_raw, (unsigned)((uintptr_t)dst & 255u), (unsigned)avp_tex_align(),
                    (uint64_t)pitch * (uint64_t)b->height, (unsigned)(((uint64_t)pitch * (uint64_t)b->height) & 255u));
        }
    }
    avp_unlock();
    return 1;
#else
    if (p->calls_video++ == 0) rt_logf("sceAvPlayerGetVideoDataEx('%s'): no frame (no decoder)", p->path);
    return 0;
#endif
}

// sceAvPlayerGetVideoData: the smaller variant. Converts the Ex frame (drops the extra fields).
static int GUEST_ABI avp_get_video_data(uint64_t h, void* info) {
    if (!info) return 0;
    AvpFrameInfoEx ex; memset(&ex, 0, sizeof ex);
    int got = avp_get_video_data_ex(h, &ex);
    if (!got) return 0;
    AvpFrameInfo* fi = (AvpFrameInfo*)info;
    AvpFrameInfo out; memset(&out, 0, sizeof out);
    out.data = ex.data; out.ts = ex.ts;
    out.det.video.w = ex.det.video.w; out.det.video.h = ex.det.video.h;
    out.det.video.aspect = ex.det.video.aspect;
    memcpy(out.det.video.lang, ex.det.video.lang, 4);
    *fi = out;
    return 1;
}

// Returns 1 and fills the guest FrameInfo when decoded audio was produced, 0 otherwise.
static int GUEST_ABI avp_get_audio_data(uint64_t h, void* info) {
    AvpPlayer* p = avp_get(h); if (!p) return 0;
    if (!info) return 0;
#ifdef JSTARS_HAS_FFMPEG
    avp_lock();
    AvpBe* b = (AvpBe*)p->be;
    if (!b || !p->started || p->paused || p->finished || b->a_idx < 0) { avp_unlock(); return 0; }
    if (!b->have_frame && !be_fill_audio(b)) {
        if (p->looping && be_restart(p) == 0) be_fill_audio(b);
        if (!b->have_frame) { avp_unlock(); return 0; }
    }
    size_t need = (size_t)b->cur_samples * b->cur_ch * sizeof(int16_t);
    int16_t* dst = (int16_t*)avp_buf_ensure(p, &p->city_pcm, &p->city_pcm_sz, &p->city_pcm_game, need ? need : 1, 0);
    if (!dst) { avp_unlock(); return 0; }
    memcpy(dst, b->pcm, need);
    int64_t ts = b->cur_us;
    b->have_frame = 0;
    AvpFrameInfo* fi = (AvpFrameInfo*)info;
    AvpFrameInfo out; memset(&out, 0, sizeof out);
    out.data = (uint8_t*)dst; out.ts = (uint64_t)(ts / 1000);
    out.det.audio.ch = (uint16_t)b->cur_ch; out.det.audio.rate = b->cur_rate;
    out.det.audio.size = (uint32_t)need;
    memcpy(out.det.audio.lang, "ENG", 4);
    *fi = out;
    p->frames_audio++;
    avp_unlock();
    return 1;
#else
    AvpPlayer* q = avp_get(h);
    if (q && q->calls_audio++ == 0) rt_logf("sceAvPlayerGetAudioData('%s'): no audio (no decoder)", q->path);
    return 0;
#endif
}

static int GUEST_ABI avp_close(uint64_t h) {
    AvpPlayer* p = avp_get(h); if (!p) return AVP_ERR_INVALID_PARAMS;
    avp_lock();
#ifdef JSTARS_HAS_FFMPEG
    be_close(p);
    avp_buffers_drop(p);
    uint64_t fa = p->frames_audio, fv = p->frames_video;
    uint64_t ca = p->calls_active, cv = p->calls_video;
    avp_unlock();
    rt_logf("sceAvPlayerClose('%s'): %d source(s), %" PRIu64 " video + %" PRIu64 " audio frames (%" PRIu64 " IsActive, %" PRIu64 " GetVideoDataEx polls)",
            p->path, p->sources, fv, fa, ca, cv);
    avp_lock();
#else
    rt_logf("sceAvPlayerClose('%s'): %d source(s) were added, %" PRIu64 " IsActive and %" PRIu64 " GetVideoDataEx polls", p->path, p->sources, p->calls_active, p->calls_video);
#endif
    p->used = 0;
    avp_unlock();
    return 0;
}

const HostSymbol avplayer_symbols[] = {
    {"sceAvPlayerInit", "libSceAvPlayer", (void*)avp_init, 0},
    {"sceAvPlayerInitEx", "libSceAvPlayer", (void*)avp_init_ex, 0},
    {"sceAvPlayerPostInit", "libSceAvPlayer", (void*)avp_post_init, 0},
    {"sceAvPlayerAddSource", "libSceAvPlayer", (void*)avp_add_source, 0},
    {"sceAvPlayerAddSourceEx", "libSceAvPlayer", (void*)avp_add_source_ex, 0},
    {"sceAvPlayerStreamCount", "libSceAvPlayer", (void*)avp_stream_count, 0},
    {"sceAvPlayerGetStreamInfo", "libSceAvPlayer", (void*)avp_get_stream_info, 0},
    {"sceAvPlayerEnableStream", "libSceAvPlayer", (void*)avp_enable_stream, 0},
    {"sceAvPlayerDisableStream", "libSceAvPlayer", (void*)avp_disable_stream, 0},
    {"sceAvPlayerStart", "libSceAvPlayer", (void*)avp_start, 0},
    {"sceAvPlayerStop", "libSceAvPlayer", (void*)avp_stop, 0},
    {"sceAvPlayerPause", "libSceAvPlayer", (void*)avp_pause, 0},
    {"sceAvPlayerResume", "libSceAvPlayer", (void*)avp_resume, 0},
    {"sceAvPlayerSetLooping", "libSceAvPlayer", (void*)avp_set_looping, 0},
    {"sceAvPlayerSetAvSyncMode", "libSceAvPlayer", (void*)avp_set_av_sync, 0},
    {"sceAvPlayerCurrentTime", "libSceAvPlayer", (void*)avp_current_time, 0},
    {"sceAvPlayerJumpToTime", "libSceAvPlayer", (void*)avp_jump_to_time, 0},
    {"sceAvPlayerSetLogCallback", "libSceAvPlayer", (void*)avp_set_log_cb, 0},
    {"sceAvPlayerIsActive", "libSceAvPlayer", (void*)avp_is_active, 0},
    {"sceAvPlayerGetVideoData", "libSceAvPlayer", (void*)avp_get_video_data, 0},
    {"sceAvPlayerGetVideoDataEx", "libSceAvPlayer", (void*)avp_get_video_data_ex, 0},
    {"sceAvPlayerGetAudioData", "libSceAvPlayer", (void*)avp_get_audio_data, 0},
    {"sceAvPlayerClose", "libSceAvPlayer", (void*)avp_close, 0},
};
const size_t avplayer_symbol_count = sizeof avplayer_symbols / sizeof *avplayer_symbols;

// Self-test of the player state machine (host tests; no game files needed). Without the FFmpeg
// backend every source reports no streams (movies are skipped); with it, the no-media paths below
// still hold and only the Start/media expectations differ.
int GUEST_ABI rt_avplayer_selftest(int (GUEST_ABI* check)(int ok, const char* what)) {
    int bad = 0;
#define C(ok, what) do { if (!check((ok), (what))) ++bad; } while (0)
    C(sizeof(AvpInitData) == 120, "avplayer: SceAvPlayerInitData is 120 bytes (packed Sony ABI)");
    C(sizeof(AvpInitDataEx) == 196, "avplayer: SceAvPlayerInitDataEx is 196 bytes (packed Sony ABI)");
    C(sizeof(AvpStreamInfo) == 40, "avplayer: SceAvPlayerStreamInfo is 40 bytes (packed Sony ABI)");
    C(sizeof(AvpFrameInfo) == 40, "avplayer: SceAvPlayerFrameInfo is 40 bytes (packed Sony ABI)");
    C(sizeof(AvpFrameInfoEx) == 104, "avplayer: SceAvPlayerFrameInfoEx is 104 bytes (packed Sony ABI)");
    C(sizeof(AvpSourceDetails) == 128, "avplayer: SceAvPlayerSourceDetails is 128 bytes (packed Sony ABI)");
    C(avp_init(NULL) == 0, "avplayer: Init(NULL) is rejected");
    C(avp_init_ex(NULL, NULL) == AVP_ERR_INVALID_PARAMS, "avplayer: InitEx(NULL) is INVALID_PARAMS");
    C(avp_post_init(0x41560001u, NULL) == AVP_ERR_INVALID_PARAMS, "avplayer: PostInit on an unknown handle is INVALID_PARAMS");
    C(avp_set_log_cb(NULL, NULL) == 0, "avplayer: SetLogCallback is accepted");
    uint64_t init[16] = {0};
    uint64_t h = avp_init(init);
    C(h > AVP_HANDLE_BASE, "avplayer: Init returns a handle");
    C(avp_add_source(h + 100, "/app0/x.mp4") == AVP_ERR_INVALID_PARAMS, "avplayer: AddSource on an unknown handle is INVALID_PARAMS");
    C(avp_add_source(h, NULL) == AVP_ERR_INVALID_PARAMS, "avplayer: AddSource(NULL path) is INVALID_PARAMS");
    C(avp_add_source_ex(h + 100, 0, NULL) == AVP_ERR_INVALID_PARAMS, "avplayer: AddSourceEx on an unknown handle is INVALID_PARAMS");
    C(avp_add_source(h, "/app0/does-not-exist.mp4") == 0, "avplayer: AddSource accepts a source that cannot be opened (logged)");
    C(avp_stream_count(h + 100) == AVP_ERR_INVALID_PARAMS, "avplayer: StreamCount on an unknown handle is INVALID_PARAMS");
    C(avp_stream_count(h) == 0, "avplayer: no streams are reported (no decoder)");
    AvpStreamInfo si; memset(&si, 0, sizeof si);
    C(avp_get_stream_info(h + 100, 0, &si) == AVP_ERR_INVALID_PARAMS, "avplayer: GetStreamInfo on an unknown handle is INVALID_PARAMS");
    C(avp_get_stream_info(h, 0, &si) == AVP_ERR_INVALID_PARAMS, "avplayer: GetStreamInfo with no streams is INVALID_PARAMS");
    C(avp_get_stream_info(h, 0, NULL) == AVP_ERR_INVALID_PARAMS, "avplayer: GetStreamInfo(NULL info) is INVALID_PARAMS");
    C(avp_enable_stream(h + 100, 0) == AVP_ERR_INVALID_PARAMS, "avplayer: EnableStream on an unknown handle is INVALID_PARAMS");
    C(avp_enable_stream(h, 0) == AVP_ERR_INVALID_PARAMS, "avplayer: EnableStream with no streams is INVALID_PARAMS");
    C(avp_disable_stream(h + 100, 0) == AVP_ERR_INVALID_PARAMS, "avplayer: DisableStream on an unknown handle is INVALID_PARAMS");
    C(avp_disable_stream(h, 0) == AVP_ERR_INVALID_PARAMS, "avplayer: DisableStream with no streams is INVALID_PARAMS");
    if (be_available()) {
        C(avp_start(h) == AVP_ERR_OPERATION_FAILED, "avplayer: Start with no open media reports OPERATION_FAILED (FFmpeg)");
        C(avp_pause(h) == AVP_ERR_OPERATION_FAILED, "avplayer: Pause with no open media reports OPERATION_FAILED (FFmpeg)");
        C(avp_resume(h) == AVP_ERR_OPERATION_FAILED, "avplayer: Resume with no open media reports OPERATION_FAILED (FFmpeg)");
        C(avp_jump_to_time(h + 100, 0) == AVP_ERR_INVALID_PARAMS, "avplayer: JumpToTime on an unknown handle is INVALID_PARAMS");
        C(avp_jump_to_time(h, 0) == AVP_ERR_OPERATION_FAILED, "avplayer: JumpToTime with no open media reports OPERATION_FAILED (FFmpeg)");
    } else {
        C(avp_start(h) == 0 && avp_is_active(h) == 0, "avplayer: after Start the player is not active (no decoder)");
        C(avp_pause(h) == 0 && avp_resume(h) == 0, "avplayer: Pause/Resume are recorded (no decoder)");
        C(avp_jump_to_time(h, 0) == 0, "avplayer: JumpToTime is recorded (no decoder)");
    }
    C(avp_is_active(h + 100) == 0, "avplayer: IsActive on an unknown handle is false");
    C(avp_set_av_sync(h + 100, 0) == AVP_ERR_INVALID_PARAMS, "avplayer: SetAvSyncMode on an unknown handle is INVALID_PARAMS");
    C(avp_set_av_sync(h, 0) == 0, "avplayer: SetAvSyncMode is accepted");
    C(avp_current_time(h + 100) == 0 && avp_current_time(h) == 0, "avplayer: CurrentTime is 0 with no media");
    C(avp_set_looping(h + 100, 1) == AVP_ERR_INVALID_PARAMS, "avplayer: SetLooping on an unknown handle is INVALID_PARAMS");
    C(avp_set_looping(h, 1) == 0, "avplayer: SetLooping is recorded");
    C(avp_get_video_data_ex(h, NULL) == 0, "avplayer: GetVideoDataEx(NULL info) returns no frame");
    C(avp_get_video_data(h, NULL) == 0, "avplayer: GetVideoData(NULL info) returns no frame");
    C(avp_get_audio_data(h, NULL) == 0, "avplayer: GetAudioData(NULL info) returns no audio");
    C(avp_get_video_data_ex(h + 100, &si) == 0, "avplayer: GetVideoDataEx on an unknown handle returns no frame");
    C(avp_get_audio_data(h + 100, &si) == 0, "avplayer: GetAudioData on an unknown handle returns no audio");
    C(avp_stop(h + 100) == AVP_ERR_INVALID_PARAMS, "avplayer: Stop on an unknown handle is INVALID_PARAMS");
    C(avp_stop(h) == 0, "avplayer: Stop succeeds");
    C(avp_close(h) == 0 && avp_close(h) == AVP_ERR_INVALID_PARAMS, "avplayer: Close once, then the handle is invalid");
#undef C
    return bad;
}
