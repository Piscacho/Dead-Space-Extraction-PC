// IOS (the Wii's security processor) replacement at the SDK's client layer.
//
// The game talks to IOS through IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv and their
// Async versions, which send IPC messages to the ARM side and wait for the reply. Here
// those functions are replaced: requests are answered on the PC, by device.
//
// Async requests must not run their callback inside the call: SDK code often marks a
// request as pending *after* the Async function returns, and the callback clears it.
// On the console, the reply arrives later as an IPC interrupt. Here callbacks are queued
// and delivered when the game enables interrupts (OSRestoreInterrupts/OSEnableInterrupts),
// which is also when the hardware would deliver them.
#include "runtime.h"
#include "ios.h"
#include <ppc_recomp_shared.h>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>

#define ORIGINAL(addr) extern "C" PPC_FUNC(__imp__sub_##addr)

namespace
{
std::map<int32_t, std::unique_ptr<IosDevice>> g_fds;
int32_t g_nextFd = 3;

struct PendingCallback
{
    uint32_t callback;
    int32_t result;
    uint32_t arg;
};
std::deque<PendingCallback> g_pending;
bool g_delivering = false;

bool LogIos()
{
    static bool on = getenv("DSE_IOSLOG") != nullptr;
    return on;
}

std::string GuestString(uint32_t addr, size_t max = 256)
{
    std::string s;
    for (size_t i = 0; i < max && Load8(addr + i); i++)
        s += char(Load8(addr + i));
    return s;
}

IosDevice* Lookup(int32_t fd)
{
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second.get();
}

void Complete(uint32_t callback, int32_t result, uint32_t arg)
{
    if (callback)
        g_pending.push_back({ callback, result, arg });
}
} // namespace

// Queues a guest callback fn(r3, r4) to run on the next interrupt delivery.
void QueueGuestCallback(uint32_t fn, uint32_t r3, uint32_t r4)
{
    Complete(fn, int32_t(r3), r4);
}

// Calls a guest function as an interrupt would: on the current stack (below a margin),
// and with every register restored afterwards.
void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t r3, uint32_t r4)
{
    PPCFunc* host = FindGuestFunction(fn);
    if (!host)
    {
        LOG("[ios] callback 0x%08X has no recompiled function", fn);
        return;
    }
    PPCContext saved = ctx;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r1.u32 = (saved.r1.u32 - 0x40) & ~0xFu;  // as close as the console (see RunHandler)
    ctx.lr = 0;
    host(ctx, base);
    ctx = saved;
}

void DeliverIosCallbacks(PPCContext& ctx, uint8_t* base)
{
    if (g_delivering)
        return;
    g_delivering = true;
    // Only the replies queued so far: a callback that sends a new request gets its reply
    // on a later interrupt, like on the console (and cannot keep this loop going forever).
    size_t count = g_pending.size();
    while (count-- > 0 && !g_pending.empty())
    {
        PendingCallback p = g_pending.front();
        g_pending.pop_front();
        if (LogIos())
            LOG("[ios] callback 0x%08X(result %d, arg 0x%08X)", p.callback, p.result, p.arg);
        // Interrupt context: no thread switch inside the callback (OSDisableScheduler).
        constexpr uint32_t kReschedule = 0x80605520;
        uint32_t reschedule = Load32(kReschedule);
        Store32(kReschedule, reschedule + 1);
        CallGuest(ctx, base, p.callback, uint32_t(p.result), p.arg);
        Store32(kReschedule, reschedule);
    }
    g_delivering = false;
}

// --- Request handling --------------------------------------------------------------

static int32_t DoOpen(uint32_t pathAddr, uint32_t mode)
{
    std::string path = GuestString(pathAddr);
    int32_t error = 0;
    std::unique_ptr<IosDevice> dev = CreateIosDevice(path, mode, error);
    if (!dev)
    {
        LOG("[ios] open \"%s\" mode %u -> %d", path.c_str(), mode, error);
        return error;
    }
    int32_t fd = g_nextFd++;
    LOG("[ios] open \"%s\" mode %u -> fd %d", path.c_str(), mode, fd);
    g_fds[fd] = std::move(dev);
    return fd;
}

static int32_t DoClose(int32_t fd)
{
    if (!g_fds.erase(fd))
        return -4;  // IPC_EINVAL
    if (LogIos())
        LOG("[ios] close fd %d", fd);
    return 0;
}

static int32_t DoIoctlv(int32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vec)
{
    IosDevice* d = Lookup(fd);
    if (!d)
        return -4;
    std::vector<IosBuffer> in, out;
    for (uint32_t i = 0; i < numIn + numOut; i++)
    {
        IosBuffer b{ Load32(vec + i * 8), Load32(vec + i * 8 + 4) };
        (i < numIn ? in : out).push_back(b);
    }
    int32_t r = d->Ioctlv(cmd, in, out);
    if (LogIos())
        LOG("[ios] ioctlv %s cmd 0x%X (%u in, %u out) -> %d", d->Name().c_str(), cmd, numIn, numOut, r);
    return r;
}

// --- SDK replacements ---------------------------------------------------------------
// Arguments follow the SDK: IOS_OpenAsync(path, mode, cb, arg), IOS_Read(fd, buf, len),
// IOS_Ioctl(fd, cmd, in, inLen, out, outLen), IOS_Ioctlv(fd, cmd, numIn, numOut, vec) and
// the Async versions with (cb, arg) appended. Async versions return 0 when queued.

PPC_FUNC(sub_803E8734)  // IOS_OpenAsync
{
    if (Load8(ctx.r3.u32) == 0)
        LOG("[ios] IOS_OpenAsync with empty path at 0x%08X (caller lr 0x%08X)", ctx.r3.u32, (uint32_t)ctx.lr);
    int32_t r = DoOpen(ctx.r3.u32, ctx.r4.u32);
    Complete(ctx.r5.u32, r, ctx.r6.u32);
    ctx.r3.s64 = 0;
}

PPC_FUNC(sub_803E884C)  // IOS_Open
{
    if (Load8(ctx.r3.u32) == 0)
        LOG("[ios] IOS_Open with empty path at 0x%08X (caller lr 0x%08X)", ctx.r3.u32, (uint32_t)ctx.lr);
    ctx.r3.s64 = DoOpen(ctx.r3.u32, ctx.r4.u32);
}

PPC_FUNC(sub_803E896C)  // IOS_CloseAsync
{
    int32_t r = DoClose(ctx.r3.s32);
    Complete(ctx.r4.u32, r, ctx.r5.u32);
    ctx.r3.s64 = 0;
}

PPC_FUNC(sub_803E8A2C)  // IOS_Close
{
    ctx.r3.s64 = DoClose(ctx.r3.s32);
}

static int32_t DoRead(int32_t fd, uint32_t buf, uint32_t len)
{
    IosDevice* d = Lookup(fd);
    int32_t r = d ? d->Read(buf, len) : -4;
    if (LogIos())
        LOG("[ios] read fd %d len 0x%X -> %d", fd, len, r);
    return r;
}

static int32_t DoWrite(int32_t fd, uint32_t buf, uint32_t len)
{
    IosDevice* d = Lookup(fd);
    int32_t r = d ? d->Write(buf, len) : -4;
    if (LogIos())
        LOG("[ios] write fd %d len 0x%X -> %d", fd, len, r);
    return r;
}

PPC_FUNC(sub_803E8AD4)  // IOS_ReadAsync
{
    int32_t r = DoRead(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32);
    Complete(ctx.r6.u32, r, ctx.r7.u32);
    ctx.r3.s64 = 0;
}

PPC_FUNC(sub_803E8BD4)  // IOS_Read
{
    ctx.r3.s64 = DoRead(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32);
}

PPC_FUNC(sub_803E8CDC)  // IOS_WriteAsync
{
    int32_t r = DoWrite(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32);
    Complete(ctx.r6.u32, r, ctx.r7.u32);
    ctx.r3.s64 = 0;
}

PPC_FUNC(sub_803E8DDC)  // IOS_Write
{
    ctx.r3.s64 = DoWrite(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32);
}

PPC_FUNC(sub_803E8EE4)  // IOS_SeekAsync(fd, offset, whence, cb, arg)
{
    IosDevice* d = Lookup(ctx.r3.s32);
    int32_t r = d ? d->Seek(ctx.r4.s32, ctx.r5.s32) : -4;
    Complete(ctx.r6.u32, r, ctx.r7.u32);
    ctx.r3.s64 = 0;
}

static int32_t DoIoctl(int32_t fd, uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen)
{
    IosDevice* d = Lookup(fd);
    if (!d)
        return -4;
    int32_t r = d->Ioctl(cmd, in, inLen, out, outLen);
    if (LogIos())
        LOG("[ios] ioctl %s cmd 0x%X in 0x%X out 0x%X -> %d", d->Name().c_str(), cmd, inLen, outLen, r);
    return r;
}

PPC_FUNC(sub_803E8FC4)  // IOS_IoctlAsync
{
    // The callback and its argument are the 7th and 8th arguments: r9 and r10.
    int32_t r = DoIoctl(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    if (r != kIosPending)
        Complete(ctx.r9.u32, r, ctx.r10.u32);
    ctx.r3.s64 = 0;
}

PPC_FUNC(sub_803E90FC)  // IOS_Ioctl
{
    int32_t r = DoIoctl(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    ctx.r3.s64 = r == kIosPending ? 0 : r;
}

PPC_FUNC(sub_803E9368)  // IOS_IoctlvAsync(fd, cmd, numIn, numOut, vec, cb, arg)
{
    int32_t r = DoIoctlv(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    if (r != kIosPending)
        Complete(ctx.r8.u32, r, ctx.r9.u32);
    ctx.r3.s64 = 0;
}

PPC_FUNC(sub_803E944C)  // IOS_Ioctlv
{
    int32_t r = DoIoctlv(ctx.r3.s32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.s64 = r == kIosPending ? 0 : r;
}

// Queued callbacks are delivered by the interrupt pump (threads.cpp), when the game
// re-enables interrupts.
