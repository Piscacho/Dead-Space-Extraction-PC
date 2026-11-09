// Guest threads and interrupts.
//
// Threads: the Wii has one core and the SDK switches threads itself (SelectThread saves
// the running thread with OSSaveContext and jumps to the next one with OSLoadContext).
// Recompiled code cannot jump into a saved register set, so each guest thread runs on
// its own host fiber, with its own PPCContext and host stack, and only one runs at a time
// (single-core semantics, no locking needed):
//   - OSSaveContext(thread) binds the running fiber to that thread.
//   - OSLoadContext(thread), from SelectThread, switches to the thread's fiber, creating
//     it the first time from the context OSCreateThread prepared (entry in srr0).
//
// Interrupts: there is no hardware raising them, so a "pump" raises them on the PC clock
// and runs the SDK's own handlers: IOS replies, the 60 Hz video retrace (VI) and OS
// alarms (decrementer). It runs when the game re-enables interrupts, and blocks in the
// scheduler's idle loop until an interrupt makes a thread ready.
#include "runtime.h"
void WatchRearm();  // main.cpp (DSE_WATCH)
#include "audio.h"
#include "gx.h"
#include "ios.h"
#include <ppc_recomp_shared.h>
#include <windows.h>
#include <csetjmp>
#include <unordered_map>

#define ORIGINAL(addr) extern "C" PPC_FUNC(__imp__sub_##addr)

namespace
{
// Low memory: current OSContext and current OSThread.
constexpr uint32_t kCurrentContext = 0x800000D4;
constexpr uint32_t kCurrentThread = 0x800000E4;
constexpr uint32_t kSystemTimeOffset = 0x800030D8;   // added to the time base by __OSGetSystemTime

// SDK addresses.
constexpr uint32_t kSelectThreadLoad = 0x803BB340;   // return address of SelectThread's OSLoadContext call
constexpr uint32_t kSelectThreadIdle = 0x803BB280;   // return address of the idle loop's OSEnableInterrupts
constexpr uint32_t kRunQueueBits = 0x80605528;
constexpr uint32_t kReschedule = 0x80605520;         // > 0 while OSDisableScheduler is in effect
constexpr uint32_t kOSReschedule = 0x803BB35C;       // __OSReschedule
constexpr uint32_t kThreadPriority = 0x2D0;          // OSThread: effective priority
constexpr uint32_t kInterruptHandlerTable = 0x806054F8;  // pointer to the 32 handlers
constexpr uint32_t kAlarmQueue = 0x806054A0;         // first pending OSAlarm; fire time (u64) at +8
constexpr uint32_t kDecrementerCallback = 0x803B2F14;
constexpr uint32_t kOSExitThread = 0x803BB5E0;
constexpr uint32_t kViInterrupt = 24;                // __OS_INTERRUPT_PI_VI
constexpr uint32_t kViDisplayInt0 = 0xCC002030;      // VI display interrupt 0; bit 0x8000 = fired

// OSContext layout.
constexpr uint32_t kCtxGpr = 0x000, kCtxLr = 0x084, kCtxCtr = 0x088, kCtxSrr0 = 0x198, kCtxGqr = 0x1A4;

struct GuestFiber
{
    void* fiber = nullptr;
    uint32_t thread = 0;
    PPCContext* ctx = nullptr;
    uint32_t entry = 0;
};

std::unordered_map<uint32_t, GuestFiber*> g_byThread;
GuestFiber g_mainFiber;
GuestFiber* g_current = &g_mainFiber;

// Interrupt handlers end with OSLoadContext(interrupted context), which must not return
// into the code after it. While a handler runs, that call jumps back to the pump.
bool g_inHandler = false;
jmp_buf g_handlerExit;

uint64_t HostNow100ns()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return uint64_t(c.QuadPart) * 10000000ull / uint64_t(freq.QuadPart);
}

void WINAPI FiberMain(void* param)
{
    auto* f = static_cast<GuestFiber*>(param);
    PPCFunc* entry = FindGuestFunction(f->entry);
    LOG("[thread] thread 0x%08X starts at 0x%08X", f->thread, f->entry);
    if (entry)
        entry(*f->ctx, g_base);
    else
        LOG("[thread] entry 0x%08X has no recompiled function", f->entry);
    // On the console the entry "returns" into OSExitThread (its lr), which never returns.
    if (PPCFunc* exitThread = FindGuestFunction(kOSExitThread))
        exitThread(*f->ctx, g_base);
    LOG("[thread] thread 0x%08X returned from OSExitThread", f->thread);
    for (;;)
        SwitchToFiber(g_mainFiber.fiber);
}

GuestFiber* CreateGuestFiber(uint32_t thread)
{
    auto* f = new GuestFiber;
    f->thread = thread;
    f->ctx = new PPCContext{};
    PPCContext& c = *f->ctx;
    // Registers prepared by OSCreateThread: r1 stack, r2/r13 small data, r3 argument.
    uint64_t* gprs[] = { &c.r0.u64, &c.r1.u64, &c.r2.u64, &c.r3.u64, &c.r4.u64, &c.r5.u64, &c.r6.u64, &c.r7.u64,
        &c.r8.u64, &c.r9.u64, &c.r10.u64, &c.r11.u64, &c.r12.u64, &c.r13.u64, &c.r14.u64, &c.r15.u64,
        &c.r16.u64, &c.r17.u64, &c.r18.u64, &c.r19.u64, &c.r20.u64, &c.r21.u64, &c.r22.u64, &c.r23.u64,
        &c.r24.u64, &c.r25.u64, &c.r26.u64, &c.r27.u64, &c.r28.u64, &c.r29.u64, &c.r30.u64, &c.r31.u64 };
    for (int i = 0; i < 32; i++)
        *gprs[i] = Load32(thread + kCtxGpr + 4 * i);
    c.lr = Load32(thread + kCtxLr);
    c.ctr.u64 = Load32(thread + kCtxCtr);
    for (int i = 0; i < 8; i++)
        c.spr[912 + i] = Load32(thread + kCtxGqr + 4 * i);
    c.fpscr.csr = c.fpscr.getcsr();
    f->entry = Load32(thread + kCtxSrr0);
    f->fiber = CreateFiberEx(64 * 1024, 16 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, FiberMain, f);
    if (!f->fiber)
        LOG("[thread] CreateFiberEx failed (error %lu)", GetLastError());
    g_byThread[thread] = f;
    return f;
}

// Special-purpose registers belong to the CPU, not to a thread (HID2 with the locked
// cache enable, DMA registers...), except the GQRs (912-919), which the SDK saves with
// each thread's context. Every PPCContext has its own copy, so they follow the switch.
void ShareSprs(const PPCContext& from, PPCContext& to)
{
    uint32_t gqr[8];
    memcpy(gqr, &to.spr[912], sizeof(gqr));
    memcpy(to.spr, from.spr, sizeof(to.spr));
    memcpy(&to.spr[912], gqr, sizeof(gqr));
}

void SwitchTo(GuestFiber* next)
{
    if (next == g_current)
        return;
    GuestFiber* prev = g_current;
    if (prev->ctx && next->ctx)
        ShareSprs(*prev->ctx, *next->ctx);
    g_current = next;
    SwitchToFiber(next->fiber);
    // Back here when some thread switches to `prev` again.
    g_current = prev;
}

// Calls an SDK interrupt or exception handler as the hardware would, on the current
// thread's stack with all registers restored afterwards.
void RunHandler(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t r3, uint32_t r4)
{
    PPCFunc* host = FindGuestFunction(fn);
    if (!host)
        return;
    PPCContext saved = ctx;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    // Right below the interrupted thread's stack pointer, like the console (the PowerPC
    // EABI has no red zone). A larger gap overflows the small stacks of some threads
    // (THP's) into the OSThread next to them.
    ctx.r1.u32 = (saved.r1.u32 - 0x40) & ~0xFu;
    ctx.lr = 0;
    bool outer = g_inHandler;
    jmp_buf outerExit;
    memcpy(outerExit, g_handlerExit, sizeof(jmp_buf));
    g_inHandler = true;
    // Like __OSDispatchInterrupt: no thread switch while a handler runs. A handler that
    // wakes a thread only makes it ready; the switch happens afterwards (preemption).
    uint32_t reschedule = Load32(kReschedule);
    Store32(kReschedule, reschedule + 1);
    if (setjmp(g_handlerExit) == 0)
        host(ctx, base);
    Store32(kReschedule, reschedule);
    g_inHandler = outer;
    memcpy(g_handlerExit, outerExit, sizeof(jmp_buf));
    ctx = saved;
}

// --- Interrupt sources -----------------------------------------------------------------

constexpr uint64_t kViPeriod100ns = 166833;  // NTSC: 59.94 Hz
uint64_t g_nextRetrace = 0;
uint64_t g_nextAi = 0;
constexpr uint32_t kAiInterrupt = 5;           // __OS_INTERRUPT_DSP_AI
constexpr uint32_t kAiAddrHi = 0xCC005030, kAiAddrLo = 0xCC005032, kAiControl = 0xCC005036;
uint32_t g_retraces = 0;
bool g_pumping = false;

uint64_t GuestSystemTime()
{
    uint64_t offset = (uint64_t(Load32(kSystemTimeOffset)) << 32) | Load32(kSystemTimeOffset + 4);
    return ppc_mftb() + offset;
}

// Raises every interrupt that is due. Returns the host time (100 ns units) of the next
// one, so an idle caller can sleep until then.
uint64_t RaiseDueInterrupts(PPCContext& ctx, uint8_t* base)
{
    DeliverIosCallbacks(ctx, base);

    uint64_t now = HostNow100ns();
    if (g_nextRetrace == 0)
        g_nextRetrace = now + kViPeriod100ns;
    uint32_t table = Load32(kInterruptHandlerTable);
    if (now >= g_nextRetrace)
    {
        // Skip missed retraces instead of raising a burst of them.
        while (g_nextRetrace <= now)
            g_nextRetrace += kViPeriod100ns;
        uint32_t handler = table ? Load32(table + kViInterrupt * 4) : 0;
        if (handler)
        {
            Store16(kViDisplayInt0, Load16(kViDisplayInt0) | 0x8000);
            RunHandler(ctx, base, handler, kViInterrupt, Load32(kCurrentContext));
            if (++g_retraces == 1 || g_retraces % 600 == 0)
            {
                const GxStats& s = GxGetStats();
                LOG("[vi] retrace %u | gx: %llu KB, %llu draws, %llu vertices, %llu display lists, %llu XFB copies, %llu texture copies, %llu unknown",
                    g_retraces, (unsigned long long)(s.bytes / 1024), (unsigned long long)s.draws,
                    (unsigned long long)s.vertices, (unsigned long long)s.displayLists,
                    (unsigned long long)s.xfbCopies, (unsigned long long)s.textureCopies, (unsigned long long)s.unknown);
            }
        }
        VideoPresent();
        WatchRearm();
    }

    // AI DMA (audio): while DMA is on (0xCC005036 bit 15), the block set up with AIInitDMA
    // (address in 0xCC005030/32, length in 32-byte units in 0x5036) "plays" for its
    // duration at 32 kHz stereo, then interrupt 5 asks for the next one. AX and the THP
    // player hook that interrupt to produce their audio.
    if (Load16(kAiControl) & 0x8000)
    {
        uint32_t addr = ((uint32_t(Load16(kAiAddrHi)) << 16 | Load16(kAiAddrLo)) & 0x03FFFFE0) | 0x80000000;
        uint32_t bytes = (Load16(kAiControl) & 0x7FFF) * 32;
        uint64_t period = bytes ? uint64_t(bytes / 4) * 10000000ull / 32000 : 30000;
        if (g_nextAi == 0)
            g_nextAi = now;
        // With a sound card, its clock sets the pace: a block is due whenever less than
        // ~100 ms is queued. Without one, the PC clock does.
        constexpr int64_t kQueueTarget = 32000 / 10;
        int64_t queued = AudioQueuedFrames();
        auto due = [&]() {
            return queued >= 0 ? AudioQueuedFrames() < kQueueTarget : now >= g_nextAi;
        };
        int burst = 0;
        while (due() && burst++ < 16)
        {
            AudioPushBigEndianRL(g_base + addr, bytes / 4);
            g_nextAi += period;
            uint32_t handler = table ? Load32(table + kAiInterrupt * 4) : 0;
            if (handler)
                RunHandler(ctx, base, handler, kAiInterrupt, Load32(kCurrentContext));
            addr = ((uint32_t(Load16(kAiAddrHi)) << 16 | Load16(kAiAddrLo)) & 0x03FFFFE0) | 0x80000000;
            bytes = (Load16(kAiControl) & 0x7FFF) * 32;
        }
        if (queued < 0 && now >= g_nextAi)
            g_nextAi = now + period;  // far behind: skip instead of catching up
    }
    else
        g_nextAi = 0;

    // PE interrupts raised by the command stream (draw sync tokens, draw done).
    if (uint32_t gx = GxTakePendingInterrupts())
    {
        for (uint32_t n : { kGxInterruptToken, kGxInterruptFinish })
        {
            uint32_t handler = table ? Load32(table + n * 4) : 0;
            if ((gx >> n) & 1 && handler)
                RunHandler(ctx, base, handler, n, Load32(kCurrentContext));
        }
    }

    // OS alarms: the decrementer exception fires the ones that are due.
    uint32_t alarm = Load32(kAlarmQueue);
    if (alarm)
    {
        uint64_t fire = (uint64_t(Load32(alarm + 8)) << 32) | Load32(alarm + 12);
        if (GuestSystemTime() >= fire)
            RunHandler(ctx, base, kDecrementerCallback, 8, Load32(kCurrentContext));
    }
    return g_nextRetrace;
}

void Pump(PPCContext& ctx, uint8_t* base, bool idle)
{
    if (g_pumping)
        return;
    g_pumping = true;
    for (;;)
    {
        uint64_t next = RaiseDueInterrupts(ctx, base);
        if (!idle || Load32(kRunQueueBits) != 0)
            break;
        // Idle: nothing can run until an interrupt readies a thread. Sleep until the next
        // retrace, or 1 ms when an alarm or IOS reply may come sooner.
        uint64_t now = HostNow100ns();
        uint64_t wait = next > now ? (next - now) / 10000 : 0;
        Sleep(DWORD(wait > 1 ? 1 : wait));
    }
    g_pumping = false;

    // Preemption: on the console, when an interrupt readies a thread with a higher
    // priority than the running one, the dispatcher reschedules before returning.
    // Lower number = higher priority; the highest ready one is cntlzw(RunQueueBits).
    if (!idle && !g_inHandler && Load32(kReschedule) == 0)
    {
        uint32_t ready = Load32(kRunQueueBits);
        uint32_t current = Load32(kCurrentThread);
        if (ready && current && uint32_t(__builtin_clz(ready)) < Load32(current + kThreadPriority))
            RunHandler(ctx, base, kOSReschedule, 0, 0);
    }
}
} // namespace

void InitGuestThreads(PPCContext* mainCtx)
{
    g_mainFiber.ctx = mainCtx;
    g_mainFiber.fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_mainFiber.fiber)
        LOG("[thread] ConvertThreadToFiberEx failed (error %lu)", GetLastError());
}

// --- SDK replacements ------------------------------------------------------------------

ORIGINAL(803B4478);
PPC_FUNC(sub_803B4478)  // OSSaveContext(context): the running fiber belongs to this thread
{
    uint32_t context = ctx.r3.u32;
    if (g_current->thread != context)
    {
        if (g_current->thread)
            g_byThread.erase(g_current->thread);
        g_current->thread = context;
        g_byThread[context] = g_current;
    }
    __imp__sub_803B4478(ctx, base);
    ctx.r3.u64 = 0;
}

PPC_FUNC(sub_803B44F8)  // OSLoadContext(context)
{
    uint32_t context = ctx.r3.u32;
    if ((uint32_t)ctx.lr == kSelectThreadLoad)
    {
        auto it = g_byThread.find(context);
        GuestFiber* next = it != g_byThread.end() ? it->second : CreateGuestFiber(context);
        SwitchTo(next);
        return;
    }
    // Any other OSLoadContext returns from an interrupt or exception handler to the
    // interrupted context, which is the one still running here.
    if (g_inHandler)
        longjmp(g_handlerExit, 1);
}

// OSGetTime: game loops time their frames with it, sometimes without re-enabling
// interrupts in between, so interrupts are also raised here, when they are enabled
// (MSR[EE]) as they would be on the console at any moment.
constexpr uint32_t kMsrEE = 0x8000;
ORIGINAL(803BBFF0);
PPC_FUNC(sub_803BBFF0)  // OSGetTime
{
    if (ctx.msr & kMsrEE)
    {
        PPCContext saved = ctx;
        Pump(ctx, base, false);
        ctx = saved;
    }
    __imp__sub_803BBFF0(ctx, base);
}

ORIGINAL(803B8180);
PPC_FUNC(sub_803B8180)  // OSEnableInterrupts
{
    bool idle = (uint32_t)ctx.lr == kSelectThreadIdle;
    __imp__sub_803B8180(ctx, base);
    uint32_t previous = ctx.r3.u32;
    Pump(ctx, base, idle);
    ctx.r3.u64 = previous;
}

ORIGINAL(803B8194);
PPC_FUNC(sub_803B8194)  // OSRestoreInterrupts(level): interrupts come back on when level != 0
{
    bool enabling = ctx.r3.u32 != 0;
    __imp__sub_803B8194(ctx, base);
    if (enabling)
    {
        uint32_t previous = ctx.r3.u32;
        Pump(ctx, base, false);
        ctx.r3.u64 = previous;
    }
}
