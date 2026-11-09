// Replacements for Nintendo SDK functions. The recompiled functions are weak: defining
// sub_XXXXXXXX here replaces them, also for direct calls from other game functions.
// The original stays callable as __imp__sub_XXXXXXXX.
//
// Names come from Dolphin's signature database (config/sdk_names.txt) and dtk's symbols.
#include "runtime.h"
#include "config.h"
#include "gx.h"
#include "ios.h"
#include <ppc_recomp_shared.h>
#include <algorithm>
#include <cstring>
#include <string>

#define ORIGINAL(addr) extern "C" PPC_FUNC(__imp__sub_##addr)

// --- EXI (memory cards, SRAM/RTC chip) -----------------------------------------------
// EXISync(chan) waits for the hardware to clear TSTART (bit 0) in the channel's control
// register at 0xCD00680C + chan * 0x14. Hardware registers are plain memory for now, so
// the bit is cleared here: the transfer "finishes" at once and reads return zeros (no
// device). The original then completes the transfer as usual.
ORIGINAL(803BFA94);
PPC_FUNC(sub_803BFA94)  // EXISync
{
    uint32_t cr = 0xCD00680C + ctx.r3.u32 * 0x14;
    Store32(cr, Load32(cr) & ~1u);
    __imp__sub_803BFA94(ctx, base);
}

// --- Audio DSP ------------------------------------------------------------------------
// __OSInitAudioSystem (called by OSInit) uploads a small boot program to the audio DSP
// and polls the DSP mailbox registers at 0xCC005000 for its answers. Audio will be
// replaced at the AX library level, so the DSP boot is skipped.
PPC_FUNC(sub_803B3890)  // __OSInitAudioSystem
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LOG("[dsp] __OSInitAudioSystem skipped (no DSP)");
    }
}

// AIInit calls __AI_SRC_INIT, which times the audio interface by waiting for its sample
// counter (0xCD006C08) to move. There is no audio hardware, so the calibration is skipped.
PPC_FUNC(sub_803D8028)  // __AI_SRC_INIT
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LOG("[ai] __AI_SRC_INIT skipped (no audio interface)");
    }
}

// __DSP_boot_task(task) uploads a DSP program (AX's mixer) and returns; when the DSP has
// booted it, the DSP interrupt runs the task's init callback (DSPTaskInfo +0x28).
// AX's init spins on a flag that callback sets (__AXDSPInitFlag, 0x80605908) without
// re-enabling interrupts, so the callback runs right here, as if the DSP booted at once.
// Mixing itself is not emulated yet: no sound.
PPC_FUNC(sub_803E2060)  // __DSP_boot_task
{
    uint32_t task = ctx.r3.u32;
    uint32_t initCallback = Load32(task + 0x28);
    LOG("[dsp] task 0x%08X booted at once (init callback 0x%08X)", task, initCallback);
    if (initCallback)
        CallGuest(ctx, base, initCallback, task, 0);
}

// --- OSReport: the SDK's and the game's debug messages, printed to the log ---------------
// Minimal printf: %d %i %u %x %X %p %c %s %f (and flags/width, which are kept as given).
// Integer arguments come from r4..r10, floats from f1..f8, as the PPC EABI passes them.
PPC_FUNC(sub_803B4A44)  // OSReport(format, ...)
{
    std::string fmt;
    for (uint32_t a = ctx.r3.u32; Load8(a) && fmt.size() < 512; a++)
        fmt += char(Load8(a));
    const PPCRegister* gprs[] = { &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10 };
    const PPCRegister* fprs[] = { &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4, &ctx.f5, &ctx.f6, &ctx.f7, &ctx.f8 };
    size_t gi = 0, fi = 0;
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++)
    {
        if (fmt[i] != '%')
        {
            out += fmt[i];
            continue;
        }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0123456789.lh", fmt[j]))
            j++;
        if (j >= fmt.size())
            break;
        std::string spec = fmt.substr(i, j - i + 1);
        spec.erase(std::remove(spec.begin(), spec.end(), 'l'), spec.end());
        spec.erase(std::remove(spec.begin(), spec.end(), 'h'), spec.end());
        char conv = fmt[j], buf[600];
        uint32_t arg = gi < 7 ? gprs[gi]->u32 : 0;
        switch (conv)
        {
        case '%': out += '%'; break;
        case 's':
        {
            std::string s;
            for (uint32_t a = arg; a && Load8(a) && s.size() < 256; a++)
                s += char(Load8(a));
            gi++;
            snprintf(buf, sizeof(buf), spec.c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'f': case 'g': case 'e':
            snprintf(buf, sizeof(buf), spec.c_str(), fi < 8 ? fprs[fi]->f64 : 0.0);
            fi++;
            out += buf;
            break;
        case 'd': case 'i': case 'c':
            snprintf(buf, sizeof(buf), spec.c_str(), int32_t(arg));
            gi++;
            out += buf;
            break;
        default:  // u x X p o
            snprintf(buf, sizeof(buf), spec.c_str(), arg);
            gi++;
            out += buf;
            break;
        }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    LOG("[report] %s", out.c_str());
}

// --- Wii Remote (WUD / Bluetooth) ----------------------------------------------------------
// WUDInit starts the Bluetooth stack (BTE), which waits in several loops for the USB
// Bluetooth adapter to come up. There is no adapter: WUDInit only marks itself
// initialized (its flag at 0x80605B30) and succeeds, so no remote is ever connected at
// the WPAD level. Input will be supplied at the KPAD level instead.
PPC_FUNC(sub_80407F8C)  // WUDInit
{
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LOG("[wpad] WUDInit: Bluetooth stack not started (no remotes connected)");
    }
    Store32(0x80605B30, 1);
    ctx.r3.u64 = 1;
}

// --- VI (video interface) ---------------------------------------------------------------
// VISetNextFrameBuffer(xfb): the frame the game wants on screen from the next retrace.
ORIGINAL(803C481C);
PPC_FUNC(sub_803C481C)  // VISetNextFrameBuffer
{
    VideoSetFramebuffer(ctx.r3.u32);
    __imp__sub_803C481C(ctx, base);
}

// --- SC (system configuration, SYSCONF) ----------------------------------------------
// SCInit starts an asynchronous NAND read of /shared2/sys/SYSCONF through IOS, and
// SCCheckStatus waits for it (BgJobStatus = 1 while busy, 3 when the read finished).
// With no IOS behind the IPC yet, the read never completes. Marking it finished makes
// the SDK find an invalid buffer and build an empty, valid config, so the SC getters
// return their defaults.
constexpr uint32_t kScBgJobStatus = 0x806059F0;

ORIGINAL(803E4C18);
PPC_FUNC(sub_803E4C18)  // SCCheckStatus
{
    static bool logged = false;
    static int calls = 0;
    if (calls++ < 3)
        LOG("[sc] SCCheckStatus: BgJobStatus = %u", Load8(kScBgJobStatus));
    if (Load8(kScBgJobStatus) == 1)
    {
        Store8(kScBgJobStatus, 3);
        if (!logged)
        {
            logged = true;
            LOG("[sc] SYSCONF read marked as finished: the SDK builds an empty config");
        }
    }
    __imp__sub_803E4C18(ctx, base);
}

// --- The engine's debug print: fn_80006AA0(level, format, ...) ----------------------------
// Empty in the retail build. Printed through the OSReport hook (arguments shifted by one).
// Off unless DSE_GAMELOG=1: some systems print every frame.
PPC_FUNC(sub_80006AA0)
{
    static bool on = getenv("DSE_GAMELOG") != nullptr;
    if (!on)
        return;
    PPCContext c = ctx;
    c.r3 = ctx.r4; c.r4 = ctx.r5; c.r5 = ctx.r6; c.r6 = ctx.r7; c.r7 = ctx.r8; c.r8 = ctx.r9; c.r9 = ctx.r10;
    c.r10.u64 = 0;
    sub_803B4A44(c, base);
}

// --- VIWaitForRetrace (experiment) -----------------------------------------------------
// DSE_NOVSYNC=1: return at once instead of waiting for the next retrace.
extern "C" PPC_FUNC(__imp__sub_803C3750);
PPC_FUNC(sub_803C3750)
{
    static const bool noVsync = getenv("DSE_NOVSYNC") != nullptr;
    if (noVsync)
        return;
    __imp__sub_803C3750(ctx, base);
}

// --- SCGetAspectRatio (fn_803E64DC, SYSCONF item 1 "IPL.AR") ------------------------------------
// 0 = 4:3, 1 = 16:9. Any wide aspecto (16:9, 21:9, 32:9) makes the game compose for 16:9;
// the backend widens it further for 21:9 and 32:9.
PPC_FUNC(sub_803E64DC)
{
    ctx.r3.u64 = VideoAspect() > 1.5f ? 1 : 0;
}
