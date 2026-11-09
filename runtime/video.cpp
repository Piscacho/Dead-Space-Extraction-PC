// Video output: a window showing the external framebuffer (XFB) that the game hands to
// VI with VISetNextFrameBuffer. The XFB is YUV 4:2:2 (Y0 U Y1 V per pixel pair); on every
// retrace it is converted to RGB and painted, scaled to the window.
//
// The window: its size comes from dse.ini (ancho_ventana, alto_ventana); F11 or Alt+Enter
// switch to borderless full screen (pantalla_completa=1 starts that way). The image keeps
// the image's aspect ratio (aspecto = 4:3, 16:9, 21:9 or 32:9) with black bars around it.
#include "runtime.h"
#include "config.h"
#include "gx.h"
#include "gx_state.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace
{
HWND g_window = nullptr;
std::mutex g_lock;
std::vector<uint32_t> g_pixels;  // BGRA, g_width x g_height
uint32_t g_width = 640, g_height = 480;
uint32_t g_xfb = 0;
std::atomic<bool> g_resized{ false };
bool g_fullscreen = false;
RECT g_windowedRect{};

// Borderless full screen on the window's monitor, or back to the saved window.
void SetFullscreen(HWND hwnd, bool on)
{
    if (on == g_fullscreen)
        return;
    g_fullscreen = on;
    if (on)
    {
        GetWindowRect(hwnd, &g_windowedRect);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, g_windowedRect.left, g_windowedRect.top, g_windowedRect.right - g_windowedRect.left,
            g_windowedRect.bottom - g_windowedRect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        g_resized = true;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29)))  // Alt+Enter
        {
            SetFullscreen(hwnd, !g_fullscreen);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11)
        {
            SetFullscreen(hwnd, !g_fullscreen);
            return 0;
        }
        break;
    case WM_PAINT:
    {
        if (GxBackendActive())
        {
            ValidateRect(hwnd, nullptr);  // the D3D12 swap chain draws the window
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        std::lock_guard lock(g_lock);
        if (!g_pixels.empty())
        {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = LONG(g_width);
            bi.bmiHeader.biHeight = -LONG(g_height);  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, g_width, g_height, g_pixels.data(), &bi,
                DIB_RGB_COLORS, SRCCOPY);
        }
        else
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        LOG("[video] window closed");
        ExitProcess(0);
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI WindowThread(LPVOID ready)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DSEWindow";
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    int width = ConfigInt("ancho_ventana", 1280), height = ConfigInt("alto_ventana", 960);
    RECT r{ 0, 0, std::max(320, width), std::max(240, height) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowW(wc.lpszClassName, L"Dead Space: Extraction", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (ConfigBool("pantalla_completa", false))
        SetFullscreen(g_window, true);
    SetEvent((HANDLE)ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

uint8_t Clamp(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
} // namespace

void VideoInit()
{
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, WindowThread, ready, 0, nullptr);
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
}

void VideoSetFramebuffer(uint32_t xfb)
{
    if (xfb != g_xfb)
    {
        static int logged = 0;
        if (logged++ < 4)
            LOG("[video] XFB 0x%08X", xfb);
    }
    g_xfb = xfb;
}

void VideoSetMode(uint32_t width, uint32_t height)
{
    if (width && height && width <= 1024 && height <= 1024)
    {
        std::lock_guard lock(g_lock);
        g_width = width;
        g_height = height;
        LOG("[video] mode %ux%u", width, height);
    }
}

void* VideoGetWindow() { return g_window; }

bool VideoTakeResize() { return g_resized.exchange(false); }

// aspecto = 4:3, 16:9, 21:9 or 32:9 (the older panoramico=1 means 16:9).
float VideoAspect()
{
    static const float aspect = [] {
        std::string a = ConfigString("aspecto", "");
        if (a == "16:9") return 16.0f / 9.0f;
        if (a == "21:9") return 21.0f / 9.0f;
        if (a == "32:9") return 32.0f / 9.0f;
        if (a == "4:3") return 4.0f / 3.0f;
        return ConfigBool("panoramico", false) ? 16.0f / 9.0f : 4.0f / 3.0f;
    }();
    return aspect;
}

// Wider than 16:9: the game composes for 16:9 and the backend widens its 3D views by
// this factor's inverse (clip x is multiplied by it), so 1 up to 16:9.
float VideoWideFactor()
{
    float a = VideoAspect();
    return a > 16.0f / 9.0f + 0.01f ? (16.0f / 9.0f) / a : 1.0f;
}

// Where the image goes in a client area of w x h: the game's aspect ratio, centered.
void VideoImageRect(int w, int h, int& x, int& y, int& iw, int& ih)
{
    float aspect = VideoAspect();
    iw = w;
    ih = int(w / aspect + 0.5f);
    if (ih > h)
    {
        ih = h;
        iw = int(h * aspect + 0.5f);
    }
    x = (w - iw) / 2;
    y = (h - ih) / 2;
}

void VideoPresent()
{
    // With the D3D12 backend, frames are presented on EFB -> XFB copies instead.
    if (!g_xfb || !g_window || GxBackendActive())
        return;
    uint32_t xfb = g_xfb | 0x80000000;
    {
        std::lock_guard lock(g_lock);
        g_pixels.resize(size_t(g_width) * g_height);
        const uint8_t* src = g_base + xfb;
        for (uint32_t y = 0; y < g_height; y++)
        {
            const uint8_t* row = src + size_t(y) * g_width * 2;
            uint32_t* out = &g_pixels[size_t(y) * g_width];
            for (uint32_t x = 0; x + 1 < g_width; x += 2)
            {
                int y0 = row[x * 2] - 16, u = row[x * 2 + 1] - 128, y1 = row[x * 2 + 2] - 16, v = row[x * 2 + 3] - 128;
                auto rgb = [&](int yy) {
                    int c = 298 * yy;
                    uint8_t r = Clamp((c + 409 * v + 128) >> 8);
                    uint8_t g = Clamp((c - 100 * u - 208 * v + 128) >> 8);
                    uint8_t b = Clamp((c + 516 * u + 128) >> 8);
                    return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
                };
                out[x] = rgb(y0);
                out[x + 1] = rgb(y1);
            }
        }
    }
    InvalidateRect(g_window, nullptr, FALSE);

    // DSE_SHOT=1: once a second, save the frame as logs/frame.bmp (for testing without
    // looking at the window).
    static bool shots = getenv("DSE_SHOT") != nullptr;
    static uint32_t presents = 0;
    if (shots && ++presents % 60 == 0)
    {
        std::lock_guard lock(g_lock);
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = LONG(g_width);
        ih.biHeight = -LONG(g_height);
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        uint32_t bytes = g_width * g_height * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + bytes;
        if (FILE* f = fopen("G:/Dead Space Extraction PC/Recomp/logs/frame.bmp", "wb"))
        {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            fwrite(g_pixels.data(), 1, bytes, f);
            fclose(f);
        }
    }
}
