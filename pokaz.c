#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

//
// STATE
//

static int g_win_w = WINDOW_START_W;
static int g_win_h = WINDOW_START_H;

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

    return (int)msg.wParam;
}