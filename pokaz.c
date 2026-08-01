#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <wchar.h>
#include <shellapi.h>

#include <GL/gl.h>

#define SPNG_USE_MINIZ
#define SPNG_STATIC
#include "deps/spng.h"
#include "deps/miniz.h"

#define LIBJPEG_TURBO_STATIC
#include <turbojpeg.h>
#pragma comment(lib, "turbojpeg-static")

#pragma comment(lib, "user32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "opengl32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "windowscodecs")

//
// DEFINES
//

#define DEBUG_TO_CONSOLE

#define UNUSED(val)       ((void)(val))
#define ARRAY_LENGTH(arr) (sizeof((arr)) / sizeof((arr)[0]))

#define WINDOW_START_H (1024)
#define WINDOW_START_W (768)

#define FILES_MAX_COUNT (65535)

#define IMAGE_MAX_W         (4096)
#define IMAGE_MAX_H         (4096)
#define IMAGE_PREALLOC_SIZE (IMAGE_MAX_W * IMAGE_MAX_H * 4)

#define WM_DECODED_IMAGE_READY (WM_APP + 1)

static const wchar_t *SUPPORTED_EXTENSIONS[] = {
    L".jpg", L".jpeg", L".png", L".bmp", L".gif",
    L".tiff", L".tif", L".ico", L".webp", L".wdp",
    L".hdp", L".jxr"};

//
// STATE
//

struct file_list
{
    wchar_t *paths[FILES_MAX_COUNT];
    wchar_t *data;
    size_t   data_used_chars;
    uint16_t count;
    uint16_t current;
};

// TODO : better naming this
struct render_state
{
    GLuint tex;
    int    w, h;

    int   rotation;
    float zoom;

    int   drag_start_x, drag_start_y;
    float pan_start_x, pan_start_y;
    float pan_x, pan_y;
    bool  dragging;
};

static int g_win_w = WINDOW_START_W;
static int g_win_h = WINDOW_START_H;

static struct file_list    g_files  = {0};
static struct render_state g_render = {0};

static IWICImagingFactory *g_wic       = NULL;
static tjhandle            g_tj_handle = NULL;

//
// DEBUG
//

static void dprintf(const char *fmt, ...)
{
#ifdef DEBUG
    char buffer[512];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

#ifdef DEBUG_TO_CONSOLE
    fputs(buffer, stdout);
#else
    OutputDebugStringA(buffer);
#endif

#else
    UNUSED(fmt);
#endif
}

//
// TIMING
//

static inline double time_in_ms(void)
{
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);

    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
}

//
// MEMORY
//

static inline void *vmalloc(size_t size)
{
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static inline void vfree(void *ptr)
{
    VirtualFree(ptr, 0, MEM_RELEASE);
}

//
// OPENGL
//

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE (0x812F)
#endif

#ifndef GL_BGRA
#define GL_BGRA (0x80E1)
#endif

struct gl_context
{
    HDC   hdc;
    HGLRC hglrc;
};

static struct gl_context g_gl = {0};

static inline bool opengl_init(HWND hwnd)
{
    g_gl.hdc = GetDC(hwnd);
    if (!g_gl.hdc) return false;

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize                 = sizeof(pfd);
    pfd.nVersion              = 1;
    pfd.dwFlags               = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType            = PFD_TYPE_RGBA;
    pfd.cColorBits            = 32;
    pfd.cDepthBits            = 24;
    pfd.cStencilBits          = 8;
    pfd.iLayerType            = PFD_MAIN_PLANE;

    int pixel_format = ChoosePixelFormat(g_gl.hdc, &pfd);
    if (!pixel_format) return false;

    if (!SetPixelFormat(g_gl.hdc, pixel_format, &pfd)) return false;

    g_gl.hglrc = wglCreateContext(g_gl.hdc);
    if (!g_gl.hglrc) return false;

    if (!wglMakeCurrent(g_gl.hdc, g_gl.hglrc)) return false;

    glClearColor(0.0f, 0.3686f, 0.7216f, 1.0f); // pantone 300 C
    // glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    return true;
}

static void opengl_cleanup(HWND hwnd)
{
    if (g_render.tex != 0)
    {
        glDeleteTextures(1, &g_render.tex);
        g_render.tex = 0;
    }

    if (g_gl.hglrc)
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_gl.hglrc);
        g_gl.hglrc = NULL;
    }

    if (g_gl.hdc)
    {
        ReleaseDC(hwnd, g_gl.hdc);
        g_gl.hdc = NULL;
    }
}

typedef enum
{
    PIXEL_FORMAT_RGB8,  // 3 bytes/px: R,G,B
    PIXEL_FORMAT_RGBA8, // 4 bytes/px: R,G,B,A
    PIXEL_FORMAT_BGRA8, // 4 bytes/px: B,G,R,A
} pixel_format_t;

static GLuint opengl_texture_upload(BYTE *buf, UINT w, UINT h, pixel_format_t format)
{
    GLenum gl_format       = GL_RGBA;
    GLenum internal_format = GL_RGBA8;
    GLint  alignment       = 4;

    switch (format)
    {
        case PIXEL_FORMAT_RGB8:
            gl_format       = GL_RGB;
            internal_format = GL_RGB8;
            alignment       = 1;
            break;
        case PIXEL_FORMAT_RGBA8:
            gl_format       = GL_RGBA;
            internal_format = GL_RGBA8;
            alignment       = 4;
            break;
        case PIXEL_FORMAT_BGRA8:
            gl_format       = GL_BGRA;
            internal_format = GL_RGBA8;
            alignment       = 4;
            break;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);

    // dprintf("texture_upload : alloc + upload : %.3fms\n", time);

    return tex;
}

//
// WIC
//

static HRESULT wic_init(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        return hr;
    }

    hr = CoCreateInstance(&CLSID_WICImagingFactory,
                          NULL,
                          CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory,
                          (void **)&g_wic);
    return hr;
}

static void wic_cleanup(void)
{
    if (g_wic) IWICImagingFactory_Release(g_wic);
    CoUninitialize();
}

static bool wic_decode(const wchar_t *path, BYTE *out_buf, UINT *out_w, UINT *out_h, pixel_format_t *out_format)
{
    if (!path || !out_buf || !out_w || !out_h) return false;

    IWICBitmapDecoder     *decoder = NULL;
    IWICBitmapFrameDecode *frame   = NULL;
    IWICFormatConverter   *conv    = NULL;

    bool result = false;

    double time_start = time_in_ms();

    HRESULT hr;
    hr = IWICImagingFactory_CreateDecoderFromFilename(g_wic, path, NULL, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) goto done;

    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) goto done;

    hr = IWICImagingFactory_CreateFormatConverter(g_wic, &conv);
    if (FAILED(hr)) goto done;

    hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                        &GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0,
                                        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto done;

    IWICFormatConverter_GetSize(conv, out_w, out_h);

    hr = IWICFormatConverter_CopyPixels(conv, NULL,
                                        (*out_w) * 4,
                                        (*out_w) * (*out_h) * 4,
                                        out_buf);
    if (FAILED(hr)) goto done;

    *out_format = PIXEL_FORMAT_BGRA8;
    result      = true;

done:
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);

    if (0)
    {
        double time_elapsed = time_in_ms() - time_start;
        dprintf("decode_wic: %ux%u %.3fms, %s\n", *out_w, *out_h, time_elapsed, result ? "OK" : "ERROR");
    }

    return result;
}

//
// TURBO JPEG
//

static bool turbojpeg_decode(tjhandle handle, const wchar_t *path, BYTE *buf, UINT *out_w, UINT *out_h, pixel_format_t *out_format)
{
    if (!path || !buf || !out_w || !out_h) return false;

    BYTE  *file_buf = NULL;
    bool   result   = false;
    HANDLE f        = INVALID_HANDLE_VALUE;

    double time_start = time_in_ms();

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) goto done;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(f, &file_size)) goto done;

    // TODO : prealloc
    file_buf = vmalloc((size_t)file_size.QuadPart);
    if (!file_buf) goto done;

    DWORD bytes_read = 0;
    if (!ReadFile(f, file_buf, (DWORD)file_size.QuadPart, &bytes_read, NULL) ||
        bytes_read != (DWORD)file_size.QuadPart) goto done;

    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;

    int w, h, subsamp, colour_space;
    if (tjDecompressHeader3(handle, file_buf, (unsigned long)bytes_read,
                            &w, &h, &subsamp, &colour_space) < 0) goto done;

    if (tjDecompress2(handle, file_buf, (unsigned long)bytes_read,
                      buf, w, 0, h, TJPF_BGRA, TJFLAG_FASTDCT) < 0) goto done;

    *out_w      = (UINT)w;
    *out_h      = (UINT)h;
    *out_format = PIXEL_FORMAT_BGRA8; // TJPF_BGRA

    result = true;

done:
    if (f != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f);
    }

    if (file_buf)
    {
        vfree(file_buf);
    }

    if (!result)
    {
        dprintf("decode_jpeg_turbo: %s\n", tjGetErrorStr2(handle));
    }

    if (0)
    {
        double time_elapsed = time_in_ms() - time_start;
        dprintf("turbojpeg_decode: %ux%u %.3fms, %s\n", *out_w, *out_h, time_elapsed, result ? "OK" : "ERROR");
    }

    return result;
}

//
// SPNG
//

static bool decode_png_spng(const wchar_t *path, BYTE **out_buf, UINT *out_w, UINT *out_h, pixel_format_t *out_format)
{
    if (!path || !out_buf || !out_w || !out_h) return false;

    BYTE     *file_buf = NULL;
    spng_ctx *ctx      = NULL;
    bool      result   = false;
    HANDLE    f        = INVALID_HANDLE_VALUE;

    double time_start = time_in_ms();

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) goto done;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(f, &file_size)) goto done;

    file_buf = vmalloc((size_t)file_size.QuadPart);
    if (!file_buf) goto done;

    DWORD bytes_read = 0;
    if (!ReadFile(f, file_buf, (DWORD)file_size.QuadPart, &bytes_read, NULL) ||
        bytes_read != (DWORD)file_size.QuadPart) goto done;

    CloseHandle(f);
    f = INVALID_HANDLE_VALUE;

    ctx = spng_ctx_new(0);
    if (!ctx) goto done;

    if (spng_set_png_buffer(ctx, file_buf, (size_t)bytes_read) != 0) goto done;

    struct spng_ihdr ihdr = {0};
    if (spng_get_ihdr(ctx, &ihdr) != 0) goto done;

    bool has_alpha = (ihdr.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ||
                      ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA);

    int fmt = has_alpha ? SPNG_FMT_RGBA8 : SPNG_FMT_RGB8;

    size_t out_size = 0;
    if (spng_decoded_image_size(ctx, fmt, &out_size) != 0) goto done;

    if (spng_decode_image(ctx, *out_buf, out_size, fmt, 0) != 0) goto done;

    *out_w = (UINT)ihdr.width;
    *out_h = (UINT)ihdr.height;

    *out_format = has_alpha ? PIXEL_FORMAT_RGBA8 : PIXEL_FORMAT_RGB8;

    result = true;

done:
    if (f != INVALID_HANDLE_VALUE)
    {
        CloseHandle(f);
    }

    if (file_buf)
    {
        vfree(file_buf);
    }

    if (ctx)
    {
        spng_ctx_free(ctx);
    }

    if (0)
    {
        double time_elapsed = time_in_ms() - time_start;
        dprintf("decode_png_spng: %ux%u %.3fms, %s\n", *out_w, *out_h, time_elapsed, result ? "OK" : "ERROR");
    }

    return result;
}

//
// DECODE
//

static bool decode_image(tjhandle tj_handle, const wchar_t *path, BYTE *buf, UINT *w, UINT *h, pixel_format_t *out_format)
{
    bool result = false;

    // TODO : we could save the extension info when loading the paths?
    //  enum { IMAGE_JPG ... }
    const wchar_t *ext = wcsrchr(path, L'.');
    if (ext)
    {
        if (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0)
        {
            result = turbojpeg_decode(tj_handle, path, buf, w, h, out_format);
        }

        // try again / defualt with wic
        if (!result) result = wic_decode(path, buf, w, h, out_format);
    }
    else
    {
        dprintf("decode_image : ext fail\n");
    }

    return result;
}

//
// CACHE
//

#define CACHE_CAPACITY (5) // current, next, previous
#define THREAD_COUNT   (2)

enum cache_state
{
    CACHE_EMPTY,
    CACHE_NEEDS_DECODING,
    CACHE_DECODING,
    CACHE_READY
};

struct thread_info
{
    int      id;
    tjhandle tj_handle;
};

struct cache_entry
{
    uint16_t         index;
    enum cache_state state;

    pixel_format_t format;
    BYTE          *pixels;
    UINT           w, h;
};

struct image_cache
{
    CRITICAL_SECTION   lock;
    struct cache_entry entries[CACHE_CAPACITY];
    int                target_index; // The index the user is currently looking at

    HANDLE threads[THREAD_COUNT];
    HANDLE semaphore;
    bool   running;
};

static struct image_cache g_cache = {0};

static DWORD WINAPI background_prefetch_thread(LPVOID arg)
{
    HWND     hwnd      = (HWND)arg;
    tjhandle tj_handle = tjInitDecompress();
    DWORD    thread_id = GetCurrentThreadId();

    for (;;)
    {
        WaitForSingleObject(g_cache.semaphore, INFINITE);
        if (!g_cache.running) break;

        int  i     = 0;
        bool found = false;
        EnterCriticalSection(&g_cache.lock);
        for (; i < CACHE_CAPACITY; i++)
        {
            if (g_cache.entries[i].state == CACHE_NEEDS_DECODING)
            {
                g_cache.entries[i].state = CACHE_DECODING;
                found                    = true;
                break;
            }
        }
        LeaveCriticalSection(&g_cache.lock);
        if (!found) continue;

        // target is not in the cache
        int target = g_cache.entries[i].index;
        // dprintf("thread %u decoding %d\n", thread_id, target);

        bool res = decode_image(tj_handle, g_files.paths[target],
                                g_cache.entries[i].pixels,
                                &g_cache.entries[i].w,
                                &g_cache.entries[i].h,
                                &g_cache.entries[i].format);
        if (res)
        {
            // TODO : do we need to lock here?
            EnterCriticalSection(&g_cache.lock);

            g_cache.entries[i].state = CACHE_READY;

            PostMessageW(hwnd, WM_DECODED_IMAGE_READY, 0, (LPARAM)&g_cache.entries[i]);

            LeaveCriticalSection(&g_cache.lock);
        }
    }

    if (tj_handle) tjDestroy(tj_handle);

    return 0;
}
static void cache_reset(void)
{
    EnterCriticalSection(&g_cache.lock);

    for (uint16_t i = 0; i < CACHE_CAPACITY; i++)
    {
        g_cache.entries[i].index = (uint16_t)-1;
        g_cache.entries[i].state = CACHE_EMPTY;

        if (!g_cache.entries[i].pixels)
            g_cache.entries[i].pixels = vmalloc(IMAGE_PREALLOC_SIZE);
    }

    LeaveCriticalSection(&g_cache.lock);
}

static void cache_init(HWND hwnd)
{
    InitializeCriticalSection(&g_cache.lock);
    g_cache.semaphore    = CreateSemaphore(NULL, 0, CACHE_CAPACITY, NULL);
    g_cache.target_index = 0;
    g_cache.running      = true;

    cache_reset();

    for (uint16_t i = 0; i < THREAD_COUNT; i++)
    {
        g_cache.threads[i] = CreateThread(NULL, 0, background_prefetch_thread, (LPVOID)hwnd, 0, NULL);
    }
}

static void cache_cleanup(void)
{
    g_cache.running = false;
    ReleaseSemaphore(g_cache.semaphore, THREAD_COUNT, NULL);

    DWORD result = WaitForMultipleObjects(THREAD_COUNT, g_cache.threads, TRUE, INFINITE);
    if (result == WAIT_FAILED)
    {
        DWORD err = GetLastError();
        dprintf("WaitForMultipleObjects failed: %lu\n", err);
    }

    for (uint16_t i = 0; i < THREAD_COUNT; i++)
    {
        if (!CloseHandle(g_cache.threads[i])) dprintf("g_cache.threads[%u] close issue\n", i);
    }

    if (!CloseHandle(g_cache.semaphore)) dprintf("g_cache.semaphore close issue\n");

    for (uint16_t i = 0; i < CACHE_CAPACITY; i++)
    {
        if (g_cache.entries[i].pixels)
        {
            vfree(g_cache.entries[i].pixels);
        }
    }

    DeleteCriticalSection(&g_cache.lock);
}

//
// SLIDESHOW
//

struct slideshow
{
    uint16_t speed_index;
    bool     active;
};

static const int SLIDESHOW_SPEEDS[] = {500, 1000, 2000, 3000, 5000, 8000, 12000}; // ms
#define SLIDESHOW_SPEED_COUNT   ARRAY_LENGTH(SLIDESHOW_SPEEDS)
#define SLIDESHOW_TIMER_ID      (1)
#define SLIDESHOW_SPEED_DEFAULT (2)

static struct slideshow g_slide = {.speed_index = SLIDESHOW_SPEED_DEFAULT};

static void slideshow_rearm(HWND hWnd)
{
    if (g_slide.active)
    {
        KillTimer(hWnd, SLIDESHOW_TIMER_ID);
        SetTimer(hWnd, SLIDESHOW_TIMER_ID, SLIDESHOW_SPEEDS[g_slide.speed_index], NULL);
    }
}

static void slideshow_start(HWND hWnd)
{
    dprintf("slideshow START\n");

    if (g_files.count <= 1) return;

    g_slide.active = true;
    slideshow_rearm(hWnd);
}

static void slideshow_stop(HWND hWnd)
{
    dprintf("slideshow STOP\n");

    g_slide.active = false;
    KillTimer(hWnd, SLIDESHOW_TIMER_ID);
}

static inline void slideshow_toggle(HWND hWnd)
{
    g_slide.active ? slideshow_stop(hWnd) : slideshow_start(hWnd);
}

static void slideshow_advance(void)
{
    if (g_files.count <= 1) return;

    // same as a "right" move
    g_files.current = (g_files.current + 1) % g_files.count;
}

//
// RENDER
//

static inline void image_reset_view(void)
{
    g_render.zoom     = 0.0f;
    g_render.pan_x    = 0.0f;
    g_render.pan_y    = 0.0f;
    g_render.rotation = 0;
}

static uint16_t image_map_index(int offset)
{
    if (g_files.count == 0) return 0;

    int count   = (int)g_files.count;
    int current = (int)g_files.current;

    int pos = (current + offset) % count;
    if (pos < 0) pos += count;


    return (uint16_t)pos;
}

static void image_show_current(void)
{
    if (g_files.count == 0) return;

    image_reset_view();
    uint16_t current_idx = image_map_index(0);

    dprintf("image_show_current : %u\n", current_idx);

    BYTE          *pixels_to_upload = NULL;
    pixel_format_t format           = 0;
    UINT           w = 0, h = 0;
    bool           cache_hit = false;

    EnterCriticalSection(&g_cache.lock);
    {
        for (int j = 0; j < CACHE_CAPACITY; j++)
        {
            if (g_cache.entries[j].index == current_idx &&
                g_cache.entries[j].state == CACHE_READY)
            {
                pixels_to_upload = g_cache.entries[j].pixels;
                w                = g_cache.entries[j].w;
                h                = g_cache.entries[j].h;
                format           = g_cache.entries[j].format;
                cache_hit        = true;
                break;
            }
        }
    }

    // rebuild the cache around the current index
    {
        // TODO : this is very hardcoded, should be based on CACHE_CAPACITY
        uint16_t to_cache[5] = {
            image_map_index(0),  // Current image
            image_map_index(1),  // Next image
            image_map_index(-1), // Previous image
            image_map_index(2),  // +2 images ahead
            image_map_index(-2)  // -2 images behind
        };

        // when less images than the cache size
        int active_targets = (g_files.count < 5) ? g_files.count : 5;
        for (int t = 0; t < active_targets; t++)
        {
            uint16_t target = to_cache[t];

            bool already_cached = false;
            for (uint16_t j = 0; j < CACHE_CAPACITY; j++)
            {
                if (g_cache.entries[j].index == target &&
                    g_cache.entries[j].state != CACHE_EMPTY)
                {
                    already_cached = true;
                    break;
                }
            }
            if (already_cached) continue;

            // Find a slot whose index is NOT in to_cache[] - safe to evict
            for (uint16_t j = 0; j < CACHE_CAPACITY; j++)
            {
                uint16_t cached_idx = g_cache.entries[j].index;

                if (cached_idx != to_cache[0] &&
                    cached_idx != to_cache[1] &&
                    cached_idx != to_cache[2] &&
                    cached_idx != to_cache[3] &&
                    cached_idx != to_cache[4])
                {
                    g_cache.entries[j].state = CACHE_NEEDS_DECODING;
                    g_cache.entries[j].index = target;
                    ReleaseSemaphore(g_cache.semaphore, 1, NULL);
                    break;
                }
            }
        }
    }
    LeaveCriticalSection(&g_cache.lock);

    if (cache_hit)
    {
        // dprintf("image_show_current : cache hit!\n");
        if (g_render.tex != 0) glDeleteTextures(1, &g_render.tex);
        g_render.tex = opengl_texture_upload(pixels_to_upload, w, h, format);
        g_render.w   = w;
        g_render.h   = h;
    }
    else
    {
        ReleaseSemaphore(g_cache.semaphore, 1, NULL);
    }
}
}

static void image_render(void)
{
    glViewport(0, 0, g_win_w, g_win_h);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!g_render.tex || g_files.count == 0) return;

    int is_sideways = (g_render.rotation % 180 != 0);
    int fit_w       = is_sideways ? g_render.h : g_render.w;
    int fit_h       = is_sideways ? g_render.w : g_render.h;

    float sx    = (float)g_win_w / (float)fit_w;
    float sy    = (float)g_win_h / (float)fit_h;
    float fit   = (sx < sy) ? sx : sy;
    float scale = fit * powf(1.15f, g_render.zoom);

    float dw = g_render.w * scale;
    float dh = g_render.h * scale;

    float cx = g_win_w * 0.5f + g_render.pan_x;
    float cy = g_win_h * 0.5f + g_render.pan_y;

    float x0 = cx - dw * 0.5f, y0 = cy - dh * 0.5f;
    float x1 = cx + dw * 0.5f, y1 = cy + dh * 0.5f;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_win_w, g_win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Rotate unrotated quad around center
    glTranslatef(cx, cy, 0.0f);
    glRotatef((float)g_render.rotation, 0.0f, 0.0f, 1.0f);
    glTranslatef(-cx, -cy, 0.0f);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_render.tex);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0.0f, 0.0f);
        glVertex2f(x0, y0);

        glTexCoord2f(1.0f, 0.0f);
        glVertex2f(x1, y0);

        glTexCoord2f(1.0f, 1.0f);
        glVertex2f(x1, y1);

        glTexCoord2f(0.0f, 1.0f);
        glVertex2f(x0, y1);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

//
// DIRECTORY
//

static int is_ext_supported(const wchar_t *name)
{
    const wchar_t *dot = wcsrchr(name, L'.');
    if (!dot) return 0;

    for (size_t i = 0; i < ARRAY_LENGTH(SUPPORTED_EXTENSIONS); ++i)
    {
        if (_wcsicmp(dot, SUPPORTED_EXTENSIONS[i]) == 0)
            return 1;
    }
    return 0;
}

static void scan_directory(const wchar_t *dir, size_t len)
{
    dprintf("scan_directory : %ls (%u)\n", dir, len);

    if (!g_files.data)
        g_files.data = vmalloc(FILES_MAX_COUNT * MAX_PATH * sizeof(wchar_t));

    g_files.count   = 0;
    g_files.current = 0;

    wchar_t pattern[MAX_PATH] = {0};
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE           hf = FindFirstFileExW(pattern,
                                           FindExInfoBasic,
                                           &fd,
                                           FindExSearchNameMatch,
                                           NULL,
                                           0);

    // pattern would look like : "images\4",
    // dir look like           : "images\4\*",
    // len + 1 is to include the "\" we just added
    size_t base_len = len + 1;

    if (hf == INVALID_HANDLE_VALUE) return;

    do
    {
        if (g_files.count >= FILES_MAX_COUNT) break;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (!is_ext_supported(fd.cFileName))
            continue;

        size_t file_len  = wcslen(fd.cFileName);
        size_t total_len = base_len + file_len + 1; // +1 for null terminator

        if (total_len > MAX_PATH)
            continue; // or truncate/log, but don't write past the slot

        wchar_t *full                = g_files.data + (g_files.count * MAX_PATH);
        g_files.paths[g_files.count] = full;

        // TODO : could we save the base somewhere, then just append the file when loading?
        wmemcpy(full, pattern, base_len);
        wmemcpy(full + base_len, fd.cFileName, file_len + 1); // include \0

        g_files.data_used_chars += total_len;
        g_files.count++;

    } while (FindNextFileW(hf, &fd));

    FindClose(hf);
}

static void scan_from_path(const wchar_t *path)
{
    // TODO : timing

    dprintf("scan_from_path : %ls\n", path);

    wchar_t raw[MAX_PATH] = {0};
    wcscpy_s(raw, MAX_PATH, path);

    // strip trailing quotes and backslashes
    size_t rlen = wcslen(raw);
    while (rlen > 0 &&
           ((raw[rlen - 1] == L'"') || (raw[rlen - 1] == L'\\')))
    {
        raw[--rlen] = L'\0';
    }

    // resolve to a full, absolute path so relative CLI args
    // (e.g. "image_3.png" or "..\images\9\image_3.png") match
    // the absolute paths returned by FindFirstFileExW later
    wchar_t full_clean_path[MAX_PATH] = {0};
    if (!GetFullPathNameW(raw, MAX_PATH, full_clean_path, NULL))
        return; // couldn't resolve — bad path

    wchar_t dir[MAX_PATH] = {0};
    wcscpy_s(dir, MAX_PATH, full_clean_path);
    size_t len = wcslen(dir);

    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        scan_directory(dir, len);
    }
    else
    {
        wchar_t *sep = wcsrchr(dir, L'\\');
        if (sep)
        {
            *sep = L'\0';
            len  = wcslen(dir);
            scan_directory(dir, len);
        }
        // sep == NULL should now be unreachable, since
        // GetFullPathNameW always returns a drive/UNC-rooted path
    }

    for (uint16_t i = 0; i < g_files.count; i++)
    {
        if (_wcsicmp(g_files.paths[i], full_clean_path) == 0)
        {
            g_files.current = i;
            break;
        }
    }

    if (1)
    {
        dprintf("Loaded images from: '%ls'\n", path);
        for (uint16_t i = 0; i < g_files.count; i++)
        {
            dprintf("    %ls %s\n", g_files.paths[i], g_files.current == i ? "<--" : "");
        }
    }
}

//
// WIN MAIN/PROC
//

static void window_update_title(HWND hWnd)
{
    if (g_files.count == 0)
    {
        SetWindowTextW(hWnd, L"Pokaz - no images found");
        return;
    }

    const wchar_t *path = g_files.paths[g_files.current];
    const wchar_t *name = wcsrchr(path, L'\\');
    name                = name ? name + 1 : path;

    uint16_t current_idx = image_map_index(0) + 1;

    wchar_t title[256] = {0};
    if (g_slide.active)
    {
        double secs = SLIDESHOW_SPEEDS[g_slide.speed_index] / 1000.0;

        swprintf_s(title, 256, L"[\u25b6 %.1fs] [%d / %d] %s (%d \u00d7 %d)",
                   secs,
                   g_files.current + 1, g_files.count,
                   name,
                   g_render.w, g_render.h);
    }
    else
    {
        swprintf_s(title, 256, L"[%d / %d] %s (%d \u00d7 %d)",
                   g_files.current + 1, g_files.count,
                   name,
                   g_render.w, g_render.h);
    }

#if 1
    wchar_t cache_state[64] = {0};
    EnterCriticalSection(&g_cache.lock);
    swprintf_s(cache_state, 64, L"[%u, %u, %u, %u, %u]",
               g_cache.entries[0].index,
               g_cache.entries[1].index,
               g_cache.entries[2].index,
               g_cache.entries[3].index,
               g_cache.entries[4].index);
    LeaveCriticalSection(&g_cache.lock);

    wchar_t full[256] = {0};
    swprintf_s(full, 256, L"%s %s", cache_state, title);
    SetWindowTextW(hWnd, full);
#else
    SetWindowTextW(hWnd, title);
#endif
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_DECODED_IMAGE_READY:
        {
            struct cache_entry *entry = (struct cache_entry *)lParam;

            // can only get here on CACHE_READY state
            EnterCriticalSection(&g_cache.lock);
            uint16_t current_index = image_map_index(0);
            if (entry->index == current_index)
            {
                dprintf("WM_DECODED_IMAGE_READY %d\n", entry->index);

                if (g_render.tex != 0) glDeleteTextures(1, &g_render.tex);
                g_render.tex = opengl_texture_upload(entry->pixels, entry->w, entry->h, entry->format);
                g_render.w   = entry->w;
                g_render.h   = entry->h;

                InvalidateRect(hwnd, NULL, FALSE);
            }
            LeaveCriticalSection(&g_cache.lock);
            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == SLIDESHOW_TIMER_ID)
            {
                slideshow_advance();
                image_show_current();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_KEYDOWN:
            switch (wParam)
            {
                case VK_ESCAPE:
                {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    break;
                }
                case VK_RIGHT:
                case VK_NEXT:
                case 'D':
                {
                    if (g_files.count > 0)
                    {
                        g_files.current = (g_files.current + 1) % g_files.count;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case VK_LEFT:
                case VK_PRIOR:
                case 'A':
                {
                    if (g_files.count > 0)
                    {
                        g_files.current = (g_files.current + g_files.count - 1) % g_files.count;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case VK_HOME:
                {
                    if (g_files.count > 0)
                    {
                        g_files.current = 0;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case VK_END:
                {
                    if (g_files.count > 0)
                    {
                        g_files.current = g_files.count - 1;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case 'R':
                {
                    g_render.rotation = (g_render.rotation + 90) % 360;
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case 'Z':
                {
                    // TODO : zoom limits
                    g_render.zoom += 1.0f;
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case 'X':
                {
                    // TODO : zoom limits
                    g_render.zoom -= 1.0f;
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case '0':
                {
                    image_reset_view();
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case VK_SPACE:
                {
                    slideshow_toggle(hwnd);
                    // image_show_current();
                    window_update_title(hwnd);
                    break;
                }
                case VK_OEM_PLUS:
                case VK_ADD:
                {
                    if (g_slide.speed_index > 0)
                    {
                        g_slide.speed_index--;
                        slideshow_rearm(hwnd);
                        window_update_title(hwnd);
                    }
                    break;
                }
                case VK_OEM_MINUS:
                case VK_SUBTRACT:
                {
                    if (g_slide.speed_index < (SLIDESHOW_SPEED_COUNT - 1))
                    {
                        g_slide.speed_index++;
                        slideshow_rearm(hwnd);
                        window_update_title(hwnd);
                    }
                    break;
                }
            }
            break;

        case WM_LBUTTONDOWN:
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            // mouse down in the main image - start pan drag
            g_render.dragging     = true;
            g_render.drag_start_x = mx;
            g_render.drag_start_y = my;
            g_render.pan_start_x  = g_render.pan_x;
            g_render.pan_start_y  = g_render.pan_y;

            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            // int mx = GET_X_LPARAM(lParam);
            // int my = GET_Y_LPARAM(lParam);

            g_render.dragging = false;
            ReleaseCapture();
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            if (g_render.dragging)
            {
                g_render.pan_x = g_render.pan_start_x + (float)(mx - g_render.drag_start_x);
                g_render.pan_y = g_render.pan_start_y + (float)(my - g_render.drag_start_y);

                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);

            POINT lpPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            BOOL  res     = ScreenToClient(hwnd, &lpPoint);

            if (res)
            {
                g_render.zoom += (delta > 0) ? 1.0f : -1.0f;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_DROPFILES:
        {
            HDROP   hDrop         = (HDROP)wParam;
            wchar_t buf[MAX_PATH] = {0};

            // for now, only care about the first file dropped
            if (DragQueryFileW(hDrop, 0, buf, MAX_PATH))
            {
                // we need to rebuild and invalidate the cache
                scan_from_path(buf);
                cache_reset();
                image_show_current();
                InvalidateRect(hwnd, NULL, FALSE);
            }

            DragFinish(hDrop);
            return 0;
        }

        case WM_ERASEBKGND:
            // prevent Windows from clearing background with GDI white brush
            return 1;

        case WM_SIZE:
        {
            g_win_w = LOWORD(lParam);
            g_win_h = HIWORD(lParam);
            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps = {0};
            BeginPaint(hwnd, &ps);

            window_update_title(hwnd);
            image_render();
            SwapBuffers(g_gl.hdc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
        {
            dprintf("Shutting down : WIC\n");
            wic_cleanup();

            dprintf("Shutting down : opengl\n");
            opengl_cleanup(hwnd);

            dprintf("Shutting down : cache\n");
            cache_cleanup();

            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    UNUSED(hPrevInstance);
    UNUSED(pCmdLine);
    UNUSED(nCmdShow);

#ifdef DEBUG
    if (AllocConsole())
    {
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
#endif

    HRESULT hr = wic_init();
    if (FAILED(hr))
    {
        dprintf("wic_init failed: 0x%08X\n", (unsigned int)hr);
        return 1; // Exit early since WIC isn't usable
    }

    int     argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2)
    {
        scan_from_path(argv[1]);
        LocalFree(argv);
    }

    LPCWSTR class_name  = L"PokazWC";
    LPCWSTR window_name = L"Pokaz";

    WNDCLASSEXW wc   = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = class_name;

    if (!RegisterClassExW(&wc))
    {
        dprintf("RegisterClassExW failed\n");
        return 1;
    }

    HWND hwnd = CreateWindowEx(WS_EX_ACCEPTFILES,
                               class_name,
                               window_name,
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               WINDOW_START_H, WINDOW_START_W,
                               NULL, NULL, hInstance, NULL);

    if (!hwnd)
    {
        dprintf("CreateWindowEx failed\n");
        UnregisterClassW(class_name, hInstance);
        return 1;
    }

    if (!opengl_init(hwnd))
    {
        dprintf("opengl_init failed\n");
        DestroyWindow(hwnd);
        UnregisterClassW(class_name, hInstance);
        return 1;
    }

    if (!g_tj_handle)
    {
        g_tj_handle = tjInitDecompress();
        if (!g_tj_handle)
        {
            dprintf("tjInitDecompress failed on main thread\n");
            return 1;
        }
    }

    cache_init(hwnd);

    dprintf("setup finished\n");

    image_show_current();
    ShowWindow(hwnd, nCmdShow);
    // UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterClassW(class_name, hInstance);

    if (g_files.data) vfree(g_files.data);
    if (g_tj_handle) tjDestroy(g_tj_handle);

    return (int)msg.wParam;
}