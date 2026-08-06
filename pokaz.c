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

#include "turbojpeg.h"
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

// handles up to ~64 Megapixel images
#define IMAGE_MAX_W         (4096)
#define IMAGE_MAX_H         (4096)
#define IMAGE_PREALLOC_SIZE (IMAGE_MAX_W * IMAGE_MAX_H * 4)

#define WM_DECODED_IMAGE_READY (WM_APP + 1)

static const wchar_t *SUPPORTED_EXTENSIONS[] = {
    L".jpg", L".jpeg", L".png", L".bmp", L".gif",
    L".tiff", L".tif", L".ico", L".webp", L".wdp",
    L".hdp", L".jxr"};

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

#define KB(x) ((size_t)((double)(x) * 1024.0))
#define MB(x) ((size_t)((double)(x) * 1024.0 * 1024.0))
#define GB(x) ((size_t)((double)(x) * 1024.0 * 1024.0 * 1024.0))

static size_t system_get_page_size(void)
{
    SYSTEM_INFO sys_info = {0};
    GetSystemInfo(&sys_info);
    return sys_info.dwPageSize;
}

static inline size_t align_forward(size_t ptr, size_t align)
{
    assert((align & (align - 1)) == 0 && "alignment must be a power of 2");
    return (ptr + (align - 1)) & ~(align - 1);
}

static inline size_t align_to_page(size_t size)
{
    size_t page_size = system_get_page_size();
    return align_forward(size, page_size);
}

//
// ARENA
//

struct arena
{
    uint8_t *base;
    size_t   capacity;
    size_t   committed;
    size_t   used;
};

static bool arena_init(struct arena *arena, size_t reserve_bytes)
{
    if (!arena) return false;

    arena->base = VirtualAlloc(
        NULL,
        reserve_bytes,
        MEM_RESERVE,
        PAGE_NOACCESS // access prohibited until explicitly committed
    );
    if (!arena->base) return false;

    arena->capacity  = reserve_bytes;
    arena->committed = 0;
    arena->used      = 0;

    return true;
}

static void *arena_alloc(struct arena *arena, size_t size)
{
    size_t align          = sizeof(void *);
    size_t aligned_offset = align_forward(arena->used, align);

    size_t total_size = aligned_offset + size;

    if (total_size > arena->capacity) return NULL;

    if (total_size > arena->committed)
    {
        size_t bytes_needed = total_size - arena->committed;
        size_t commit_size  = align_to_page(bytes_needed);

        void *commit_result = VirtualAlloc(
            (uint8_t *)arena->base + arena->committed,
            commit_size,
            MEM_COMMIT,
            PAGE_READWRITE);

        if (!commit_result) return NULL;
        arena->committed += commit_size;
    }

    void *ptr   = (uint8_t *)arena->base + aligned_offset;
    arena->used = total_size;

    return ptr;
}

static void arena_free(struct arena *arena)
{
    if (arena && arena->base)
    {
        VirtualFree(arena->base, 0, MEM_RELEASE);
        arena->base      = NULL;
        arena->capacity  = 0;
        arena->committed = 0;
        arena->used      = 0;
    }
}

struct mem_block
{
    uint8_t *base;
    size_t   max_capacity;
    size_t   committed;
};

static struct mem_block block_init(size_t max_capacity)
{
    struct mem_block block = {0};

    size_t reserve_size = align_to_page(max_capacity);

    block.base = VirtualAlloc(
        NULL,
        reserve_size,
        MEM_RESERVE,
        PAGE_NOACCESS // access prohibited until explicitly committed
    );
    block.max_capacity = (block.base != NULL) ? max_capacity : 0;
    block.committed    = 0;

    return block;
}

static void *block_commit(struct mem_block *block, size_t needed_size)
{
    if (!block || !block->base) return NULL;

    if (needed_size <= block->committed) return block->base;
    if (needed_size > block->max_capacity)
    {
        dprintf("block_commit : requested (%zu B) exceeds max (%zu B)\n", needed_size, block->max_capacity);
        return NULL;
    }

    size_t target_commit    = align_to_page(needed_size);
    size_t additional_bytes = target_commit - block->committed;

    void *result = VirtualAlloc(
        (uint8_t *)block->base + block->committed,
        additional_bytes,
        MEM_COMMIT,
        PAGE_READWRITE);

    if (!result) return NULL;

    block->committed = target_commit;
    return block->base;
}

void block_free(struct mem_block *block)
{
    if (block && block->base)
    {
        VirtualFree(block->base, 0, MEM_RELEASE);
        block->base         = NULL;
        block->max_capacity = 0;
        block->committed    = 0;
    }
}

//
// STATE
//

struct file_list
{
    struct arena arena;

    wchar_t  base_path[MAX_PATH];         // C:/folder/images/
    wchar_t *file_names[FILES_MAX_COUNT]; // image1.jpg, image2.jpg...

    uint16_t paths_count;
    uint16_t current;
};

struct main_image
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

static struct file_list  g_files      = {0};
static struct main_image g_main_image = {0};

static void     image_full_path(uint16_t index, wchar_t *out, size_t out_cap);
static uint16_t image_map_index(int offset);

void file_list_remove_at(uint16_t idx)
{
    if (idx >= g_files.paths_count || g_files.paths_count == 0)
    {
        return;
    }

    uint16_t elements_to_move = g_files.paths_count - idx - 1;
    if (elements_to_move > 0)
    {
        memmove(&g_files.file_names[idx],
                &g_files.file_names[idx + 1],
                elements_to_move * sizeof(wchar_t *));
    }
    g_files.file_names[g_files.paths_count] = NULL;

    g_files.paths_count--;

    if (g_files.paths_count == 0)
    {
        g_files.current = 0; // list is now empty
    }
    else if (g_files.current >= g_files.paths_count)
    {
        // if we deleted the very last file in the list, wrap back to the new end
        g_files.current = g_files.paths_count - 1;
    }
    // leaving g_files.current unchanged automatically selects the next image
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

enum pixel_format
{
    PIXEL_FORMAT_RGB8,  // 3 bytes/px: R,G,B
    PIXEL_FORMAT_RGBA8, // 4 bytes/px: R,G,B,A
    PIXEL_FORMAT_BGRA8, // 4 bytes/px: B,G,R,A
};

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

    // glClearColor(0.0f, 0.3686f, 0.7216f, 1.0f); // pantone 300 C
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    return true;
}

static void opengl_cleanup(HWND hwnd)
{
    if (g_main_image.tex != 0)
    {
        glDeleteTextures(1, &g_main_image.tex);
        g_main_image.tex = 0;
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

static GLuint opengl_texture_upload(BYTE *buf, UINT w, UINT h, enum pixel_format format)
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

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, gl_format, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);

    return tex;
}

//
// MAPPING FILES
//

struct file_map
{
    uint8_t *data;
    size_t   size;
    HANDLE   file_handle;
    HANDLE   mapping_handle;
};

static bool file_map_read_only(const wchar_t *path, struct file_map *out_map)
{
    out_map->file_handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out_map->file_handle == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER file_size = {0};
    GetFileSizeEx(out_map->file_handle, &file_size);
    out_map->size = (size_t)file_size.QuadPart;

    out_map->mapping_handle = CreateFileMappingW(out_map->file_handle, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!out_map->mapping_handle)
    {
        CloseHandle(out_map->file_handle);
        return false;
    }

    out_map->data = MapViewOfFile(out_map->mapping_handle, FILE_MAP_READ, 0, 0, 0);
    return out_map->data != NULL;
}

static void file_map_unmap(struct file_map *map)
{
    if (map->data) UnmapViewOfFile(map->data);
    if (map->mapping_handle) CloseHandle(map->mapping_handle);
    if (map->file_handle != INVALID_HANDLE_VALUE) CloseHandle(map->file_handle);
}

//
// WIC
//

static IWICImagingFactory *g_wic = NULL;

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

static bool wic_decode(struct file_map *map, struct mem_block *out_pixels, UINT *out_w, UINT *out_h, enum pixel_format *out_format)
{
    if (!map || !out_pixels || !out_w || !out_h || !out_format) return false;

    IWICStream            *stream  = NULL;
    IWICBitmapDecoder     *decoder = NULL;
    IWICBitmapFrameDecode *frame   = NULL;
    IWICFormatConverter   *conv    = NULL;

    bool result = false;

    double time_start = time_in_ms();

    HRESULT hr;
    hr = IWICImagingFactory_CreateStream(g_wic, &stream);
    if (FAILED(hr)) goto done;

    hr = IWICStream_InitializeFromMemory(stream, (BYTE *)map->data, (DWORD)map->size);
    if (FAILED(hr)) goto done;

    hr = IWICImagingFactory_CreateDecoderFromStream(g_wic, (IStream *)stream, NULL,
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

    void *pixels = block_commit(out_pixels, (size_t)((*out_w) * (*out_h) * 4));
    if (!pixels) goto done;

    hr = IWICFormatConverter_CopyPixels(conv, NULL,
                                        (*out_w) * 4,
                                        (*out_w) * (*out_h) * 4,
                                        pixels);
    if (FAILED(hr)) goto done;

    *out_format = PIXEL_FORMAT_BGRA8;
    result      = true;

done:
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IWICStream_Release(stream);

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

static tjhandle g_tj_handle = NULL; // handle for main thread

static bool turbojpeg_decode(struct file_map *map, tjhandle handle, struct mem_block *out_pixels, UINT *out_w, UINT *out_h, enum pixel_format *out_format)
{
    if (!map || !handle || !out_pixels || !out_w || !out_h || !out_format) return false;

    bool   result     = false;
    double time_start = time_in_ms();

    int w, h, subsamp, colour_space;
    if (tjDecompressHeader3(handle, map->data, (unsigned long)map->size,
                            &w, &h, &subsamp, &colour_space) < 0) goto done;

    void *pixels = block_commit(out_pixels, (size_t)w * h * tjPixelSize[TJPF_BGRA]);
    if (!pixels) goto done;

    if (tjDecompress2(handle, map->data, (unsigned long)map->size,
                      pixels, w, 0, h, TJPF_BGRA, TJFLAG_FASTDCT) < 0) goto done;

    *out_w      = (UINT)w;
    *out_h      = (UINT)h;
    *out_format = PIXEL_FORMAT_BGRA8; // TJPF_BGRA

    result = true;

done:
    if (!result) dprintf("decode_jpeg_turbo: %s\n", tjGetErrorStr2(handle));

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

static bool decode_png_spng(struct file_map *map, struct mem_block *out_pixels, UINT *out_w, UINT *out_h, enum pixel_format *out_format)
{
    if (!map || !out_pixels || !out_w || !out_h || !out_format) return false;

    bool   result     = false;
    double time_start = time_in_ms();

    spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) goto done;

    if (spng_set_png_buffer(ctx, map->data, (size_t)map->size) != 0) goto done;

    struct spng_ihdr ihdr = {0};
    if (spng_get_ihdr(ctx, &ihdr) != 0) goto done;

    bool has_alpha = (ihdr.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ||
                      ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA);

    int fmt = has_alpha ? SPNG_FMT_RGBA8 : SPNG_FMT_RGB8;

    size_t out_size = 0;
    if (spng_decoded_image_size(ctx, fmt, &out_size) != 0) goto done;

    void *pixels = block_commit(out_pixels, out_size);
    if (!pixels) goto done;

    if (spng_decode_image(ctx, pixels, out_size, fmt, 0) != 0) goto done;

    *out_w = (UINT)ihdr.width;
    *out_h = (UINT)ihdr.height;

    *out_format = has_alpha ? PIXEL_FORMAT_RGBA8 : PIXEL_FORMAT_RGB8;

    result = true;

done:
    if (ctx) spng_ctx_free(ctx);

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

static bool decode_image(struct mem_block *out_pixels, tjhandle tj_handle, const wchar_t *path, UINT *w, UINT *h, enum pixel_format *out_format)
{
    bool result = false;

    // TODO : we could save the extension info when loading the paths?
    //  enum { IMAGE_JPG ... }
    const wchar_t *ext = wcsrchr(path, L'.');
    if (ext)
    {
        struct file_map file = {0};
        if (!file_map_read_only(path, &file)) return false;

        if (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0)
        {
            result = turbojpeg_decode(&file, tj_handle, out_pixels, w, h, out_format);
        }
        else if (_wcsicmp(ext, L".png") == 0)
        {
            result = decode_png_spng(&file, out_pixels, w, h, out_format);
        }

        // try again / defualt with wic
        if (!result) result = wic_decode(&file, out_pixels, w, h, out_format);

        file_map_unmap(&file);
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

    enum pixel_format format;
    struct mem_block  block;
    UINT              w, h;
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

        uint16_t target = g_cache.entries[i].index;

        wchar_t full_path[MAX_PATH];
        image_full_path(target, full_path, MAX_PATH);

        if (0) dprintf("thread %u decoding %u, '%ls'\n", thread_id, target, full_path);

        bool res = decode_image(&g_cache.entries[i].block,
                                tj_handle,
                                full_path,
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

        if (!g_cache.entries[i].block.base)
        {
            g_cache.entries[i].block = block_init(IMAGE_PREALLOC_SIZE);
        }
        // NOTE : we would release the block back to the OS again here
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
        block_free(&g_cache.entries[i].block);
    }

    DeleteCriticalSection(&g_cache.lock);
}

//
// SHUFFLE
//

struct shuffle
{
    uint16_t        *map;
    struct mem_block memory;

    bool is_shuffle;
};

static struct shuffle g_shuffle = {.is_shuffle = false};

static void shuffle_map_generate(uint16_t pin)
{
    if (!g_shuffle.map)
    {
        g_shuffle.memory = block_init(sizeof(uint16_t) * FILES_MAX_COUNT);
    }

    if (g_files.paths_count == 0) return;

    g_shuffle.map = block_commit(&g_shuffle.memory, sizeof(uint16_t) * g_files.paths_count);
    if (!g_shuffle.map) return;

    for (uint16_t i = 0; i < g_files.paths_count; i++)
    {
        g_shuffle.map[i] = i;
    }

    // if we didnt swap these, when we choose random we would jump to the first random
    // image but it wont be displayed, as our cache lags behind.
    // uint16_t current_file_index = image_map_index(0);
    if (pin < g_files.paths_count)
    {
        uint16_t temp      = g_shuffle.map[0];
        g_shuffle.map[0]   = g_shuffle.map[pin];
        g_shuffle.map[pin] = temp;
    }

    static bool is_seeded = false;
    if (!is_seeded)
    {
        srand((unsigned int)time(NULL));
        is_seeded = true;
    }

    // Fisher-Yates Shuffle
    for (uint16_t i = g_files.paths_count - 1; i > 1; i--)
    {
        // ensuring we can safely shuffle up to 65535 (uint16_t max) items
        uint32_t large_rand = ((rand() << 15) | rand());

        // we dont want to move our first element
        uint16_t j = 1 + (uint16_t)(large_rand % i);

        uint16_t temp    = g_shuffle.map[i];
        g_shuffle.map[i] = g_shuffle.map[j];
        g_shuffle.map[j] = temp;
    }

    g_files.current = 0;

#if DEBUG
    dprintf("shuffle_map_generate\n");
    dprintf("[");
    for (uint16_t i = 0; i < g_files.paths_count; i++)
    {
        dprintf("%s%u", (i == 0) ? "" : ", ", g_shuffle.map[i]);
    }
    dprintf("]\n");
#endif
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

    if (g_files.paths_count <= 1) return;

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
    if (g_files.paths_count <= 1) return;

    // same as a "right" move
    g_files.current = (g_files.current + 1) % g_files.paths_count;
}

//
// RENDER
//

static void image_full_path(uint16_t index, wchar_t *out, size_t out_cap)
{
    swprintf_s(out, out_cap, L"%s\\%s", g_files.base_path, g_files.file_names[index]);
}

static inline void image_reset_view(void)
{
    g_main_image.zoom     = 0.0f;
    g_main_image.pan_x    = 0.0f;
    g_main_image.pan_y    = 0.0f;
    g_main_image.rotation = 0;
}

static uint16_t image_map_index(int offset)
{
    if (g_files.paths_count == 0) return 0;

    int count   = (int)g_files.paths_count;
    int current = (int)g_files.current;

    int pos = (current + offset) % count;
    if (pos < 0) pos += count;

    if (g_shuffle.is_shuffle && g_shuffle.map != NULL)
    {
        return g_shuffle.map[pos];
    }

    return (uint16_t)pos;
}

static void image_show_current(void)
{
    if (g_files.paths_count == 0) return;

    image_reset_view();
    uint16_t current_idx = image_map_index(0);

    BYTE             *pixels_to_upload = NULL;
    enum pixel_format format           = 0;
    UINT              w = 0, h = 0;
    bool              cache_hit = false;

    EnterCriticalSection(&g_cache.lock);
    {
        for (uint16_t j = 0; j < CACHE_CAPACITY; j++)
        {
            if (g_cache.entries[j].index == current_idx &&
                g_cache.entries[j].state == CACHE_READY)
            {
                pixels_to_upload = g_cache.entries[j].block.base;
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
        int active_targets = (g_files.paths_count < 5) ? g_files.paths_count : 5;
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

            // find a slot whose index is NOT in to_cache[] - safe to evict
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
        if (g_main_image.tex != 0) glDeleteTextures(1, &g_main_image.tex);
        g_main_image.tex = opengl_texture_upload(pixels_to_upload, w, h, format);
        g_main_image.w   = w;
        g_main_image.h   = h;
    }
    else
    {
        ReleaseSemaphore(g_cache.semaphore, 1, NULL);
    }
}

static float image_compute_scale(void)
{
    int is_sideways = (g_main_image.rotation % 180 != 0);
    int fit_w       = is_sideways ? g_main_image.h : g_main_image.w;
    int fit_h       = is_sideways ? g_main_image.w : g_main_image.h;

    float sx  = (float)g_win_w / (float)fit_w;
    float sy  = (float)g_win_h / (float)fit_h;
    float fit = (sx < sy) ? sx : sy;

    return fit * powf(1.15f, g_main_image.zoom);
}

static void image_render(void)
{
    glViewport(0, 0, g_win_w, g_win_h);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint tex_to_draw = g_main_image.tex;
    float  img_w       = (float)g_main_image.w;
    float  img_h       = (float)g_main_image.h;

    float scale    = image_compute_scale();
    float pan_x    = g_main_image.pan_x;
    float pan_y    = g_main_image.pan_y;
    float rotation = (float)g_main_image.rotation;

    float dw = img_w * scale;
    float dh = img_h * scale;

    float cx = g_win_w * 0.5f + pan_x;
    float cy = g_win_h * 0.5f + pan_y;

    float x0 = cx - dw * 0.5f, y0 = cy - dh * 0.5f;
    float x1 = cx + dw * 0.5f, y1 = cy + dh * 0.5f;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g_win_w, g_win_h, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (rotation != 0.0f)
    {
        glTranslatef(cx, cy, 0.0f);
        glRotatef(rotation, 0.0f, 0.0f, 1.0f);
        glTranslatef(-cx, -cy, 0.0f);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_to_draw);
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
    glDisable(GL_BLEND); // Turn it back off when done
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

static void scan_for_images(const wchar_t *dir)
{
    dprintf("scan_for_images : '%ls'\n", dir);

    if (!g_files.arena.base)
    {
        if (!arena_init(&g_files.arena, MB(64))) return;
    }

    g_files.paths_count = 0;
    g_files.current     = 0;

    wchar_t pattern[MAX_PATH] = {0};
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);

    WIN32_FIND_DATAW fd = {0};
    HANDLE           hf = FindFirstFileExW(pattern,
                                           FindExInfoBasic,
                                           &fd,
                                           FindExSearchNameMatch,
                                           NULL,
                                           FIND_FIRST_EX_LARGE_FETCH);
    if (hf == INVALID_HANDLE_VALUE) return;

    do
    {
        if (g_files.paths_count >= FILES_MAX_COUNT)
            break;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (!is_ext_supported(fd.cFileName))
            continue;

        size_t file_name_len = wcslen(fd.cFileName) + 1; // +1 for L'\0'

        wchar_t *file_name_location = arena_alloc(&g_files.arena, file_name_len * sizeof(wchar_t));
        if (file_name_location)
        {
            wmemcpy(file_name_location, fd.cFileName, file_name_len);

            g_files.file_names[g_files.paths_count] = file_name_location;
            g_files.paths_count++;
        }

    } while (FindNextFileW(hf, &fd));

    FindClose(hf);
}

static void scan_from_path(const wchar_t *path)
{
    dprintf("scan_from_path : '%ls'\n", path);

    double start_time = time_in_ms();

    wchar_t *file_name     = NULL;
    DWORD    full_path_len = GetFullPathNameW(path, MAX_PATH, g_files.base_path, &file_name);
    if (full_path_len == 0 || full_path_len > MAX_PATH)
    {
        dprintf("   GetFullPathNameW failed\n");
        return;
    };

    DWORD attr = GetFileAttributesW(g_files.base_path);
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        dprintf("   GetFileAttributesW == INVALID_FILE_ATTRIBUTES\n");
        return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        file_name = NULL;
    }

    // sets the last '\' as 0 if we have a file name on the end
    if (file_name != NULL) *(file_name - 1) = L'\0';

    scan_for_images(g_files.base_path);

    if (file_name && is_ext_supported(file_name))
    {
        for (uint16_t i = 0; i < g_files.paths_count; i++)
        {
            if (_wcsicmp(g_files.file_names[i], file_name) == 0)
            {
                g_files.current = i;
                break;
            }
        }
    }

    double elapsed_time = time_in_ms() - start_time;
    dprintf("scan_from_path time : %fms\n", elapsed_time);
    if (0)
    {
        dprintf("Loaded images from: '%ls'\n", path);
        for (uint16_t i = 0; i < g_files.paths_count; i++)
        {
            dprintf("    '%ls' %s\n", g_files.file_names[i], g_files.current == i ? "<--" : "");
        }
    }
}

//
// WIN MAIN/PROC
//

static void window_update_title(HWND hWnd)
{
    if (g_files.paths_count == 0)
    {
        SetWindowTextW(hWnd, L"Pokaz - no images found");
        return;
    }

    const wchar_t *name = g_files.file_names[g_files.current];

    uint16_t current_idx = image_map_index(0) + 1;

    wchar_t title[256] = {0};
    if (g_slide.active)
    {
        double secs = SLIDESHOW_SPEEDS[g_slide.speed_index] / 1000.0;

        swprintf_s(title, 256, L"[\u25b6 %.1fs] %s [%d / %d] %s (%d \u00d7 %d)",
                   secs,
                   g_shuffle.is_shuffle ? L"[\u21c4] " : L"",
                   current_idx, g_files.paths_count,
                   name,
                   g_main_image.w, g_main_image.h);
    }
    else
    {
        swprintf_s(title, 256, L"%s [%d / %d] %s (%d \u00d7 %d)",
                   g_shuffle.is_shuffle ? L"[\u21c4] " : L"",
                   current_idx, g_files.paths_count,
                   name,
                   g_main_image.w, g_main_image.h);
    }

#if DEBUG
    wchar_t cache_mem_state[256] = {0};
    EnterCriticalSection(&g_cache.lock);
    swprintf_s(cache_mem_state, 256, L"[(%u) %0.1fMB, (%u) %0.1fMB, (%u) %0.1fMB, (%u) %0.1fMB, (%u) %0.1fMB]",
               g_cache.entries[0].index, g_cache.entries[0].block.committed / 1024.0 / 1024.0,
               g_cache.entries[1].index, g_cache.entries[1].block.committed / 1024.0 / 1024.0,
               g_cache.entries[2].index, g_cache.entries[2].block.committed / 1024.0 / 1024.0,
               g_cache.entries[3].index, g_cache.entries[3].block.committed / 1024.0 / 1024.0,
               g_cache.entries[4].index, g_cache.entries[4].block.committed / 1024.0 / 1024.0);
    LeaveCriticalSection(&g_cache.lock);

    wchar_t full[512] = {0};
    swprintf_s(full, 512, L"%s %s", cache_mem_state, title);
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
                if (g_main_image.tex != 0) glDeleteTextures(1, &g_main_image.tex);
                g_main_image.tex = opengl_texture_upload(entry->block.base, entry->w, entry->h, entry->format);
                g_main_image.w   = entry->w;
                g_main_image.h   = entry->h;

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
                case VK_DELETE:
                {
                    uint16_t active_file = image_map_index(0);

                    bool is_actively_decoding = false;

                    EnterCriticalSection(&g_cache.lock);
                    for (uint16_t j = 0; j < CACHE_CAPACITY; j++)
                    {
                        if (g_cache.entries[j].index == active_file &&
                            g_cache.entries[j].state == CACHE_DECODING)
                        {
                            is_actively_decoding = true;
                            break;
                        }
                    }
                    LeaveCriticalSection(&g_cache.lock);

                    if (is_actively_decoding)
                    {
                        dprintf("cannot delete: thread is currently reading this file\n");
                        break;
                    }

                    wchar_t file_path[MAX_PATH + 2] = {0};
                    image_full_path(active_file, file_path, MAX_PATH);
                    dprintf("sending to recycling : '%ls'\n", file_path);

                    // SHFILEOPSTRUCT requires a double-null-terminated string buffer
                    SHFILEOPSTRUCT file_op = {
                        .hwnd                  = NULL,
                        .wFunc                 = FO_DELETE,
                        .pFrom                 = file_path,
                        .pTo                   = NULL,
                        .fFlags                = FOF_ALLOWUNDO,
                        .fAnyOperationsAborted = FALSE,
                    };
                    if (SHFileOperation(&file_op) == 0 && !file_op.fAnyOperationsAborted)
                    {
                        // need to evict the image from our cache
                        dprintf("evicting image (%u) from cache and file list\n", active_file);
                        file_list_remove_at(active_file);

                        EnterCriticalSection(&g_cache.lock);
                        for (uint16_t j = 0; j < CACHE_CAPACITY; j++)
                        {
                            if (g_cache.entries[j].state == CACHE_EMPTY) continue;

                            if (g_cache.entries[j].index == active_file)
                            {
                                g_cache.entries[j].index = (uint16_t)(-1);
                                g_cache.entries[j].state = CACHE_EMPTY;
                            }
                            else if (g_cache.entries[j].index > active_file)
                            {
                                g_cache.entries[j].index--;
                            }
                        }
                        LeaveCriticalSection(&g_cache.lock);
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    else
                    {
                        dprintf("delete aborted or failed\n");
                    }
                    break;
                }
                case VK_RIGHT:
                case VK_NEXT:
                case 'D':
                {
                    if (g_files.paths_count > 0)
                    {
                        g_files.current = (g_files.current + 1) % g_files.paths_count;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case VK_LEFT:
                case VK_PRIOR:
                case 'A':
                {
                    if (g_files.paths_count > 0)
                    {
                        g_files.current = (g_files.current + g_files.paths_count - 1) % g_files.paths_count;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case VK_HOME:
                {
                    if (g_files.paths_count > 0)
                    {
                        g_files.current = 0;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case VK_END:
                {
                    if (g_files.paths_count > 0)
                    {
                        g_files.current = g_files.paths_count - 1;
                        image_show_current();
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;
                }
                case 'R':
                {
                    g_main_image.rotation = (g_main_image.rotation + 90) % 360;
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case 'Z':
                {
                    // TODO : zoom limits
                    g_main_image.zoom += 1.0f;
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case 'X':
                {
                    // TODO : zoom limits
                    g_main_image.zoom -= 1.0f;
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case '0':
                {
                    image_reset_view();
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
                case 'O':
                {
                    uint16_t active_file = image_map_index(0);

                    wchar_t file_path[MAX_PATH];
                    image_full_path(active_file, file_path, MAX_PATH);

                    char cmd[1024] = {0};
                    snprintf(cmd, sizeof(cmd), "explorer.exe /select,\"%ls\"", file_path);
                    system(cmd);
                    break;
                }

                case 'T':
                {
                    if (g_files.paths_count == 0) break;

                    if (!g_shuffle.is_shuffle)
                    {
                        uint16_t active_file = image_map_index(0);
                        g_shuffle.is_shuffle = true; // must be toggled AFTER image_map_index

                        shuffle_map_generate(active_file);
                    }
                    else
                    {
                        uint16_t active_file = image_map_index(0);
                        g_files.current      = active_file;
                        g_shuffle.is_shuffle = false; // must be toggled AFTER image_map_index
                    }

                    image_show_current();
                    window_update_title(hwnd);
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
            g_main_image.dragging     = true;
            g_main_image.drag_start_x = mx;
            g_main_image.drag_start_y = my;
            g_main_image.pan_start_x  = g_main_image.pan_x;
            g_main_image.pan_start_y  = g_main_image.pan_y;

            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            // int mx = GET_X_LPARAM(lParam);
            // int my = GET_Y_LPARAM(lParam);

            g_main_image.dragging = false;
            ReleaseCapture();
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            if (g_main_image.dragging)
            {
                g_main_image.pan_x = g_main_image.pan_start_x + (float)(mx - g_main_image.drag_start_x);
                g_main_image.pan_y = g_main_image.pan_start_y + (float)(my - g_main_image.drag_start_y);

                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);

            POINT lpPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            BOOL  res     = ScreenToClient(hwnd, &lpPoint);

            if (res && g_main_image.w != 0 && g_main_image.h != 0)
            {
                float old_scale = image_compute_scale();
                float cx        = g_win_w * 0.5f + g_main_image.pan_x;
                float cy        = g_win_h * 0.5f + g_main_image.pan_y;

                g_main_image.zoom += (delta > 0) ? 1.0f : -1.0f;

                float new_scale = image_compute_scale();
                float ratio     = new_scale / old_scale;

                g_main_image.pan_x += (lpPoint.x - cx) * (1.0f - ratio);
                g_main_image.pan_y += (lpPoint.y - cy) * (1.0f - ratio);

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

            dprintf("Shutting down : shuffle\n");
            block_free(&g_shuffle.memory);
            g_shuffle.map = NULL;

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
        return 1;
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

    if (g_tj_handle) tjDestroy(g_tj_handle);

    arena_free(&g_files.arena);

    return (int)msg.wParam;
}