// Online play by lockstep. Both PCs run the game in deterministic mode (threads.cpp): the
// same controls on the same frames give the same game on both, so only the controls
// travel. Every video retrace each PC sends its player's controls for the frame `delay`
// retraces ahead, and waits for the other player's controls for the current one.
//
// Before the game boots (NetplayStart): the guest says hello with the hash of its DSE.exe,
// the host answers with its settings (input delay, wide screen) and sends its saves
// (user/nand), which the guest uses from user/nand_online (its own saves stay untouched).
// Then both boot at once. Every 120 retraces they compare a hash of MEM1: a difference
// means the games are no longer the same ("desync"), which the title bar shows.
//
// Network: one UDP port. The host opens it on the router by UPnP when it can and shows its
// local and public addresses; the guest types one in the launcher (ONLINE page).
#include "netplay.h"
#include "runtime.h"
#include "config.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void* VideoGetWindow();  // video.cpp
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace
{
constexpr uint8_t kVersion = 1;
enum : uint8_t
{
    kHello = 1,      // guest -> host: build hash (8)
    kWelcome = 2,    // host -> guest: ok (1), delay (1), wide (1), save size (4)
    kGetChunk = 3,   // guest -> host: chunk index (4)
    kChunk = 4,      // host -> guest: chunk index (4), data
    kReady = 5,      // guest -> host: has everything
    kStart = 6,      // host -> guest: boot now
    kInput = 7,      // both: ack (4: next frame wanted from the other), first frame (4), count (1), inputs
    kHash = 8,       // both: frame (4), hash (8)
    kBye = 9,
};
constexpr uint32_t kChunkSize = 1024;
constexpr size_t kInputBytes = 12;

std::mutex g_m;
std::condition_variable g_cv;
SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_peer{};
bool g_havePeer = false;
std::atomic<bool> g_active{ false }, g_running{ false };
bool g_host = false;
int g_delay = 3;
bool g_wide = false;
uint64_t g_build = 0;
uint16_t g_port = 7350;

// Session setup.
std::vector<uint8_t> g_save;           // host: packed saves; guest: what arrived
std::vector<bool> g_haveChunk;
uint32_t g_saveSize = 0;
bool g_welcomed = false, g_ready = false, g_started = false, g_buildMismatch = false;
std::wstring g_setupText;

// Lockstep.
std::vector<NetInput> g_local, g_remote;  // by frame
std::vector<bool> g_haveRemote;
uint32_t g_remoteWants = 0;               // first frame of ours the other PC has not got yet
uint32_t g_contiguous = 0;                // first remote frame we do not have
std::map<uint32_t, uint64_t> g_localHash, g_remoteHash;
bool g_desync = false;
bool g_lost = false;           // the other player left or stopped answering
uint32_t g_desyncFrame = 0;
std::atomic<uint32_t> g_rttMs{ 0 };
uint64_t g_lastRecv = 0;

uint64_t NowMs() { return GetTickCount64(); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back(uint8_t(v >> (i * 8))); }
void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void Put64(std::vector<uint8_t>& b, uint64_t v) { Put32(b, uint32_t(v >> 32)); Put32(b, uint32_t(v)); }
uint32_t Get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t Get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t Get64(const uint8_t* p) { return (uint64_t(Get32(p)) << 32) | Get32(p + 4); }

std::vector<uint8_t> Header(uint8_t type)
{
    std::vector<uint8_t> b = { 'D', 'S', 'E', 'N', kVersion, type };
    return b;
}
void Send(const std::vector<uint8_t>& b)
{
    if (g_havePeer)
        sendto(g_sock, (const char*)b.data(), int(b.size()), 0, (const sockaddr*)&g_peer, sizeof(g_peer));
}

void PackInput(std::vector<uint8_t>& b, const NetInput& in)
{
    Put32(b, in.buttons);
    Put16(b, uint16_t(in.pointer[0]));
    Put16(b, uint16_t(in.pointer[1]));
    b.push_back(uint8_t(in.stick[0]));
    b.push_back(uint8_t(in.stick[1]));
    b.push_back(in.flags);
    b.push_back(0);
}
NetInput UnpackInput(const uint8_t* p)
{
    NetInput in;
    in.buttons = Get32(p);
    in.pointer[0] = int16_t(Get16(p + 4));
    in.pointer[1] = int16_t(Get16(p + 6));
    in.stick[0] = int8_t(p[8]);
    in.stick[1] = int8_t(p[9]);
    in.flags = p[10];
    return in;
}

// Our inputs the other PC still needs (up to 40 frames per packet), and which of theirs we want.
void SendInputsLocked()
{
    uint32_t first = g_remoteWants;
    uint32_t last = uint32_t(g_local.size());  // exclusive
    if (last > first + 40)
        last = first + 40;
    std::vector<uint8_t> b = Header(kInput);
    Put32(b, g_contiguous);
    Put32(b, first);
    b.push_back(uint8_t(last > first ? last - first : 0));
    for (uint32_t f = first; f < last; f++)
        PackInput(b, g_local[f]);
    Send(b);
}

void HandlePacket(const uint8_t* d, int n, const sockaddr_in& from)
{
    if (n < 6 || memcmp(d, "DSEN", 4) != 0 || d[4] != kVersion)
        return;
    uint8_t type = d[5];
    const uint8_t* p = d + 6;
    int len = n - 6;
    std::lock_guard lock(g_m);
    // The host takes the first guest that says hello; after that only that address counts.
    if (!g_havePeer && g_host && type == kHello)
    {
        g_peer = from;
        g_havePeer = true;
    }
    if (!g_havePeer || from.sin_addr.s_addr != g_peer.sin_addr.s_addr || from.sin_port != g_peer.sin_port)
        return;
    g_lastRecv = NowMs();
    switch (type)
    {
    case kHello:
        if (g_host && len >= 8)
        {
            bool ok = Get64(p) == g_build;
            g_buildMismatch = !ok;
            std::vector<uint8_t> b = Header(kWelcome);
            b.push_back(ok ? 1 : 0);
            b.push_back(uint8_t(g_delay));
            b.push_back(g_wide ? 1 : 0);
            Put32(b, uint32_t(g_save.size()));
            Send(b);
        }
        break;
    case kWelcome:
        if (!g_host && len >= 7 && !g_welcomed)
        {
            g_welcomed = true;
            g_buildMismatch = p[0] == 0;
            g_delay = p[1];
            g_wide = p[2] != 0;
            g_saveSize = Get32(p + 3);
            g_save.assign(g_saveSize, 0);
            g_haveChunk.assign((g_saveSize + kChunkSize - 1) / kChunkSize, false);
        }
        break;
    case kGetChunk:
        if (g_host && len >= 4)
        {
            uint32_t i = Get32(p);
            if (size_t(i) * kChunkSize < g_save.size())
            {
                std::vector<uint8_t> b = Header(kChunk);
                Put32(b, i);
                size_t off = size_t(i) * kChunkSize, sz = std::min<size_t>(kChunkSize, g_save.size() - off);
                b.insert(b.end(), g_save.begin() + off, g_save.begin() + off + sz);
                Send(b);
            }
        }
        break;
    case kChunk:
        if (!g_host && len >= 4 && g_welcomed)
        {
            uint32_t i = Get32(p);
            size_t off = size_t(i) * kChunkSize;
            if (i < g_haveChunk.size() && !g_haveChunk[i] && off + (len - 4) <= g_save.size())
            {
                memcpy(g_save.data() + off, p + 4, size_t(len - 4));
                g_haveChunk[i] = true;
            }
        }
        break;
    case kReady:
        if (g_host)
        {
            g_ready = true;
            Send(Header(kStart));
        }
        break;
    case kStart:
        if (!g_host)
            g_started = true;
        break;
    case kInput:
        if (len >= 9)
        {
            g_started = true;  // a guest that missed START still starts when inputs come
            uint32_t wants = Get32(p), first = Get32(p + 4);
            uint32_t count = p[8];
            g_remoteWants = std::max(g_remoteWants, wants);
            for (uint32_t k = 0; k < count && 9 + (k + 1) * kInputBytes <= uint32_t(len); k++)
            {
                uint32_t f = first + k;
                if (f >= g_remote.size())
                {
                    g_remote.resize(f + 1);
                    g_haveRemote.resize(f + 1, false);
                }
                if (!g_haveRemote[f])
                {
                    g_remote[f] = UnpackInput(p + 9 + k * kInputBytes);
                    g_haveRemote[f] = true;
                }
            }
            while (g_contiguous < g_haveRemote.size() && g_haveRemote[g_contiguous])
                g_contiguous++;
            g_cv.notify_all();
        }
        break;
    case kHash:
        if (len >= 12)
        {
            uint32_t f = Get32(p);
            g_remoteHash[f] = Get64(p + 4);
            auto it = g_localHash.find(f);
            if (it != g_localHash.end() && it->second != g_remoteHash[f] && !g_desync)
            {
                g_desync = true;
                g_desyncFrame = f;
                LOG("[online] DESYNC at frame %u: the two games are no longer the same", f);
            }
        }
        break;
    case kBye:
        LOG("[online] the other player left");
        g_lost = true;
        g_cv.notify_all();
        break;
    }
}

void RecvThread()
{
    std::vector<uint8_t> buf(2048);
    while (g_running)
    {
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = recvfrom(g_sock, (char*)buf.data(), int(buf.size()), 0, (sockaddr*)&from, &fl);
        if (n > 0)
            HandlePacket(buf.data(), n, from);
        else if (n == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT && WSAGetLastError() != WSAECONNRESET)
            Sleep(5);
    }
}

// --- Saves ---------------------------------------------------------------------------------
fs::path ExeDir()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// user/nand packed as: u16 name length, name (utf-8, '/' separators), u32 size, data.
std::vector<uint8_t> PackSaves()
{
    std::vector<uint8_t> b;
    fs::path root = ExeDir() / "user" / "nand";
    std::error_code ec;
    if (!fs::exists(root, ec))
        return b;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec || !it->is_regular_file())
            continue;
        auto u8 = fs::relative(it->path(), root, ec).generic_u8string();
        std::string rel(u8.begin(), u8.end());
        std::ifstream f(it->path(), std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Put16(b, uint16_t(rel.size()));
        b.insert(b.end(), rel.begin(), rel.end());
        Put32(b, uint32_t(data.size()));
        b.insert(b.end(), data.begin(), data.end());
    }
    return b;
}

bool UnpackSaves(const std::vector<uint8_t>& b)
{
    fs::path root = ExeDir() / "user" / "nand_online";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    size_t o = 0;
    while (o + 2 <= b.size())
    {
        uint16_t nl = Get16(&b[o]);
        o += 2;
        if (o + nl + 4 > b.size())
            return false;
        std::string rel(b.begin() + o, b.begin() + o + nl);
        o += nl;
        uint32_t sz = Get32(&b[o]);
        o += 4;
        if (o + sz > b.size() || rel.find("..") != std::string::npos)
            return false;
        fs::path p = root / fs::u8path(rel);
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f.write((const char*)&b[o], sz);
        o += sz;
    }
    return true;
}

uint64_t BuildHash()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::ifstream f(exe, std::ios::binary);
    uint64_t h = 0xCBF29CE484222325ull;
    std::vector<char> buf(1 << 20);
    while (f)
    {
        f.read(buf.data(), std::streamsize(buf.size()));
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; i++)
            h = (h ^ uint8_t(buf[size_t(i)])) * 0x100000001B3ull;
    }
    return h;
}

// --- Addresses -----------------------------------------------------------------------------
std::string IpStr(uint32_t ip)
{
    char b[20];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}
uint32_t LocalIp()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    uint32_t ip = 0x7F000001;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    if (s != INVALID_SOCKET && connect(s, (sockaddr*)&a, sizeof(a)) == 0)
    {
        sockaddr_in me{};
        int l = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &l) == 0)
            ip = ntohl(me.sin_addr.s_addr);
    }
    if (s != INVALID_SOCKET)
        closesocket(s);
    return ip;
}

// UPnP (adapted from the Guitar Hero port): opens the UDP port on the router for 2 hours.
std::string g_upnpControl, g_upnpService;
std::atomic<uint32_t> g_publicIp{ 0 };
std::atomic<int> g_upnpState{ 0 };  // 0 trying, 1 open, 2 no UPnP

bool HttpReq(const std::wstring& url, const wchar_t* verb, const std::wstring& headers, const std::string& body, std::string& out,
    DWORD& status)
{
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        return false;
    HINTERNET ses = WinHttpOpen(L"DSE", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        return false;
    WinHttpSetTimeouts(ses, 2000, 2000, 3000, 3000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, verb, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    bool ok = req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1),
                         body.empty() ? nullptr : (LPVOID)body.data(), DWORD(body.size()), DWORD(body.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr);
    if (ok)
    {
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        char b[4096];
        DWORD n = 0;
        while (WinHttpReadData(req, b, sizeof(b), &n) && n)
            out.append(b, n);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}
std::string XmlTag(const std::string& x, const std::string& tag, size_t from = 0)
{
    size_t a = x.find("<" + tag + ">", from);
    if (a == std::string::npos)
        return "";
    a += tag.size() + 2;
    size_t b = x.find("</" + tag + ">", a);
    return b == std::string::npos ? "" : x.substr(a, b - a);
}
std::wstring WideAscii(const std::string& s) { return std::wstring(s.begin(), s.end()); }
bool UpnpSoap(const char* action, const std::string& args, std::string& out)
{
    std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
        " xmlns:u=\"" + g_upnpService + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    std::wstring hdr = L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + WideAscii(g_upnpService) + L"#" +
        WideAscii(action) + L"\"\r\n";
    DWORD st = 0;
    return HttpReq(WideAscii(g_upnpControl), L"POST", hdr, body, out, st) && st == 200;
}
std::string MapArgs(int lease)
{
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(g_port) +
        "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(g_port) +
        "</NewInternalPort><NewInternalClient>" + IpStr(LocalIp()) +
        "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Dead Space Extraction PC</NewPortMappingDescription>"
        "<NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
}
void UpnpThread()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    std::vector<std::string> locs;
    for (const char* st : { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1" })
    {
        std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, m.data(), int(m.size()), 0, (sockaddr*)&to, sizeof(to));
    }
    uint64_t end = NowMs() + 2500;
    char buf[2048];
    while (NowMs() < end)
    {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        timeval tv{ 0, 200000 };
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0)
            continue;
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            continue;
        buf[n] = 0;
        std::string r = buf, low = r;
        for (auto& c : low)
            c = char(tolower((unsigned char)c));
        size_t k = low.find("\nlocation:");
        if (k == std::string::npos)
            continue;
        size_t a = k + 10;
        while (a < r.size() && r[a] == ' ')
            a++;
        std::string loc = r.substr(a, r.find_first_of("\r\n", a) - a);
        if (std::find(locs.begin(), locs.end(), loc) == locs.end())
            locs.push_back(loc);
    }
    closesocket(s);
    for (auto& loc : locs)
    {
        std::string desc;
        DWORD st = 0;
        if (!HttpReq(WideAscii(loc), L"GET", L"", "", desc, st) || st != 200)
            continue;
        for (const char* svc : { "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                 "urn:schemas-upnp-org:service:WANPPPConnection:1" })
        {
            size_t k = desc.find(svc);
            if (k == std::string::npos)
                continue;
            std::string ctl = XmlTag(desc, "controlURL", k);
            if (ctl.empty())
                continue;
            if (ctl.find("://") == std::string::npos)
            {
                std::string base = XmlTag(desc, "URLBase");
                if (base.empty())
                    base = loc.substr(0, loc.find('/', loc.find("://") + 3));
                while (!base.empty() && base.back() == '/')
                    base.pop_back();
                ctl = base + (ctl[0] == '/' ? "" : "/") + ctl;
            }
            g_upnpControl = ctl;
            g_upnpService = svc;
            std::string out;
            if (!UpnpSoap("AddPortMapping", MapArgs(7200), out))
            {
                out.clear();
                if (!UpnpSoap("AddPortMapping", MapArgs(0), out))
                    continue;
            }
            out.clear();
            if (UpnpSoap("GetExternalIPAddress", "", out))
            {
                in_addr ia{};
                if (inet_pton(AF_INET, XmlTag(out, "NewExternalIPAddress").c_str(), &ia) == 1)
                    g_publicIp = ntohl(ia.s_addr);
            }
            g_upnpState = 1;
            LOG("[online] UPnP: UDP port %u open on the router (public IP %s)", g_port, IpStr(g_publicIp).c_str());
            return;
        }
    }
    g_upnpState = 2;
    LOG("[online] UPnP: no router answered; the host needs the UDP port %u forwarded (or a VPN like Radmin/ZeroTier)", g_port);
}

// --- A small window while connecting --------------------------------------------------------
std::function<std::wstring()> g_waitText;
LRESULT CALLBACK WaitProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_TIMER:
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(5, 9, 12));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(215, 238, 248));
        HFONT f = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Bahnschrift");
        HGDIOBJ of = SelectObject(dc, f);
        RECT tr{ 24, 20, rc.right - 24, rc.bottom - 20 };
        std::wstring t = g_waitText ? g_waitText() : L"";
        DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Shows `text` until done() or the window is closed (false: cancelled).
bool WaitWindow(std::function<std::wstring()> text, std::function<bool()> done, std::function<void()> tick)
{
    g_waitText = text;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WaitProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"DSEOnlineWait";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(wc.lpszClassName, L"Dead Space: Extraction · En línea", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 260, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    SetTimer(h, 1, 100, nullptr);
    MSG msg;
    while (IsWindow(h))
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done())
        {
            DestroyWindow(h);
            return true;
        }
        tick();
        Sleep(15);
    }
    return false;
}
} // namespace

bool NetplayStart()
{
    std::string mode = ConfigString("online", "no");
    if (mode != "crear" && mode != "unirse")
        return false;
    g_host = mode == "crear";
    g_port = uint16_t(std::clamp(ConfigInt("online_puerto", 7350), 1024, 65535));
    g_delay = std::clamp(ConfigInt("online_retraso", 3), 1, 10);
    g_build = BuildHash();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(g_host ? g_port : 0);
    if (g_sock == INVALID_SOCKET || bind(g_sock, (sockaddr*)&me, sizeof(me)) != 0)
    {
        MessageBoxW(nullptr, (L"No se pudo abrir el puerto UDP " + std::to_wstring(g_port) + L". ¿Otra copia del juego lo usa?").c_str(),
            L"En línea", MB_ICONERROR);
        return false;
    }
    DWORD timeout = 200;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    g_running = true;
    std::thread(RecvThread).detach();

    if (g_host)
    {
        g_wide = ConfigString("aspecto", "4:3") != "4:3";
        g_save = PackSaves();
        if (ConfigBool("online_upnp", true))
            std::thread(UpnpThread).detach();
        else
            g_upnpState = 2;
        std::string lan = IpStr(LocalIp()) + ":" + std::to_string(g_port);
        bool ok = WaitWindow(
            [&] {
                std::lock_guard lock(g_m);
                std::wstring t = L"CREAR PARTIDA · esperando al jugador 2\n\n";
                t += L"En la misma red:  " + WideAscii(lan) + L"\n";
                if (g_upnpState == 0)
                    t += L"Internet:  abriendo el puerto en el router…\n";
                else if (g_upnpState == 1 && g_publicIp)
                    t += L"Internet:  " + WideAscii(IpStr(g_publicIp) + ":" + std::to_string(g_port)) + L"\n";
                else
                    t += L"Internet:  el router no abre puertos solo (UPnP). Redirige el puerto UDP " + std::to_wstring(g_port) +
                        L" o usad una VPN (Radmin VPN, ZeroTier).\n";
                if (g_buildMismatch)
                    t += L"\nEl otro jugador tiene otra versión de DSE.exe: debéis usar la misma.";
                else if (g_havePeer)
                    t += L"\nJugador 2 conectado: enviando la partida guardada…";
                t += L"\n\nCierra esta ventana para jugar sin conexión.";
                return t;
            },
            [&] {
                std::lock_guard lock(g_m);
                return g_ready && !g_buildMismatch;
            },
            [] {});
        if (!ok)
        {
            g_running = false;
            return false;
        }
    }
    else
    {
        std::string addr = ConfigString("online_direccion", "");
        std::string host = addr, port = "7350";
        if (size_t c = addr.rfind(':'); c != std::string::npos)
        {
            host = addr.substr(0, c);
            port = addr.substr(c + 1);
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (host.empty() || getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        {
            MessageBoxW(nullptr, (L"No se encuentra la dirección «" + WideAscii(addr) + L"».").c_str(), L"En línea", MB_ICONERROR);
            return false;
        }
        {
            std::lock_guard lock(g_m);
            memcpy(&g_peer, res->ai_addr, sizeof(g_peer));
            g_havePeer = true;
        }
        freeaddrinfo(res);
        uint64_t lastHello = 0, lastReq = 0;
        bool ok = WaitWindow(
            [&] {
                std::lock_guard lock(g_m);
                std::wstring t = L"UNIRSE · " + WideAscii(addr) + L"\n\n";
                if (!g_welcomed)
                    t += L"Buscando la partida… (comprueba la dirección y que el anfitrión la haya creado)";
                else if (g_buildMismatch)
                    t += L"El anfitrión tiene otra versión de DSE.exe: debéis usar la misma.";
                else
                {
                    size_t have = std::count(g_haveChunk.begin(), g_haveChunk.end(), true);
                    t += L"Conectado. Recibiendo su partida guardada: " + std::to_wstring(have) + L" / " +
                        std::to_wstring(g_haveChunk.size()) + L" bloques…";
                }
                t += L"\n\nCierra esta ventana para jugar sin conexión.";
                return t;
            },
            [&] {
                std::lock_guard lock(g_m);
                return g_started;
            },
            [&] {
                std::lock_guard lock(g_m);
                uint64_t now = NowMs();
                if (!g_welcomed)
                {
                    if (now - lastHello > 250)
                    {
                        lastHello = now;
                        std::vector<uint8_t> b = Header(kHello);
                        Put64(b, g_build);
                        Send(b);
                    }
                    return;
                }
                if (g_buildMismatch)
                    return;
                bool all = std::all_of(g_haveChunk.begin(), g_haveChunk.end(), [](bool v) { return v; });
                if (!all && now - lastReq > 30)
                {
                    lastReq = now;
                    int asked = 0;
                    for (uint32_t i = 0; i < g_haveChunk.size() && asked < 32; i++)
                        if (!g_haveChunk[i])
                        {
                            std::vector<uint8_t> b = Header(kGetChunk);
                            Put32(b, i);
                            Send(b);
                            asked++;
                        }
                }
                if (all && !g_ready)
                {
                    g_ready = UnpackSaves(g_save);
                    LOG("[online] host's saves received (%zu bytes)", g_save.size());
                }
                if (g_ready && now - lastHello > 100)
                {
                    lastHello = now;
                    Send(Header(kReady));
                }
            });
        if (!ok)
        {
            g_running = false;
            return false;
        }
    }
    g_active = true;
    LOG("[online] started as %s, input delay %d frames, wide %d", g_host ? "host (player 1)" : "guest (player 2)", g_delay, g_wide);
    return true;
}

bool NetplayActive() { return g_active; }
bool NetplayIsHost() { return g_host; }
int NetplayLocalPlayer() { return g_host ? 0 : 1; }
bool NetplayHostWide() { return g_wide; }

void NetplayExchange(uint32_t frame, const NetInput& local, NetInput out[2])
{
    std::unique_lock lock(g_m);
    uint32_t target = frame + uint32_t(g_delay);
    // Our controls for frame + delay (the frames before the first latch stay empty).
    if (g_local.size() <= target)
        g_local.resize(target + 1);
    g_local[target] = local;
    SendInputsLocked();
    int me = g_host ? 0 : 1;
    out[me] = frame < g_local.size() ? g_local[frame] : NetInput{};
    out[1 - me] = NetInput{};
    if (frame < uint32_t(g_delay) || !g_running)
        return;
    uint64_t start = NowMs(), lastSend = start, lastTitle = 0;
    while (g_running && !g_lost && (frame >= g_haveRemote.size() || !g_haveRemote[frame]))
    {
        g_cv.wait_for(lock, std::chrono::milliseconds(8));
        uint64_t now = NowMs();
        if (now - lastSend >= 16)
        {
            lastSend = now;
            SendInputsLocked();  // lost packets: the other PC asks again with its ack
        }
        // After a second, the title bar says what is going on.
        if (now - start > 1000 && now - lastTitle > 500)
        {
            lastTitle = now;
            std::wstring t = L"Dead Space: Extraction \u00b7 esperando al otro jugador\u2026 (" +
                std::to_wstring((now - start) / 1000) + L" s)";
            lock.unlock();
            if (HWND w = (HWND)VideoGetWindow())
                SetWindowTextW(w, t.c_str());
            lock.lock();
        }
        if (now - start > 15000)
        {
            LOG("[online] no controls from the other player for 15 s");
            g_lost = true;
        }
    }
    if (g_lost)
    {
        // The session is over: say so and go back to the launcher (a new process).
        lock.unlock();
        LOG("[online] connection lost: back to the launcher");
        NetplayShutdown();
        MessageBoxW((HWND)VideoGetWindow(), L"Se perdi\u00f3 la conexi\u00f3n con el otro jugador.\n\nSe vuelve al launcher.",
            L"Dead Space: Extraction \u00b7 En l\u00ednea", MB_ICONINFORMATION | MB_TOPMOST);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --launcher";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (frame < g_haveRemote.size() && g_haveRemote[frame])
        out[1 - me] = g_remote[frame];
    uint64_t waited = NowMs() - start;
    if (waited > 0)
        g_rttMs = uint32_t(waited);
}

void NetplayReportHash(uint32_t frame, uint64_t hash)
{
    std::lock_guard lock(g_m);
    g_localHash[frame] = hash;
    auto it = g_remoteHash.find(frame);
    if (it != g_remoteHash.end() && it->second != hash && !g_desync)
    {
        g_desync = true;
        g_desyncFrame = frame;
        LOG("[online] DESYNC at frame %u: the two games are no longer the same", frame);
    }
    std::vector<uint8_t> b = Header(kHash);
    Put32(b, frame);
    Put64(b, hash);
    Send(b);
}

void NetplayShutdown()
{
    if (!g_active || !g_havePeer)
        return;
    std::lock_guard lock(g_m);
    for (int i = 0; i < 3; i++)  // UDP: a few times so one gets through
        Send(Header(kBye));
}

std::wstring NetplayStatusText()
{
    if (!g_active)
        return L"";
    std::lock_guard lock(g_m);
    std::wstring t = g_host ? L"En línea · anfitrión (J1)" : L"En línea · invitado (J2)";
    if (g_lost)
        t += L" · conexión perdida";
    if (g_desync)
        t += L" · ¡DESINCRONIZADO! (fotograma " + std::to_wstring(g_desyncFrame) + L")";
    return t;
}
