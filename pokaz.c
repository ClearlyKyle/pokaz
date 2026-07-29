#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <wchar.h>
#include <shellapi.h>

#include <GL/gl.h>

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
};

static int g_win_w = WINDOW_START_W;
static int g_win_h = WINDOW_START_H;

static struct file_list    g_files  = {0};
static struct render_state g_render = {0};

static IWICImagingFactory *g_wic = NULL;

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
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
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

static GLuint opengl_texture_upload(BYTE *buf, UINT w, UINT h)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

static bool wic_decode(const wchar_t *path, BYTE *out_buf, UINT *out_w, UINT *out_h)
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

    result = true;

done:
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);

    double time_elapsed = time_in_ms() - time_start;
    dprintf("decode_wic: %ux%u %.3fms, %s\n", *out_w, *out_h, time_elapsed, result ? "OK" : "ERROR");

    return result;
}

//
// TURBO JPEG
//

static bool turbojpeg_decode(const wchar_t *path, BYTE *buf, UINT *out_w, UINT *out_h)
{
    if (!path || !buf || !out_w || !out_h) return false;

    // TODO : when multithreaded, each thread will need its own handle
    static tjhandle handle = NULL;
    if (!handle) handle = tjInitDecompress();

    BYTE  *file_buf = NULL;
    bool   result   = false;
    HANDLE f        = INVALID_HANDLE_VALUE;

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

    int w, h, subsamp, colour_space;
    if (tjDecompressHeader3(handle, file_buf, (unsigned long)bytes_read,
                            &w, &h, &subsamp, &colour_space) < 0) goto done;

    if (tjDecompress2(handle, file_buf, (unsigned long)bytes_read,
                      buf, w, 0, h, TJPF_BGRA, TJFLAG_FASTDCT) < 0) goto done;

    *out_w = (UINT)w;
    *out_h = (UINT)h;

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

    double time_elapsed = time_in_ms() - time_start;
    dprintf("turbojpeg_decode: %ux%u %.3fms, %s\n", *out_w, *out_h, time_elapsed, result ? "OK" : "ERROR");

    return result;
}

//
// DECODE
//

static bool decode_image(const wchar_t *path, BYTE *buf, UINT *w, UINT *h)
{
    bool result = false;

    // TODO : we could save the extension info when loading the paths?
    //  enum { IMAGE_JPG ... }
    const wchar_t *ext = wcsrchr(path, L'.');
    if (ext)
    {
        if (_wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0)
        {
            result = turbojpeg_decode(path, buf, w, h);
        }

        // try again / defualt with wic
        if (!result) result = wic_decode(path, buf, w, h);
    }
    else
    {
        dprintf("decode_image : ext fail\n");
    }

    return result;
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

static void image_show_current(void)
{
    if (g_files.count > 0)
    {
        image_reset_view();

        UINT     w = 0, h = 0;
        BYTE    *image = vmalloc(IMAGE_PREALLOC_SIZE);
        wchar_t *path  = g_files.paths[g_files.current];

        dprintf("uploading image %ls\n", path);
        bool res = decode_image(path, image, &w, &h);
        if (!res) dprintf("image decode failed\n");

        if (g_render.tex != 0) glDeleteTextures(1, &g_render.tex);

        g_render.tex = opengl_texture_upload(image, w, h);
        g_render.w   = w;
        g_render.h   = h;

        vfree(image);
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

    // pattern[pattern_end - 1] = 0; // now we have the base path
    // pattern would look like : "images\4\*",
    //  pattern_len = 10                  ^
    //  base_len    = 9                  ^    (up to the last \)
    // size_t pattern_len = wcslen(pattern);
    // size_t base_len    = pattern_len - 1;
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

        // TODO : pack the names tigher
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
    dprintf("scan_from_path : %ls\n", path);

    wchar_t dir[MAX_PATH] = {0};
    wcscpy_s(dir, MAX_PATH, path); // NOTE : do we even need to copy?

    // strip trailing quotes and backslashes
    size_t len = wcslen(dir);
    while (len > 0 &&
           ((dir[len - 1] == L'"') || (dir[len - 1] == L'\\')))
    {
        dir[--len] = L'\0';
    }

    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        scan_directory(dir, len);
    }
    else
    {
        // truncate to parent directory
        wchar_t *sep = wcsrchr(dir, L'\\');
        if (sep)
        {
            *sep = L'\0';
            len  = wcslen(dir);
            // len = (size_t)(sep) - (size_t)(dir);
            scan_directory(dir, len);
        }
    }

    // NOTE : if we are given an image path, we set it as current
    //  but if given a directory we set current as the first image
    //  we should skip this if given a directory
    for (uint16_t i = 0; i < g_files.count; i++)
    {
        if (_wcsicmp(g_files.paths[i], path) == 0)
        {
            g_files.current = i;
            break;
        }
    }

    if (0)
    {
        dprintf("Loaded images : \n");
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

    wchar_t title[256] = {0};
    swprintf_s(title, 2048, L"[%d / %d]  %s  (%d \u00d7 %d)",
               g_files.current + 1, g_files.count, name, g_render.w, g_render.h);

    SetWindowTextW(hWnd, title);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_KEYDOWN:
            switch (wParam)
            {
                case VK_ESCAPE:
                {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
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
            }
            return 0;

        case WM_DROPFILES:
        {
            HDROP   hDrop         = (HDROP)wParam;
            wchar_t buf[MAX_PATH] = {0};
            if (DragQueryFileW(hDrop, 0, buf, MAX_PATH))
            {
                scan_from_path(buf);
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
    else
    {
        dprintf("no paths found on the command line\n");
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
        // TODO : send errors to both the console and or the debuger?
        dprintf("[ERROR] Failed to register window class.\n");
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
        dprintf("[ERROR] Failed to create window.\n");
        UnregisterClassW(class_name, hInstance);
        return 1;
    }

    if (!opengl_init(hwnd))
    {
        dprintf("[ERROR] Failed to initialize OpenGL context.\n");
        DestroyWindow(hwnd);
        UnregisterClassW(class_name, hInstance);
        return 1;
    }

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

    return (int)msg.wParam;
}