// IOS devices answered on the PC (see ios.cpp for the client layer).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct PPCContext;

// Returned by a device for a request that never completes (e.g. an event hook that
// only fires on power-button presses): the callback is not queued.
constexpr int32_t kIosPending = 0x7FFFFFFF;

// Error codes the SDK checks for.
constexpr int32_t kIpcEInval = -4;
constexpr int32_t kIpcENoEnt = -6;
constexpr int32_t kFsENoEnt = -106;
constexpr int32_t kFsEExist = -105;

struct IosBuffer
{
    uint32_t addr;
    uint32_t size;
};

class IosDevice
{
public:
    explicit IosDevice(std::string name) : name_(std::move(name)) {}
    virtual ~IosDevice() = default;
    const std::string& Name() const { return name_; }

    virtual int32_t Read(uint32_t buf, uint32_t len);
    virtual int32_t Write(uint32_t buf, uint32_t len);
    virtual int32_t Seek(int32_t offset, int32_t whence);
    virtual int32_t Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen);
    virtual int32_t Ioctlv(uint32_t cmd, const std::vector<IosBuffer>& in, const std::vector<IosBuffer>& out);

protected:
    void Unhandled(const char* what, uint32_t cmd);

private:
    std::string name_;
};

// Opens a device or NAND file. On failure returns nullptr and sets error.
std::unique_ptr<IosDevice> CreateIosDevice(const std::string& path, uint32_t mode, int32_t& error);

// Queues a guest callback fn(r3, r4) for the next interrupt delivery.
void QueueGuestCallback(uint32_t fn, uint32_t r3, uint32_t r4);
// Runs queued Async callbacks (as the IPC interrupt would).
void DeliverIosCallbacks(PPCContext& ctx, uint8_t* base);
// Calls a guest function with r3/r4 set, restoring all registers afterwards.
void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t fn, uint32_t r3, uint32_t r4);
