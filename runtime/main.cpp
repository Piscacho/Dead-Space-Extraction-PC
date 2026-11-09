// Entry point: reserve guest memory, load main.dol, register the recompiled functions
// and jump to the game's __start.
#include "runtime.h"
#include "config.h"
bool LauncherRun();  // launcher.cpp
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <unordered_map>
#include "audio.h"
#include "disc.h"
#include "gx.h"
#include "gx_state.h"
#include <ppc_recomp_shared.h>
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

uint8_t* g_base = nullptr;
extern PPCFuncMapping PPCFuncMappings[];

// ---------------------------------------------------------------------------------------
// Guest memory

// Reserves the 4 GB window. MEM1 is visible at 0x80000000 and 0xC0000000, and MEM2 at
// 0x90000000 and 0xD0000000 (the second address of each is the uncached mirror), using
// views of shared sections over placeholders. Everything else is plain committed memory,
// including the hardware register pages for now.
static uint8_t* ReserveGuestMemory()
{
    using VA2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MV3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto va2 = kb ? (VA2)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
    auto mv3 = kb ? (MV3)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
    if (!va2 || !mv3)
    {
        LOG("[mem] VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803+ needed)");
        return nullptr;
    }

    constexpr uint64_t kMem1View = 0x02000000;  // 32 MB views (MEM1 is 24 MB)
    constexpr uint64_t kMem2View = 0x04000000;
    struct Piece { uint64_t addr, size; int section; };  // section: -1 private, 0 MEM1, 1 MEM2
    const Piece pieces[] = {
        { 0x00000000, 0x80000000, -1 },
        { 0x80000000, kMem1View, 0 },
        { 0x82000000, 0x0E000000, -1 },
        { 0x90000000, kMem2View, 1 },
        { 0x94000000, 0x2C000000, -1 },
        { 0xC0000000, kMem1View, 0 },
        { 0xC2000000, 0x0E000000, -1 },
        { 0xD0000000, kMem2View, 1 },
        { 0xD4000000, 0x2C000000, -1 },
    };

    HANDLE proc = GetCurrentProcess();
    auto* b = (uint8_t*)va2(proc, nullptr, PPC_MEMORY_SIZE, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!b)
    {
        LOG("[mem] cannot reserve 4 GB (error %lu)", GetLastError());
        return nullptr;
    }
    // Split the placeholder into the pieces (the last one is what remains).
    for (size_t i = 0; i + 1 < std::size(pieces); i++)
    {
        if (!VirtualFree(b + pieces[i].addr, pieces[i].size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        {
            LOG("[mem] split at 0x%llX failed (error %lu)", (unsigned long long)pieces[i].addr, GetLastError());
            return nullptr;
        }
    }
    HANDLE sections[2] = {
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, (DWORD)kMem1View, nullptr),
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, (DWORD)kMem2View, nullptr),
    };
    if (!sections[0] || !sections[1])
    {
        LOG("[mem] cannot create the MEM1/MEM2 sections (error %lu)", GetLastError());
        return nullptr;
    }
    for (const Piece& p : pieces)
    {
        void* r = p.section < 0
            ? va2(proc, b + p.addr, p.size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)
            : mv3(sections[p.section], proc, b + p.addr, 0, p.size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (!r)
        {
            LOG("[mem] mapping 0x%llX failed (error %lu)", (unsigned long long)p.addr, GetLastError());
            return nullptr;
        }
    }
    volatile uint8_t* vb = b;
    vb[0x80000100] = 0x5A;
    vb[0x90000100] = 0xA5;
    bool ok = vb[0xC0000100] == 0x5A && vb[0xD0000100] == 0xA5;
    vb[0x80000100] = vb[0x90000100] = 0;
    LOG("[mem] guest memory at %p: MEM1 0x80000000/0xC0000000, MEM2 0x90000000/0xD0000000 (mirrors %s)",
        (void*)b, ok ? "ok" : "BROKEN");
    return ok ? b : nullptr;
}

// ---------------------------------------------------------------------------------------
// Recompiled functions

static std::vector<PPCFunc*> g_funcTable;                          // indexed by (guest - code base) / 4
static std::vector<std::pair<uintptr_t, uint32_t>> g_hostToGuest;  // sorted (host address, guest function)

static void RegisterFunctions()
{
    g_funcTable.assign(PPC_CODE_SIZE / 4 + 1, nullptr);
    for (auto* m = PPCFuncMappings; m->host != nullptr; ++m)
    {
        if (m->guest >= PPC_CODE_BASE && m->guest < PPC_CODE_BASE + PPC_CODE_SIZE)
            g_funcTable[(m->guest - PPC_CODE_BASE) / 4] = m->host;
        g_hostToGuest.emplace_back((uintptr_t)m->host, (uint32_t)m->guest);
    }
    std::sort(g_hostToGuest.begin(), g_hostToGuest.end());
    LOG("[runtime] %zu recompiled functions", g_hostToGuest.size());
}

PPCFunc* FindGuestFunction(uint32_t guest)
{
    if (guest < PPC_CODE_BASE || guest >= PPC_CODE_BASE + PPC_CODE_SIZE || (guest & 3))
        return nullptr;
    return g_funcTable[(guest - PPC_CODE_BASE) / 4];
}

uint32_t GuestFunctionAtHost(uintptr_t host)
{
    // The table holds the small weak wrappers (sub_X), which the compiler emits right
    // after each body (__imp__sub_X). An address inside a body therefore belongs to the
    // first wrapper above it, not the one below.
    auto it = std::upper_bound(g_hostToGuest.begin(), g_hostToGuest.end(), std::make_pair(host, 0xFFFFFFFFu));
    return it == g_hostToGuest.end() ? 0 : it->second;
}

void GuestCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    if (PPCFunc* fn = FindGuestFunction(target))
    {
        fn(ctx, base);
        return;
    }
    static int reported = 0;
    if (reported++ < 32)
        LOG("[call] indirect call to 0x%08X has no recompiled function (lr 0x%08X)", target, (uint32_t)ctx.lr);
}

// ---------------------------------------------------------------------------------------
// Helpers emitted by the recompiler (tools/ppc_indirect.h)

void ppc_trap(uint32_t address)
{
    static int reported = 0;
    if (reported++ < 32)
        LOG("[trap] tw/twi at 0x%08X", address);
}

// On the Wii, `sc` is the OS's system call that runs sync/isync with the right privilege
// (DCFlushRange and friends end with it). Nothing to do on the PC.
void ppc_syscall(PPCContext&, uint8_t*, uint32_t) {}

// Single guest thread for now, so the reservation is a plain compare-and-swap.
static uint32_t g_reserveEa = 0xFFFFFFFF;
void ppc_reserve(uint32_t ea) { g_reserveEa = ea; }
bool ppc_stwcx32(uint8_t* base, uint32_t ea, uint32_t expectedRaw, uint32_t newRaw)
{
    if (g_reserveEa != ea)
        return false;
    g_reserveEa = 0xFFFFFFFF;
    return __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ea), expectedRaw, newRaw);
}

// Wii time base: bus clock / 4 = 60.75 MHz.
uint64_t ppc_mftb()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    constexpr uint64_t tb = 243000000ull / 4;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t t = (uint64_t)c.QuadPart, f = (uint64_t)freq.QuadPart;
    return (t / f) * tb + (t % f) * tb / f;
}

// ---------------------------------------------------------------------------------------
// Diagnostics

static std::string HostSymbol(uintptr_t addr);

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* e)
{
    auto* r = e->ExceptionRecord;
    if (r->ExceptionCode == 0x406D1388 || r->ExceptionCode == 0x40010006 || r->ExceptionCode == 0x4001000A)
        return EXCEPTION_CONTINUE_SEARCH;  // thread names / OutputDebugString
    uintptr_t mod = (uintptr_t)GetModuleHandleW(nullptr);
    uintptr_t access = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    LOG("[crash] exception 0x%08lX at exe+0x%llX in %s", r->ExceptionCode,
        (unsigned long long)((uintptr_t)r->ExceptionAddress - mod), HostSymbol((uintptr_t)r->ExceptionAddress).c_str());
    if (r->NumberParameters > 1)
    {
        static const char* kinds[] = { "read", "write", "?", "?", "?", "?", "?", "?", "execute" };
        LOG("[crash] %s of host 0x%llX = guest 0x%llX", kinds[r->ExceptionInformation[0] & 15],
            (unsigned long long)access, (unsigned long long)(access - (uintptr_t)g_base));
    }
    const CONTEXT& rc = *e->ContextRecord;
    LOG("[crash] rip 0x%llX (exe+0x%llX) rax %llX rcx %llX rdx %llX rsi %llX rdi %llX rsp %llX",
        (unsigned long long)rc.Rip, (unsigned long long)(rc.Rip - mod), (unsigned long long)rc.Rax,
        (unsigned long long)rc.Rcx, (unsigned long long)rc.Rdx, (unsigned long long)rc.Rsi,
        (unsigned long long)rc.Rdi, (unsigned long long)rc.Rsp);
    LOG("[crash] guest base %p, image base 0x%llX", (void*)g_base, (unsigned long long)mod);
    // Host stack by function name (guest functions appear as sub_XXXXXXXX).
    CONTEXT c = *e->ContextRecord;
    std::string line = "[crash] stack:";
    for (int f = 0; f < 16 && c.Rip; f++)
    {
        line += " " + HostSymbol(c.Rip);
        DWORD64 imageBase;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!fe)
        {
            c.Rip = *(DWORD64*)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        void* hd;
        DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &hd, &est, nullptr);
    }
    LOG("%s", line.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// Host function name from the executable's debug symbols (DSE.pdb), e.g.
// "__imp__sub_803BD9CC" or "sub_803E4C18" (C++ names are shown undecorated).
static std::string HostSymbol(uintptr_t addr)
{
    static bool init = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    if (!init)
        return "?";
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), addr, &disp, s))
        return "?";
    return s->Name;
}

// DSE_SAMPLE=1: sampling profiler. Every millisecond the main thread (all guest threads
// are fibers on it) is stopped and its instruction pointer recorded; every 10 seconds the
// functions it was in most often are printed with their share of the samples.
static HANDLE g_mainThread;
static DWORD WINAPI Sampler(LPVOID)
{
    timeBeginPeriod(1);  // 1 ms sleeps
    std::vector<DWORD64> rips;
    ULONGLONG last = GetTickCount64();
    for (;;)
    {
        Sleep(1);
        if (SuspendThread(g_mainThread) == (DWORD)-1)
            return 0;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(g_mainThread, &c);
        ResumeThread(g_mainThread);
        rips.push_back(c.Rip);
        if (GetTickCount64() - last < 10000)
            continue;
        last = GetTickCount64();
        std::unordered_map<std::string, uint32_t> byName;
        std::unordered_map<DWORD64, std::string> cache;
        for (DWORD64 r : rips)
        {
            auto it = cache.find(r);
            if (it == cache.end())
                it = cache.emplace(r, HostSymbol(r)).first;
            byName[it->second]++;
        }
        std::vector<std::pair<uint32_t, std::string>> top;
        for (auto& [n, k] : byName)
            top.push_back({ k, n });
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string line = "[sample] " + std::to_string(rips.size()) + " samples:";
        for (size_t i = 0; i < top.size() && i < 18; i++)
        {
            char buf[160];
            snprintf(buf, sizeof(buf), "\n    %5.1f%% %s", 100.0 * top[i].first / rips.size(), top[i].second.c_str());
            line += buf;
        }
        LOG("%s", line.c_str());
        rips.clear();
    }
}

// Every 2 seconds, print the main thread's call stack by function name. Shows where the
// boot stops when the game waits forever for hardware that is not emulated yet.
static DWORD WINAPI Watchdog(LPVOID)
{
    std::string last;
    int same = 0;
    for (;;)
    {
        Sleep(2000);
        if (SuspendThread(g_mainThread) == (DWORD)-1)
            return 0;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        GetThreadContext(g_mainThread, &c);
        std::string stack;
        for (int f = 0; f < 10 && c.Rip; f++)
        {
            std::string name = HostSymbol(c.Rip);
            if (name.rfind("__imp__", 0) == 0)
                name.erase(0, 7);
            if (!stack.empty())
                stack += " <- ";
            stack += name;
            DWORD64 imageBase;
            auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fe)
            {
                c.Rip = *(DWORD64*)c.Rsp;
                c.Rsp += 8;
                continue;
            }
            void* hd;
            DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &hd, &est, nullptr);
        }
        ResumeThread(g_mainThread);
        same = stack == last ? same + 1 : 0;
        last = stack;
        LOG("[watchdog]%s %s", same >= 2 ? " (stuck?)" : "", stack.c_str());

        // DSE_THREADS=1: list the guest threads (OSThread: state u16 +0x2C8, suspend +0x2CC,
        // priority +0x2D0, waiting queue +0x2DC, next active thread +0x2FC; the active list
        // starts at 0x800000DC). States: 1 ready, 2 running, 4 waiting, 8 moribund.
        if (getenv("DSE_THREADS"))
        {
            std::string line = "[threads] ready bits " + std::to_string(Load32(0x80605528)) + ":";
            uint32_t t = Load32(0x800000DC);
            for (int n = 0; t && n < 32; n++, t = Load32(t + 0x2FC))
            {
                char buf[96];
                snprintf(buf, sizeof(buf), " %08X(st %u, prio %u, susp %d, q %08X)", t, Load16(t + 0x2C8),
                    Load32(t + 0x2D0), int32_t(Load32(t + 0x2CC)), Load32(t + 0x2DC));
                line += buf;
            }
            LOG("%s", line.c_str());
        }

        // DSE_PEEK=0xADDR,0xADDR,...: print those guest words too.
        if (const char* peek = getenv("DSE_PEEK"))
        {
            std::string line = "[peek]";
            for (const char* p = peek; *p;)
            {
                uint32_t a = (uint32_t)strtoul(p, (char**)&p, 0);
                char buf[32];
                snprintf(buf, sizeof(buf), " %08X=%08X", a, Load32(a));
                line += buf;
                while (*p == ',' || *p == ' ')
                    p++;
            }
            LOG("%s", line.c_str());
        }
    }
}

// ---------------------------------------------------------------------------------------

// DSE_WATCH=0xADDR,bytes (investigation): which functions touch that guest memory. Its host
// page is made inaccessible; each access faults, the function (and its caller) is logged
// once, the page opens again, and the pump closes it again on the next retrace.
static uintptr_t g_watchPage = 0, g_watchFrom = 0, g_watchTo = 0;
static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e)
{
    if (!g_watchPage)
        return EXCEPTION_CONTINUE_SEARCH;
    static thread_local bool stepping = false;
    DWORD old;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && stepping)
    {
        // The access went through: close the page again.
        stepping = false;
        VirtualProtect((void*)g_watchPage, 0x1000, PAGE_NOACCESS, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = e->ExceptionRecord->ExceptionInformation[1];
    if ((addr & ~uintptr_t(0xFFF)) != g_watchPage)
        return EXCEPTION_CONTINUE_SEARCH;
    // Open the page for this one instruction (trap flag), then close it again.
    VirtualProtect((void*)g_watchPage, 0x1000, PAGE_READWRITE, &old);
    e->ContextRecord->EFlags |= 0x100;
    stepping = true;
    if (addr >= g_watchFrom && addr < g_watchTo)
    {
        static std::unordered_map<DWORD64, bool> seen;
        DWORD64 rip = e->ContextRecord->Rip;
        if (!seen[rip] && seen.size() < 200)
        {
            seen[rip] = true;
            CONTEXT c = *e->ContextRecord;
            std::string chain = HostSymbol(c.Rip);
            DWORD64 imageBase;
            if (auto* fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr))
            {
                void* hd;
                DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &hd, &est, nullptr);
                chain += " <- " + HostSymbol(c.Rip);
            }
            LOG("[watch] %s guest 0x%08X by %s", e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                uint32_t(addr - uintptr_t(g_base)), chain.c_str());
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool g_watchArmed = false;
void WatchRearm()
{
    if (g_watchPage && !g_watchArmed)
    {
        DWORD old;
        static bool reported = false;
        if (!VirtualProtect((void*)g_watchPage, 0x1000, PAGE_NOACCESS, &old) && !reported)
        {
            reported = true;
            LOG("[watch] VirtualProtect failed (%lu)", GetLastError());
        }
        else if (!reported)
        {
            reported = true;
            LOG("[watch] armed on host page %p", (void*)g_watchPage);
        }
        g_watchArmed = true;
    }
}

int main(int argc, char** argv)
{
    // DSE.exe has no console. When nobody redirects its log (tools/run.ps1 does), it goes to
    // logs/juego.log, two folders above the exe (project/build -> Recomp/logs).
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == nullptr || err == INVALID_HANDLE_VALUE || GetFileType(err) == FILE_TYPE_UNKNOWN)
    {
        std::string logs = ExeFolder() + "/../../logs";
        CreateDirectoryA(logs.c_str(), nullptr);
        if (!freopen((logs + "/juego.log").c_str(), "w", stderr))
            freopen("NUL", "w", stderr);
    }
    AddVectoredExceptionHandler(1, CrashHandler);

    // The launcher first (launcher.cpp), unless --jugar, an automated test (DSE_AUTOSCRIPT /
    // DSE_AUTOPRESS) or mostrar_launcher=0; --launcher or holding Shift always shows it.
    bool play = false, forceLauncher = false;
    std::string dolArg;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--jugar")
            play = true;
        else if (a == "--launcher")
            forceLauncher = true;
        else
            dolArg = a;
    }
    bool automated = getenv("DSE_AUTOSCRIPT") || getenv("DSE_AUTOPRESS");
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (forceLauncher || shift || (!play && !automated && ConfigBool("mostrar_launcher", true)))
    {
        if (!LauncherRun())
            return 0;
        ConfigLoad();  // what the launcher saved
    }
    std::string folder = ConfigString("carpeta_juego", "G:/Dead Space Extraction PC/juego/DATA");
    std::string dolDefault = folder + "/sys/main.dol";
    const char* dolPath = !dolArg.empty() ? dolArg.c_str() : dolDefault.c_str();

    g_base = ReserveGuestMemory();
    if (const char* w = getenv("DSE_WATCH"))
    {
        char* end = nullptr;
        uint32_t guest = uint32_t(strtoul(w, &end, 0));
        uint32_t bytes = end && *end == ',' ? uint32_t(strtoul(end + 1, nullptr, 0)) : 4;
        g_watchFrom = uintptr_t(g_base) + guest;
        g_watchTo = g_watchFrom + bytes;
        g_watchPage = g_watchFrom & ~uintptr_t(0xFFF);
        AddVectoredExceptionHandler(1, WatchHandler);  // ahead of the crash handler
    }
    if (!g_base)
        return 1;
    uint32_t entry = 0;
    if (!LoadDol(dolPath, entry))
        return 1;
    // The data partition sits next to sys/main.dol: <root>/sys/main.dol.
    std::string partition = dolPath;
    partition = partition.substr(0, partition.find_last_of("\\/"));
    partition = partition.substr(0, partition.find_last_of("\\/"));
    if (!DiscInit(partition))
        return 1;
    InitLowMemory();
    RegisterFunctions();

    PPCFunc* start = FindGuestFunction(entry);
    if (!start)
    {
        LOG("[runtime] entry point 0x%08X has no recompiled function", entry);
        return 1;
    }

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (!getenv("DSE_NOWATCHDOG"))
        CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (getenv("DSE_SAMPLE"))
        CreateThread(nullptr, 0, Sampler, nullptr, 0, nullptr);

    static PPCContext ctx{};
    InitGuestThreads(&ctx);
    AudioInit();
    if (!getenv("DSE_NOWINDOW"))
    {
        VideoInit();
        if (!GxBackendInit(VideoGetWindow()))
            LOG("[video] D3D12 not available: GDI window showing the XFB in memory");
    }
    // ctx.fpscr keeps a copy of the host MXCSR and writes it back whole when the game
    // sets the rounding mode (mtfsf). Starting from 0 would unmask every floating-point
    // exception (divide by zero, invalid...) on the first mtfsf, so it starts from the
    // host's current value (all exceptions masked).
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    LOG("[runtime] jumping to __start (0x%08X)", entry);
    start(ctx, g_base);
    LOG("[runtime] __start returned (r3 = 0x%X)", ctx.r3.u32);
    return 0;
}
