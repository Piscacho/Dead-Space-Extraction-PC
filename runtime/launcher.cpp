// Launcher: a settings window that opens before the game, inside DSE.exe (like the Guitar
// Hero port's). It reads and writes dse.ini (config.h), keeping comments and unknown keys.
// "mostrar_launcher = 0" goes straight to the game (holding Shift shows it again);
// --jugar skips it and --launcher forces it.
//
// Look: Dead Space's holographic interface. Near-black metal with scanlines, cyan panels
// with cut corners, uppercase spaced headings. Everything is drawn here with GDI (no
// common controls), into a back buffer.
#include "runtime.h"
#include "bindings.h"
#include "config.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <xinput.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")

namespace
{
// --- Look ------------------------------------------------------------------------------
const COLORREF kBg = RGB(5, 9, 12);
const COLORREF kScan = RGB(8, 15, 20);
const COLORREF kPanel = RGB(9, 20, 27);
const COLORREF kPanelHi = RGB(12, 34, 45);
const COLORREF kLine = RGB(40, 110, 135);
const COLORREF kCyan = RGB(90, 210, 255);
const COLORREF kCyanDim = RGB(50, 140, 175);
const COLORREF kText = RGB(215, 238, 248);
const COLORREF kMuted = RGB(110, 145, 160);
const COLORREF kRed = RGB(220, 60, 45);
const COLORREF kPlayText = RGB(4, 16, 22);

const wchar_t* kVersion = L"1.0";
const wchar_t* kAuthor = L"Piscacho";

enum Page { kHome, kGraphics, kControls, kAudio, kOnline, kAdvanced, kPages };
const wchar_t* kPageNames[kPages] = { L"INICIO", L"GRÁFICOS", L"CONTROLES", L"AUDIO", L"EN LÍNEA", L"AVANZADO" };

// Hit-test ids.
enum : int
{
    kIdPlay = 1, kIdSave, kIdBrowse, kIdOpenLogs, kIdOpenSaves, kIdResetControls, kIdVolume,
    kIdPageBase = 100, kIdChoiceBase = 1000, kIdToggleBase = 2000, kIdBindBase = 4000, kIdTextBase = 5000,
};

// A setting with fixed choices (shown as  <  value  >).
struct Choice
{
    const char* key;
    const wchar_t* label;
    const wchar_t* hint;
    std::vector<std::pair<std::string, std::wstring>> options;  // ini value, text
    const char* fallback;
};
// A yes/no setting.
struct Toggle
{
    const char* key;
    const wchar_t* label;
    const wchar_t* hint;
    bool fallback;
};

const Choice kChoices[] = {
    { "escala", L"Resolución interna", L"Multiplica la de la Wii (640x480). Más alta = imagen más nítida.",
        { { "1", L"1x  ·  640x480 (Wii)" }, { "2", L"2x  ·  1280x960" }, { "3", L"3x  ·  1920x1440" },
          { "4", L"4x  ·  2560x1920" }, { "5", L"5x  ·  3200x2400" }, { "6", L"6x  ·  3840x2880" } }, "3" },
    { "fps", L"Fotogramas por segundo", L"Fotogramas interpolados entre los del juego (que va a 30). Uno más de retraso.",
        { { "0", L"30 (original)" }, { "60", L"60" }, { "120", L"120" }, { "144", L"144" }, { "165", L"165" },
          { "240", L"240" } }, "240" },
    { "ventana", L"Tamaño de la ventana", L"En modo ventana. F11 o Alt+Intro alternan la pantalla completa.",
        { { "1280x960", L"1280 x 960" }, { "1600x1200", L"1600 x 1200" }, { "1920x1080", L"1920 x 1080" },
          { "2560x1440", L"2560 x 1440" } }, "1280x960" },
    { "sens_mando", L"Velocidad del puntero con mando", L"Stick derecho del mando de Xbox.",
        { { "0.5", L"0,5x" }, { "0.75", L"0,75x" }, { "1.0", L"1x" }, { "1.5", L"1,5x" }, { "2.0", L"2x" } }, "1.0" },
    { "aspecto", L"Relación de aspecto", L"21:9 y 32:9 amplían la vista a los lados; el HUD queda en el centro.",
        { { "4:3", L"4:3 (original)" }, { "16:9", L"16:9" }, { "21:9", L"21:9 ultrapanorámico" },
          { "32:9", L"32:9 superultrapanorámico" } }, "4:3" },
    { "mando_jugador2", L"Mando del jugador 2", L"El jugador 1 usa teclado, ratón y el mando 1 (si no es este).",
        { { "0", L"Mando 1" }, { "1", L"Mando 2" }, { "2", L"Mando 3" }, { "3", L"Mando 4" } }, "1" },
    { "online", L"Modo", L"Al pulsar JUGAR se conecta antes de empezar el juego.",
        { { "no", L"Desactivado" }, { "crear", L"Crear partida (J1)" }, { "unirse", L"Unirse (J2)" } }, "no" },
    { "online_retraso", L"Retraso de los controles", L"Más alto aguanta mejor las conexiones lentas; más bajo responde antes.",
        { { "2", L"2 fotogramas (red local)" }, { "3", L"3 fotogramas" }, { "4", L"4 fotogramas" }, { "5", L"5 fotogramas" },
          { "6", L"6 fotogramas (lejos)" } }, "3" },
};
enum { kChoiceScale, kChoiceFps, kChoiceWindow, kChoicePad, kChoiceAspect, kChoiceP2Pad, kChoiceOnline, kChoiceDelay, kChoiceCount };

// Free text settings (an address, a port): click, type, Intro.
struct TextSetting
{
    const char* key;
    const wchar_t* label;
    const wchar_t* hint;
    const char* fallback;
};
const TextSetting kTexts[] = {
    { "online_direccion", L"Dirección del anfitrión", L"La IP:puerto que le aparece al que crea la partida.", "" },
    { "online_puerto", L"Puerto UDP", L"El anfitrión lo abre en su router (UPnP automático) o en su VPN.", "7350" },
};
enum { kTextAddress, kTextPort, kTextCount };

const Toggle kToggles[] = {
    { "pantalla_completa", L"Pantalla completa", L"Sin bordes, en el monitor de la ventana.", false },
    { "efectos_audio", L"Efectos de sonido (reverberación)", L"Los buses de efectos del juego (eco, reverb).", true },
    { "mostrar_launcher", L"Mostrar este launcher al abrir", L"Si se desactiva, mantén Mayús al abrir para verlo.", true },
    { "profundidad_campo", L"Desenfoque de profundidad (original)", L"Lo lejano se ve borroso, como en Wii. Más fuerte en 21:9 y 32:9.", false },
    { "vibracion", L"Vibración del mando", L"La vibración del Wii Remote, en el mando de Xbox.", true },
    { "jugador2", L"Segundo jugador (cooperativo local)", L"Un segundo Wii Remote + Nunchuk con otro mando de Xbox.", false },
};
enum { kToggleFullscreen, kToggleEffects, kToggleLauncher, kToggleDof, kToggleRumble, kToggleCoop, kToggleCount };

// --- State -----------------------------------------------------------------------------
struct Hit
{
    RECT r;
    int id;
};

struct State
{
    HWND wnd = nullptr;
    int dpi = 96;
    IniFile ini;
    Page page = kHome;
    std::vector<Hit> hits;
    int hover = 0;
    bool play = false, done = false, dirty = false;
    int capture = -1;          // binding being captured: action * 2 + (0 keys, 1 pad)
    WORD padBefore = 0;        // gamepad buttons held when the capture started
    int scroll = 0;            // controls page
    int dragVolume = false;
    int editing = -1;          // text setting being typed
    std::wstring status;
    HBITMAP banner = nullptr;
    int bannerW = 0, bannerH = 0;
    IWICImagingFactory* wic = nullptr;
    HFONT title = nullptr, subtitle = nullptr, heading = nullptr, body = nullptr, smallFont = nullptr, button = nullptr,
          playFont = nullptr;
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }

std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(0, n - 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string IniValue(const char* key, const char* fallback) { return g.ini.Get(key, fallback); }
void SetIni(const char* key, const std::string& value)
{
    g.ini.Set(key, value);
    g.dirty = true;
}

std::string ChoiceValue(int i)
{
    if (i == kChoiceWindow)
        return IniValue("ancho_ventana", "1280") + "x" + IniValue("alto_ventana", "960");
    return IniValue(kChoices[i].key, kChoices[i].fallback);
}
void SetChoice(int i, const std::string& v)
{
    if (i == kChoiceWindow)
    {
        size_t x = v.find('x');
        SetIni("ancho_ventana", v.substr(0, x));
        SetIni("alto_ventana", v.substr(x + 1));
    }
    else
        SetIni(kChoices[i].key, v);
}
int ChoiceIndex(int i)
{
    std::string v = ChoiceValue(i);
    const auto& o = kChoices[i].options;
    for (size_t k = 0; k < o.size(); k++)
        if (o[k].first == v)
            return int(k);
    return -1;
}
bool ToggleValue(int i)
{
    std::string v = IniValue(kToggles[i].key, kToggles[i].fallback ? "1" : "0");
    return v == "1" || v == "si" || v == "true";
}
std::string BindingText(int action, int kind)
{
    const ControlAction& a = kControlActions[action];
    return IniValue((std::string(kind ? "mando_" : "tecla_") + a.id).c_str(), kind ? a.pad : a.keys);
}

// --- Drawing helpers -------------------------------------------------------------------
HFONT MakeFont(int size, int weight, const wchar_t* face)
{
    return CreateFontW(-S(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void FillRectColor(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// A panel with cut corners (top-left and bottom-right), the interface's signature shape.
void Chamfer(HDC dc, RECT r, int cut, COLORREF fill, COLORREF border, int width = 1)
{
    POINT pts[6] = { { r.left + cut, r.top }, { r.right, r.top }, { r.right, r.bottom - cut },
        { r.right - cut, r.bottom }, { r.left, r.bottom }, { r.left, r.top + cut } };
    HBRUSH b = fill == CLR_NONE ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN p = border == CLR_NONE ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, width, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (fill != CLR_NONE)
        DeleteObject(b);
    if (border != CLR_NONE)
        DeleteObject(p);
}

void Line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int width = 1)
{
    HPEN p = CreatePen(PS_SOLID, width, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, op);
    DeleteObject(p);
}

void Text(HDC dc, const std::wstring& t, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE,
    int spacing = 0)
{
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, f);
    SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, of);
}

// --- Pictures (launcher\*.jpg / *.png next to DSE.exe; art from SteamGridDB) ----------
// Decoded once with WIC; each size they are drawn at is scaled once (Fant filter) and kept.
enum Picture { kPicArt, kPicAegis, kPicIshimura, kPicMarker, kPicLogo, kPicCover, kPictures };
const char* kPictureFiles[kPictures] = { "heroe_arte.jpg", "heroe_aegis.jpg", "heroe_ishimura.jpg",
    "heroe_marcador.jpg", "logo.png", "portada_wii.jpg" };
struct PictureData
{
    IWICBitmapSource* source = nullptr;  // premultiplied BGRA, decoded in memory
    UINT w = 0, h = 0;
    struct Scaled { RECT src; int w, h; HBITMAP bmp; };
    std::vector<Scaled> cache;
} g_pics[kPictures];

void LoadPictures()
{
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic))))
        return;
    for (int i = 0; i < kPictures; i++)
    {
        std::wstring path = Wide(ExeFolder() + "\\launcher\\" + kPictureFiles[i]);
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapSource* conv = nullptr;
        if (SUCCEEDED(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) &&
            SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame, &conv)))
        {
            IWICBitmap* mem = nullptr;
            if (SUCCEEDED(g.wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &mem)))
            {
                g_pics[i].source = mem;
                mem->GetSize(&g_pics[i].w, &g_pics[i].h);
            }
            conv->Release();
        }
        if (frame) frame->Release();
        if (dec) dec->Release();
    }
}

void FreePictures()
{
    for (PictureData& p : g_pics)
    {
        for (auto& c : p.cache)
            DeleteObject(c.bmp);
        p.cache.clear();
        if (p.source)
            p.source->Release();
        p.source = nullptr;
    }
    if (g.wic)
        g.wic->Release();
    g.wic = nullptr;
}

// The part `src` of a picture scaled to w x h, as a premultiplied 32-bit DIB (kept).
HBITMAP ScaledPicture(int pic, RECT src, int w, int h)
{
    PictureData& p = g_pics[pic];
    if (!p.source || w <= 0 || h <= 0)
        return nullptr;
    for (auto& c : p.cache)
        if (EqualRect(&c.src, &src) && c.w == w && c.h == h)
            return c.bmp;
    if (p.cache.size() > 12)
    {
        DeleteObject(p.cache.front().bmp);
        p.cache.erase(p.cache.begin());
    }
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    HBITMAP bmp = nullptr;
    WICRect wr{ src.left, src.top, src.right - src.left, src.bottom - src.top };
    if (SUCCEEDED(g.wic->CreateBitmapClipper(&clip)) && SUCCEEDED(clip->Initialize(p.source, &wr)) &&
        SUCCEEDED(g.wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(clip, UINT(w), UINT(h), WICBitmapInterpolationModeFant)))
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && FAILED(scaler->CopyPixels(nullptr, UINT(w) * 4, UINT(w) * UINT(h) * 4, (BYTE*)bits)))
        {
            DeleteObject(bmp);
            bmp = nullptr;
        }
    }
    if (scaler) scaler->Release();
    if (clip) clip->Release();
    if (bmp)
        p.cache.push_back({ src, w, h, bmp });
    return bmp;
}

// Draws the part `src` of a picture into `dst` (alpha 0-255 on top of its own transparency).
void DrawPicture(HDC dc, int pic, RECT dst, RECT src, int alpha = 255)
{
    int w = dst.right - dst.left, h = dst.bottom - dst.top;
    HBITMAP bmp = ScaledPicture(pic, src, w, h);
    if (!bmp)
        return;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, BYTE(alpha), AC_SRC_ALPHA };
    AlphaBlend(dc, dst.left, dst.top, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

// Fills `dst` with the picture, cropped to keep its proportions; fx/fy (0-1) choose which part
// stays when it is cropped.
void DrawPictureCover(HDC dc, int pic, RECT dst, float fx = 0.5f, float fy = 0.5f, int alpha = 255)
{
    const PictureData& p = g_pics[pic];
    if (!p.source)
        return;
    int w = dst.right - dst.left, h = dst.bottom - dst.top;
    double scale = std::max(double(w) / p.w, double(h) / p.h);
    int sw = std::min(int(p.w), int(w / scale + 0.5)), sh = std::min(int(p.h), int(h / scale + 0.5));
    int sx = int((p.w - sw) * fx), sy = int((p.h - sh) * fy);
    DrawPicture(dc, pic, dst, RECT{ sx, sy, sx + sw, sy + sh }, alpha);
}

// The whole picture fitted into a width (height follows its proportions); returns the height.
int DrawPictureWidth(HDC dc, int pic, int x, int y, int w, int alpha = 255)
{
    const PictureData& p = g_pics[pic];
    if (!p.source)
        return 0;
    int h = int(double(w) * p.h / p.w + 0.5);
    DrawPicture(dc, pic, RECT{ x, y, x + w, y + h }, RECT{ 0, 0, LONG(p.w), LONG(p.h) }, alpha);
    return h;
}

bool HasPicture(int pic) { return g_pics[pic].source != nullptr; }

// Translucent fill (alpha 0-255), clipped to a cut-corner panel when `cut` > 0.
void FillAlpha(HDC dc, RECT r, COLORREF c, int alpha, int cut = 0)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 1;
    bi.bmiHeader.biHeight = 1;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP px = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!px)
        return;
    *(DWORD*)bits = 0xFF000000u | (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
    HRGN rgn = nullptr;
    if (cut > 0)
    {
        POINT pts[6] = { { r.left + cut, r.top }, { r.right, r.top }, { r.right, r.bottom - cut },
            { r.right - cut, r.bottom }, { r.left, r.bottom }, { r.left, r.top + cut } };
        rgn = CreatePolygonRgn(pts, 6, WINDING);
        SelectClipRgn(dc, rgn);
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, px);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, BYTE(alpha), 0 };
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(px);
    if (rgn)
    {
        SelectClipRgn(dc, nullptr);
        DeleteObject(rgn);
    }
}

// A vertical fade from transparent (top) to `c` (bottom) over r, in bands.
void FadeDown(HDC dc, RECT r, COLORREF c, int maxAlpha)
{
    int h = r.bottom - r.top, steps = std::max(1, h / S(4));
    for (int i = 0; i < steps; i++)
    {
        RECT band{ r.left, r.top + h * i / steps, r.right, r.top + h * (i + 1) / steps };
        FillAlpha(dc, band, c, maxAlpha * (i + 1) / steps);
    }
}

void AddHit(RECT r, int id) { g.hits.push_back({ r, id }); }
bool Hovered(int id) { return g.hover == id; }

// Button: outlined (or filled when `primary`), cut corners.
void Button(HDC dc, RECT r, const std::wstring& text, int id, bool primary = false)
{
    bool hot = Hovered(id);
    if (primary)
        Chamfer(dc, r, S(10), hot ? RGB(140, 230, 255) : kCyan, CLR_NONE);
    else
        Chamfer(dc, r, S(8), hot ? kPanelHi : kPanel, hot ? kCyan : kLine);
    Text(dc, text, r, primary ? kPlayText : (hot ? kCyan : kText), primary ? g.playFont : g.button,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE, primary ? S(3) : S(1));
    AddHit(r, id);
}

// Section heading: spaced uppercase cyan text with a rule and three ticks.
int Section(HDC dc, int x, int y, int w, const std::wstring& t)
{
    RECT r{ x, y, x + w, y + S(26) };
    Text(dc, t, r, kCyan, g.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    Line(dc, x, y + S(30), x + w, y + S(30), kLine);
    for (int k = 0; k < 3; k++)
        Line(dc, x + w - S(6) - k * S(8), y + S(27), x + w - S(10) - k * S(8), y + S(33), kCyanDim);
    return y + S(44);
}

// A labelled row: label and hint on the left, the control on the right; returns next y.
int RowLabel(HDC dc, int x, int y, int w, const wchar_t* label, const wchar_t* hint)
{
    RECT lr{ x, y, x + w / 2, y + S(22) };
    Text(dc, label, lr, kText, g.body);
    if (hint && *hint)
    {
        RECT hr{ x, y + S(22), x + w / 2 + S(60), y + S(40) };
        Text(dc, hint, hr, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    return y;
}

void ChoiceRow(HDC dc, int x, int& y, int w, int i)
{
    const Choice& c = kChoices[i];
    RowLabel(dc, x, y, w, c.label, c.hint);
    int cw = S(250), cx = x + w - cw;
    RECT box{ cx, y, cx + cw, y + S(34) };
    Chamfer(dc, box, S(6), kPanel, kLine);
    int idx = ChoiceIndex(i);
    std::wstring text = idx >= 0 ? c.options[size_t(idx)].second : Wide(ChoiceValue(i));
    RECT tr{ cx + S(34), y, cx + cw - S(34), y + S(34) };
    Text(dc, text, tr, kText, g.body, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT left{ cx, y, cx + S(34), y + S(34) }, right{ cx + cw - S(34), y, cx + cw, y + S(34) };
    int lid = kIdChoiceBase + i * 2, rid = lid + 1;
    Text(dc, L"◀", left, Hovered(lid) ? kCyan : kCyanDim, g.body, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(dc, L"▶", right, Hovered(rid) ? kCyan : kCyanDim, g.body, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHit(left, lid);
    AddHit(right, rid);
    y += S(54);
}

void ToggleRow(HDC dc, int x, int& y, int w, int i)
{
    const Toggle& t = kToggles[i];
    RowLabel(dc, x, y, w, t.label, t.hint);
    bool on = ToggleValue(i);
    int id = kIdToggleBase + i;
    int tw = S(84), tx = x + w - tw;
    RECT box{ tx, y + S(4), tx + tw, y + S(30) };
    Chamfer(dc, box, S(6), on ? kPanelHi : kPanel, on || Hovered(id) ? kCyan : kLine);
    RECT knob = on ? RECT{ box.right - S(36), box.top + S(4), box.right - S(4), box.bottom - S(4) }
                   : RECT{ box.left + S(4), box.top + S(4), box.left + S(36), box.bottom - S(4) };
    Chamfer(dc, knob, S(4), on ? kCyan : kLine, CLR_NONE);
    RECT label{ box.left, box.top, box.right, box.bottom };
    if (on)
        label.right -= S(38);
    else
        label.left += S(38);
    Text(dc, on ? L"SÍ" : L"NO", label, on ? kCyan : kMuted, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    AddHit(box, id);
    y += S(54);
}

// --- Pages -----------------------------------------------------------------------------
void PaintHome(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    RECT banner{ x, y, area.right, y + S(250) };
    FillRectColor(dc, banner, RGB(0, 0, 0));
    if (HasPicture(kPicArt))
    {
        // The key art (Nathan McNeill and Gabe Weller on the right); the logo over its left side,
        // darkened towards the edge.
        DrawPictureCover(dc, kPicArt, banner, 0.62f, 0.25f);
        int lw = (banner.right - banner.left) * 60 / 100;
        for (int i = 0; i < 12; i++)
        {
            RECT band{ banner.left + lw * i / 12, banner.top, banner.left + lw * (i + 1) / 12, banner.bottom };
            FillAlpha(dc, band, RGB(2, 6, 9), 190 - i * 15);
        }
        if (HasPicture(kPicLogo))
            DrawPictureWidth(dc, kPicLogo, banner.left + S(26), banner.top + S(28), (banner.right - banner.left) * 40 / 100);
    }
    else if (g.banner)
    {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.banner);
        SetStretchBltMode(dc, HALFTONE);
        // The logo band of the title screen (11% to 49% of its height), whole, centered.
        int srcY = g.bannerH * 11 / 100, srcH = g.bannerH * 38 / 100;
        int dh = banner.bottom - banner.top - S(54), dw = g.bannerW * dh / std::max(1, srcH);  // above the name band
        dw = std::min(dw, int(banner.right - banner.left));
        int srcW = std::min(g.bannerW, dw * srcH / std::max(1, dh));
        int dx = banner.left + (banner.right - banner.left - dw) / 2;
        StretchBlt(dc, dx, banner.top, dw, dh, mem, (g.bannerW - srcW) / 2, srcY, srcW, srcH, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    // Scanlines over the picture, a dark band at the bottom.
    for (int yy = banner.top; yy < banner.bottom; yy += S(3))
        FillAlpha(dc, RECT{ banner.left, yy, banner.right, yy + 1 }, RGB(0, 0, 0), 90);
    RECT band{ banner.left, banner.bottom - S(40), banner.right, banner.bottom };
    FadeDown(dc, RECT{ banner.left, band.top - S(28), banner.right, band.top }, RGB(4, 10, 14), 200);
    FillAlpha(dc, band, RGB(4, 10, 14), 200);
    Line(dc, banner.left, band.top, banner.right, band.top, kLine);
    Chamfer(dc, banner, S(14), CLR_NONE, kLine);
    RECT name{ band.left + S(20), band.top, band.right - S(34), band.bottom };
    Text(dc, L"AEGIS VII  \u00b7  OPERACI\u00d3N DE EXTRACCI\u00d3N", name, kCyanDim, g.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    Text(dc, L"RZJE69  \u00b7  DE WII A PC", name, kCyanDim, g.smallFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    y = banner.bottom + S(16);
    y = Section(dc, x, y, w, L"AJUSTES RÁPIDOS");
    ChoiceRow(dc, x, y, w, kChoiceScale);
    ChoiceRow(dc, x, y, w, kChoiceFps);
    ToggleRow(dc, x, y, w, kToggleFullscreen);
    ChoiceRow(dc, x, y, w, kChoiceAspect);
    // The basic controls, as a reminder.
    RECT card{ x, y - S(4), area.right, y + S(56) };
    Chamfer(dc, card, S(10), kPanel, kLine);
    RECT c1{ card.left + S(16), card.top + S(4), card.right - S(16), card.top + S(28) };
    Text(dc, L"RATÓN: apuntar   ·   CLIC IZQ.: disparar   ·   CLIC DER. / ESPACIO: A", c1, kText, g.smallFont,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT c2{ card.left + S(16), card.top + S(28), card.right - S(16), card.top + S(52) };
    Text(dc, L"F: golpe (Nunchuk)   ·   G: cargar la luz   ·   R: fuego alternativo   ·   WASD: Nunchuk   ·   INTRO: pausa", c2,
        kMuted, g.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void PaintGraphics(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"IMAGEN");
    ChoiceRow(dc, x, y, w, kChoiceScale);
    ChoiceRow(dc, x, y, w, kChoiceAspect);
    ToggleRow(dc, x, y, w, kToggleDof);
    y = Section(dc, x, y + S(6), w, L"PANTALLA");
    ChoiceRow(dc, x, y, w, kChoiceFps);
    ToggleRow(dc, x, y, w, kToggleFullscreen);
    ChoiceRow(dc, x, y, w, kChoiceWindow);
}

void PaintControls(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"CONTROLES  ·  WII REMOTE + NUNCHUK");
    // Column titles.
    int colKeys = x + w - S(440), colPad = x + w - S(200);
    RECT t1{ colKeys, y - S(8), colKeys + S(230), y + S(12) }, t2{ colPad, y - S(8), colPad + S(200), y + S(12) };
    Text(dc, L"TECLADO / RATÓN", t1, kMuted, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    Text(dc, L"MANDO", t2, kMuted, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    y += S(16);
    int listTop = y, listBottom = area.bottom - S(54);
    HRGN clip = CreateRectRgn(area.left, listTop, area.right, listBottom);
    SelectClipRgn(dc, clip);
    int row = S(38);
    int maxScroll = std::max(0, kControlActionCount * row - (listBottom - listTop));
    g.scroll = std::clamp(g.scroll, 0, maxScroll);
    for (int i = 0; i < kControlActionCount; i++)
    {
        int ry = listTop + i * row - g.scroll;
        if (ry + row < listTop || ry > listBottom)
            continue;
        if (i % 2 == 0)
        {
            RECT stripe{ x, ry, area.right, ry + row };
            FillRectColor(dc, stripe, RGB(7, 15, 20));
        }
        RECT lr{ x + S(10), ry, colKeys - S(10), ry + row };
        Text(dc, kControlActions[i].label, lr, kText, g.body);
        for (int kind = 0; kind < 2; kind++)
        {
            int bx = kind ? colPad : colKeys, bw = kind ? S(200) : S(230);
            RECT b{ bx, ry + S(4), bx + bw, ry + row - S(4) };
            int id = kIdBindBase + i * 2 + kind;
            bool capturing = g.capture == i * 2 + kind;
            Chamfer(dc, b, S(5), capturing ? kPanelHi : kPanel, capturing || Hovered(id) ? kCyan : kLine);
            std::wstring text = capturing ? (kind ? L"Pulsa un botón del mando…" : L"Pulsa una tecla o botón…")
                                          : Wide(BindingText(i, kind));
            if (text.empty())
                text = L"—";
            Text(dc, text, b, capturing ? kCyan : kText, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT visible{ b.left, std::max(b.top, LONG(listTop)), b.right, std::min(b.bottom, LONG(listBottom)) };
            if (visible.bottom > visible.top)
                AddHit(visible, id);
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    if (maxScroll > 0)  // scroll bar
    {
        int track = listBottom - listTop;
        int thumb = std::max(S(30), track * track / (track + maxScroll));
        int ty = listTop + (track - thumb) * g.scroll / maxScroll;
        RECT bar{ area.right + S(8), ty, area.right + S(12), ty + thumb };
        FillRectColor(dc, bar, kCyanDim);
    }
    RECT hint{ x, listBottom + S(8), x + w - S(200), listBottom + S(44) };
    Text(dc, L"Clic para cambiar · clic derecho para borrar · Esc cancela.", hint, kMuted, g.smallFont);
    Button(dc, RECT{ area.right - S(190), listBottom + S(8), area.right, listBottom + S(44) }, L"RESTABLECER",
        kIdResetControls);
}

void PaintAudio(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"SONIDO");
    RowLabel(dc, x, y, w, L"Volumen general", L"Todo el sonido del juego.");
    int vol = std::clamp(atoi(IniValue("volumen", "100").c_str()), 0, 100);
    int sw = S(250), sx = x + w - sw;
    RECT track{ sx, y + S(14), sx + sw - S(60), y + S(20) };
    Chamfer(dc, track, S(2), kPanel, kLine);
    RECT fill{ track.left, track.top, track.left + (track.right - track.left) * vol / 100, track.bottom };
    FillRectColor(dc, fill, kCyan);
    int kx = fill.right;
    RECT knob{ kx - S(6), y + S(6), kx + S(6), y + S(28) };
    Chamfer(dc, knob, S(3), Hovered(kIdVolume) || g.dragVolume ? RGB(160, 235, 255) : kCyan, CLR_NONE);
    RECT num{ track.right + S(8), y, sx + sw, y + S(34) };
    Text(dc, std::to_wstring(vol) + L" %", num, kText, g.body, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    AddHit(RECT{ track.left - S(8), y, track.right + S(8), y + S(34) }, kIdVolume);
    y += S(54);
    ToggleRow(dc, x, y, w, kToggleEffects);
}

// A text setting: label and hint, and an edit box (cyan with a caret while typing).
void TextRow(HDC dc, int x, int& y, int w, int i)
{
    const TextSetting& t = kTexts[i];
    RowLabel(dc, x, y, w, t.label, t.hint);
    int bw = S(250), bx = x + w - bw;
    RECT box{ bx, y, bx + bw, y + S(34) };
    int id = kIdTextBase + i;
    bool editing = g.editing == i;
    Chamfer(dc, box, S(6), editing ? kPanelHi : kPanel, editing || Hovered(id) ? kCyan : kLine);
    std::wstring v = Wide(IniValue(t.key, t.fallback));
    if (editing && (GetTickCount64() / 500) % 2 == 0)
        v += L"|";
    RECT tr{ box.left + S(12), box.top, box.right - S(12), box.bottom };
    Text(dc, v.empty() ? L"(escribe aquí)" : v, tr, v.empty() ? kMuted : kText, g.body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddHit(box, id);
    y += S(54);
}

void PaintOnline(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"JUEGO EN LÍNEA  ·  COOPERATIVO PARA 2");
    ChoiceRow(dc, x, y, w, kChoiceOnline);
    std::string mode = IniValue("online", "no");
    if (mode == "unirse")
        TextRow(dc, x, y, w, kTextAddress);
    if (mode != "no")
    {
        TextRow(dc, x, y, w, kTextPort);
        ChoiceRow(dc, x, y, w, kChoiceDelay);
    }
    RECT card{ x, y + S(4), area.right, y + S(4) + S(124) };
    FillAlpha(dc, card, RGB(5, 12, 17), 200, S(10));
    Chamfer(dc, card, S(10), CLR_NONE, kLine);
    RECT t{ card.left + S(16), card.top + S(10), card.right - S(16), card.bottom - S(10) };
    const wchar_t* help =
        L"Los dos PCs ejecutan el juego y solo se envían los controles: los dos necesitáis el mismo DSE.exe y vuestra "
        L"copia del juego. El que crea la partida es el jugador 1 y lleva su partida guardada; el que se une es el "
        L"jugador 2.\n\nEl anfitrión ve su dirección al pulsar JUGAR. Por internet, su router debe dejar pasar el "
        L"puerto UDP (se intenta abrir solo con UPnP); si no, usad una VPN como Radmin VPN o ZeroTier y la IP que os dé.";
    Text(dc, help, t, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void PaintAdvanced(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"JUEGO");
    RowLabel(dc, x, y, w, L"Carpeta del juego", L"La partición de datos extraída del disco (con sys\\main.dol).");
    y += S(44);
    RECT path{ x, y, x + w - S(150), y + S(34) };
    Chamfer(dc, path, S(6), kPanel, kLine);
    RECT pt{ path.left + S(12), path.top, path.right - S(12), path.bottom };
    std::wstring folder = Wide(IniValue("carpeta_juego", ""));
    Text(dc, folder.empty() ? L"(sin elegir)" : folder, pt, folder.empty() ? kMuted : kText, g.smallFont,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    Button(dc, RECT{ x + w - S(140), y, x + w, y + S(34) }, L"ELEGIR…", kIdBrowse);
    y += S(56);
    y = Section(dc, x, y, w, L"MANDOS");
    ToggleRow(dc, x, y, w, kToggleRumble);
    ToggleRow(dc, x, y, w, kToggleCoop);
    if (ToggleValue(kToggleCoop))
        ChoiceRow(dc, x, y, w, kChoiceP2Pad);
    y = Section(dc, x, y + S(6), w, L"LAUNCHER");
    ToggleRow(dc, x, y, w, kToggleLauncher);
    y = Section(dc, x, y + S(6), w, L"CARPETAS");
    Button(dc, RECT{ x, y, x + S(220), y + S(36) }, L"PARTIDAS GUARDADAS", kIdOpenSaves);
    Button(dc, RECT{ x + S(236), y, x + S(456), y + S(36) }, L"REGISTRO (LOG)", kIdOpenLogs);
}

void Paint(HDC dc, RECT rc)
{
    g.hits.clear();
    FillRectColor(dc, rc, kBg);
    // Background: a picture of the game for each page, darkened, under scanlines.
    static const int kPageArt[kPages] = { kPicMarker, kPicAegis, kPicIshimura, kPicMarker, kPicIshimura, kPicAegis };
    int art = kPageArt[g.page];
    if (HasPicture(art))
    {
        DrawPictureCover(dc, art, rc, 0.5f, 0.4f);
        FillAlpha(dc, rc, RGB(3, 7, 10), 150);
    }
    for (int y = 0; y < rc.bottom; y += S(3))
        FillAlpha(dc, RECT{ 0, y, rc.right, y + 1 }, RGB(0, 0, 0), 110);

    // Sidebar.
    int side = S(250);
    RECT sb{ 0, 0, side, rc.bottom };
    FillAlpha(dc, sb, RGB(3, 6, 9), 215);
    Line(dc, side, 0, side, rc.bottom, kLine);
    if (HasPicture(kPicLogo))
        DrawPictureWidth(dc, kPicLogo, S(16), S(24), side - S(32));
    else
    {
        RECT t1{ S(24), S(26), side - S(10), S(70) };
        Text(dc, L"DEAD SPACE", t1, kText, g.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
        RECT t2{ S(26), S(70), side - S(10), S(92) };
        Text(dc, L"E X T R A C T I O N", t2, kCyan, g.subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(1));
    }
    Line(dc, S(24), S(104), side - S(24), S(104), kLine);
    RECT t3{ S(24), S(110), side - S(10), S(134) };
    Text(dc, L"PORT PC", t3, kCyan, g.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(4));
    RECT t4{ S(24), S(134), side - S(10), S(154) };
    Text(dc, std::wstring(L"por ") + kAuthor + L"  ·  v" + kVersion, t4, kMuted, g.smallFont);

    int y = S(190);
    for (int p = 0; p < kPages; p++)
    {
        RECT r{ S(16), y, side - S(16), y + S(44) };
        int id = kIdPageBase + p;
        bool active = g.page == p;
        if (active)
            Chamfer(dc, r, S(10), kPanelHi, kCyan);
        else if (Hovered(id))
            Chamfer(dc, r, S(10), kPanel, kLine);
        RECT tr{ r.left + S(22), r.top, r.right, r.bottom };
        Text(dc, kPageNames[p], tr, active ? kText : (Hovered(id) ? kCyan : kMuted), g.button,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
        if (active)
            Line(dc, r.left + S(10), r.top + S(14), r.left + S(10), r.bottom - S(14), kCyan, S(3));
        AddHit(r, id);
        y += S(46);
    }
    // The Wii box at the bottom of the sidebar, if there is room for it.
    if (HasPicture(kPicCover))
    {
        int coverW = S(112), coverH = int(double(coverW) * g_pics[kPicCover].h / g_pics[kPicCover].w);
        int cx = (side - coverW) / 2, cy = rc.bottom - S(74) - coverH;
        if (cy > y + S(10))
        {
            RECT frame{ cx - S(4), cy - S(4), cx + coverW + S(4), cy + coverH + S(4) };
            FillAlpha(dc, frame, RGB(0, 0, 0), 160);
            DrawPictureWidth(dc, kPicCover, cx, cy, coverW);
            Chamfer(dc, frame, S(8), CLR_NONE, kLine);
        }
    }
    RECT note{ S(24), rc.bottom - S(60), side - S(16), rc.bottom - S(14) };
    Text(dc, L"En el juego: F11 o Alt+Intro\npantalla completa.", note, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // Content panel.
    RECT content{ side + S(30), S(26), rc.right - S(34), rc.bottom - S(96) };
    RECT panel{ content.left - S(14), content.top - S(10), content.right + S(14), content.bottom + S(10) };
    FillAlpha(dc, panel, RGB(5, 12, 17), 185, S(18));
    Chamfer(dc, panel, S(18), CLR_NONE, kLine);
    // Pages with room to spare end with a strip of the page's picture.
    if ((g.page == kGraphics || g.page == kAudio) && HasPicture(art))
    {
        RECT strip{ content.left, content.bottom - S(170), content.right, content.bottom };
        DrawPictureCover(dc, art, strip, 0.5f, 0.45f, 235);
        for (int i = 0; i < 15; i++)  // fades in from the panel above
        {
            RECT band{ strip.left, strip.top + S(4) * i, strip.right, strip.top + S(4) * (i + 1) };
            FillAlpha(dc, band, RGB(5, 12, 17), 230 - i * 15);
        }
        for (int yy = strip.top; yy < strip.bottom; yy += S(3))
            FillAlpha(dc, RECT{ strip.left, yy, strip.right, yy + 1 }, RGB(0, 0, 0), 80);
        Chamfer(dc, strip, S(12), CLR_NONE, kLine);
    }
    switch (g.page)
    {
    case kHome: PaintHome(dc, content); break;
    case kGraphics: PaintGraphics(dc, content); break;
    case kControls: PaintControls(dc, content); break;
    case kAudio: PaintAudio(dc, content); break;
    case kOnline: PaintOnline(dc, content); break;
    default: PaintAdvanced(dc, content); break;
    }

    // Bottom bar: status, save, play.
    RECT bar{ side, rc.bottom - S(72), rc.right, rc.bottom };
    FillAlpha(dc, bar, RGB(3, 7, 10), 225);
    Line(dc, side, bar.top, rc.right, bar.top, kLine);
    RECT st{ side + S(30), bar.top, rc.right - S(420), bar.bottom };
    std::wstring status = g.status.empty() ? (g.dirty ? L"Cambios sin guardar." : L"Listo.") : g.status;
    Text(dc, status, st, g.dirty ? kCyan : kMuted, g.smallFont);
    Button(dc, RECT{ rc.right - S(390), bar.top + S(16), rc.right - S(250), bar.bottom - S(16) }, L"GUARDAR", kIdSave);
    Button(dc, RECT{ rc.right - S(234), bar.top + S(12), rc.right - S(34), bar.bottom - S(12) }, L"JUGAR", kIdPlay, true);
}

// --- Actions ---------------------------------------------------------------------------
bool Save()
{
    if (!g.ini.Save())
    {
        g.status = L"No se pudo guardar dse.ini.";
        return false;
    }
    g.dirty = false;
    g.status = L"Guardado en dse.ini.";
    return true;
}

void BrowseFolder()
{
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Carpeta del juego (la que contiene sys\\main.dol)");
    if (SUCCEEDED(dlg->Show(g.wnd)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                std::wstring p = path;
                CoTaskMemFree(path);
                namespace fs = std::filesystem;
                // Accept the partition folder, or a folder holding it as DATA.
                if (!fs::exists(fs::path(p) / L"sys" / L"main.dol") && fs::exists(fs::path(p) / L"DATA" / L"sys" / L"main.dol"))
                    p = (fs::path(p) / L"DATA").wstring();
                if (fs::exists(fs::path(p) / L"sys" / L"main.dol"))
                {
                    SetIni("carpeta_juego", Narrow(p));
                    g.status = L"Carpeta del juego elegida.";
                }
                else
                    g.status = L"Esa carpeta no tiene sys\\main.dol.";
            }
            item->Release();
        }
    }
    dlg->Release();
}

void OpenFolder(const std::string& sub)
{
    std::wstring p = Wide(ExeFolder() + "\\" + sub);
    std::filesystem::create_directories(p);
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void SetVolumeFromX(int mx)
{
    for (const Hit& h : g.hits)
        if (h.id == kIdVolume)
        {
            int left = h.r.left + S(8), right = h.r.right - S(8);
            int v = std::clamp((mx - left) * 100 / std::max(1, right - left), 0, 100);
            SetIni("volumen", std::to_string(v));
        }
}

void Click(int id, bool right)
{
    if (id >= kIdTextBase && id < kIdTextBase + kTextCount)
    {
        g.editing = id - kIdTextBase;
        return;
    }
    g.editing = -1;
    if (id >= kIdPageBase && id < kIdPageBase + kPages)
    {
        g.page = Page(id - kIdPageBase);
        g.capture = -1;
        return;
    }
    if (id >= kIdChoiceBase && id < kIdChoiceBase + kChoiceCount * 2)
    {
        int i = (id - kIdChoiceBase) / 2, dir = ((id - kIdChoiceBase) & 1) ? 1 : -1;
        const auto& o = kChoices[i].options;
        int k = ChoiceIndex(i);
        k = k < 0 ? 0 : (k + dir + int(o.size())) % int(o.size());
        SetChoice(i, o[size_t(k)].first);
        return;
    }
    if (id >= kIdToggleBase && id < kIdToggleBase + kToggleCount)
    {
        int i = id - kIdToggleBase;
        SetIni(kToggles[i].key, ToggleValue(i) ? "0" : "1");
        return;
    }
    if (id >= kIdBindBase && id < kIdBindBase + kControlActionCount * 2)
    {
        int slot = id - kIdBindBase, action = slot / 2, kind = slot & 1;
        if (right)
        {
            SetIni((std::string(kind ? "mando_" : "tecla_") + kControlActions[action].id).c_str(), "");
            g.capture = -1;
        }
        else
        {
            g.capture = slot;
            XINPUT_STATE xs{};
            g.padBefore = XInputGetState(0, &xs) == ERROR_SUCCESS ? xs.Gamepad.wButtons : 0;
        }
        return;
    }
    switch (id)
    {
    case kIdPlay:
        if (g.dirty)
            Save();
        g.play = true;
        g.done = true;
        break;
    case kIdSave: Save(); break;
    case kIdBrowse: BrowseFolder(); break;
    case kIdOpenLogs: ShellExecuteW(nullptr, L"open", Wide(LogFolder()).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case kIdOpenSaves: OpenFolder("user\\nand"); break;
    case kIdResetControls:
        for (int i = 0; i < kControlActionCount; i++)
        {
            SetIni((std::string("tecla_") + kControlActions[i].id).c_str(), kControlActions[i].keys);
            SetIni((std::string("mando_") + kControlActions[i].id).c_str(), kControlActions[i].pad);
        }
        g.status = L"Controles restablecidos.";
        break;
    }
}

// A key or mouse button for the binding being captured.
void FinishKeyCapture(int vk)
{
    if (g.capture < 0 || (g.capture & 1))
        return;
    std::string name = KeyName(vk);
    if (!name.empty())
        SetIni((std::string("tecla_") + kControlActions[g.capture / 2].id).c_str(), name);
    g.capture = -1;
}

void PollPadCapture()
{
    if (g.capture < 0 || !(g.capture & 1))
        return;
    XINPUT_STATE xs{};
    if (XInputGetState(0, &xs) != ERROR_SUCCESS)
        return;
    uint32_t held = xs.Gamepad.wButtons | (xs.Gamepad.bLeftTrigger > 64 ? 0x10000u : 0u) |
        (xs.Gamepad.bRightTrigger > 64 ? 0x20000u : 0u);
    uint32_t fresh = held & ~uint32_t(g.padBefore);
    g.padBefore = WORD(held);
    for (uint32_t bit = 1; bit; bit <<= 1)
        if (fresh & bit)
        {
            std::string name = PadName(bit);
            if (!name.empty())
            {
                SetIni((std::string("mando_") + kControlActions[g.capture / 2].id).c_str(), name);
                g.capture = -1;
                InvalidateRect(g.wnd, nullptr, FALSE);
            }
            return;
        }
}

int HitAt(int x, int y)
{
    for (auto it = g.hits.rbegin(); it != g.hits.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom)
            return it->id;
    return 0;
}

LRESULT CALLBACK LauncherProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int hot = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g.dragVolume)
            SetVolumeFromX(GET_X_LPARAM(l));
        if (hot != g.hover || g.dragVolume)
        {
            g.hover = hot;
            InvalidateRect(h, nullptr, FALSE);
        }
        SetCursor(LoadCursor(nullptr, hot ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        g.hover = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT)
            return TRUE;
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (g.capture >= 0 && !(g.capture & 1))
        {
            // A mouse button for the binding (MOUSE1..5).
            int vk = m == WM_LBUTTONDOWN ? VK_LBUTTON : m == WM_RBUTTONDOWN ? VK_RBUTTON : m == WM_MBUTTONDOWN ? VK_MBUTTON
                : (GET_XBUTTON_WPARAM(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
            FinishKeyCapture(vk);
        }
        else if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN)
        {
            int id = HitAt(x, y);
            if (id == kIdVolume && m == WM_LBUTTONDOWN)
            {
                g.dragVolume = true;
                SetCapture(h);
                SetVolumeFromX(x);
            }
            else if (id)
            {
                g.status.clear();
                Click(id, m == WM_RBUTTONDOWN);
            }
        }
        InvalidateRect(h, nullptr, FALSE);
        if (g.done)
            DestroyWindow(h);
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    }
    case WM_LBUTTONUP:
        if (g.dragVolume)
        {
            g.dragVolume = false;
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (g.page == kControls)
        {
            g.scroll -= GET_WHEEL_DELTA_WPARAM(w) * S(38) / WHEEL_DELTA;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CHAR:
        if (g.editing >= 0)
        {
            const char* key = kTexts[g.editing].key;
            std::string v = IniValue(key, kTexts[g.editing].fallback);
            if (w == 8)  // backspace
            {
                if (!v.empty())
                    v.pop_back();
            }
            else if (w == 13 || w == 27)
                g.editing = -1;
            else if (w >= 32 && w < 127 && v.size() < 64)
                v += char(w);
            SetIni(key, v);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g.editing >= 0)
        {
            if (w == VK_RETURN || w == VK_ESCAPE)
            {
                g.editing = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;  // the characters come with WM_CHAR
        }
        if (g.capture >= 0 && !(g.capture & 1))
        {
            if (w == VK_ESCAPE)
                g.capture = -1;
            else
            {
                int vk = int(w);
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
                    vk = int(w);  // the generic modifier names
                FinishKeyCapture(vk);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g.capture >= 0 && w == VK_ESCAPE)
        {
            g.capture = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_RETURN && g.capture < 0)
        {
            Click(kIdPlay, false);
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_TIMER:
        PollPadCapture();
        if (g.editing >= 0)  // the caret blinks
        {
            static ULONGLONG last = 0;
            if (GetTickCount64() - last >= 500)
            {
                last = GetTickCount64();
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_CLOSE:
        if (g.dirty && MessageBoxW(h, L"Hay cambios sin guardar. ¿Guardarlos antes de salir?", L"Dead Space: Extraction",
            MB_YESNO | MB_ICONQUESTION) == IDYES)
            Save();
        g.done = true;
        DestroyWindow(h);
        return 0;
    case WM_DPICHANGED:
    {
        g.dpi = HIWORD(w);
        RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// The banner: launcher\portada.bmp next to DSE.exe (a picture of the game), if present.
void LoadBanner()
{
    std::wstring path = Wide(ExeFolder() + "\\launcher\\portada.bmp");
    g.banner = (HBITMAP)LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (g.banner)
    {
        BITMAP bm{};
        GetObject(g.banner, sizeof(bm), &bm);
        g.bannerW = bm.bmWidth;
        g.bannerH = abs(bm.bmHeight);
    }
}

void MakeFonts()
{
    const wchar_t* face = L"Bahnschrift";
    g.title = MakeFont(34, FW_BOLD, face);
    g.subtitle = MakeFont(13, FW_SEMIBOLD, face);
    g.heading = MakeFont(15, FW_SEMIBOLD, face);
    g.body = MakeFont(14, FW_NORMAL, face);
    g.smallFont = MakeFont(12, FW_NORMAL, face);
    g.button = MakeFont(14, FW_SEMIBOLD, face);
    g.playFont = MakeFont(20, FW_BOLD, face);
}
} // namespace

// Shows the launcher. True to start the game, false to quit.
bool LauncherRun()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g.ini.Load(ExeFolder() + "\\dse.ini");
    if (g.ini.lines.empty())
        g.ini.lines = { "# Dead Space: Extraction (port PC). Lo escribe el launcher; las claves que no conoce se conservan." };
    if (g.ini.Get("carpeta_juego", "").empty())
    {
        // First run: the extracted disc next to the project, if it is there.
        std::filesystem::path guess = std::filesystem::path(ExeFolder()) / ".." / ".." / ".." / "juego" / "DATA";
        if (std::filesystem::exists(guess / "sys" / "main.dol"))
            g.ini.Set("carpeta_juego", Narrow(std::filesystem::weakly_canonical(guess).wstring()));
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = LauncherProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"DSELauncher";
    RegisterClassW(&wc);

    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto f = (GetDpi)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"))
        f(mon, 0, &dx, &dy);
    g.dpi = int(dx);
    MakeFonts();
    LoadBanner();
    LoadPictures();

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{ 0, 0, S(1120), S(720) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, UINT(g.dpi));
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    g.wnd = CreateWindowW(wc.lpszClassName, L"Dead Space: Extraction · Port PC", style, x, y, w, h, nullptr, nullptr,
        wc.hInstance, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = RGB(4, 7, 10), border = kLine;
    DwmSetWindowAttribute(g.wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g.wnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    if (const char* page = getenv("DSE_LAUNCHER_PAGE"))  // screenshots of each page
        g.page = Page(std::clamp(atoi(page), 0, int(kPages) - 1));
    ShowWindow(g.wnd, SW_SHOW);
    SetTimer(g.wnd, 1, 30, nullptr);

    MSG msg;
    while (!g.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(g.wnd))
        DestroyWindow(g.wnd);
    for (HFONT f : { g.title, g.subtitle, g.heading, g.body, g.smallFont, g.button, g.playFont })
        DeleteObject(f);
    if (g.banner)
        DeleteObject(g.banner);
    FreePictures();
    return g.play;
}
