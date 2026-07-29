#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <shellapi.h>

#include <GL/gl.h>

#pragma comment(lib, "user32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "opengl32")
#pragma comment(lib, "gdi32")

//
// DEFINES
//

#define DEBUG_TO_CONSOLE

#define UNUSED(val)       ((void)(val))
#define ARRAY_LENGTH(arr) (sizeof((arr)) / sizeof((arr)[0]))

#define WINDOW_START_H (1024)
#define WINDOW_START_W (768)

#define FILES_MAX_COUNT (65535)

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

static int g_win_w = WINDOW_START_W;
static int g_win_h = WINDOW_START_H;

struct file_list g_files = {0};

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

//
// RENDER
//

static void image_render(void)
{
    glViewport(0, 0, g_win_w, g_win_h);
    glClear(GL_COLOR_BUFFER_BIT);
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
    dprintf("scan_directory : %ls\n", dir);

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

    dprintf("Loaded images : \n");
    for (uint16_t i = 0; i < g_files.count; i++)
    {
        dprintf("    %ls %s\n", g_files.paths[i], g_files.current == i ? "<--" : "");
    }
}

//
// WIN MAIN/PROC
//

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;

        case WM_DROPFILES:
        {
            HDROP   hDrop         = (HDROP)wParam;
            wchar_t buf[MAX_PATH] = {0};
            if (DragQueryFileW(hDrop, 0, buf, MAX_PATH))
            {
                scan_from_path(buf);
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

            image_render();
            SwapBuffers(g_gl.hdc);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
        {
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