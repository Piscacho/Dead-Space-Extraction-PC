// IOS devices. Each one answers the requests the game makes; anything not handled yet is
// logged once (with its command) and answered with a neutral result.
#include "runtime.h"
#include "disc.h"
#include "ios.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

// --- Base device -------------------------------------------------------------------

void IosDevice::Unhandled(const char* what, uint32_t cmd)
{
    static std::set<std::string> seen;
    std::string key = name_ + ":" + what + ":" + std::to_string(cmd);
    if (seen.insert(key).second)
        LOG("[ios] %s: unhandled %s 0x%X (answered 0)", name_.c_str(), what, cmd);
}

int32_t IosDevice::Read(uint32_t, uint32_t len) { Unhandled("read", len); return 0; }
int32_t IosDevice::Write(uint32_t, uint32_t len) { Unhandled("write", len); return int32_t(len); }
int32_t IosDevice::Seek(int32_t, int32_t whence) { Unhandled("seek", uint32_t(whence)); return 0; }
int32_t IosDevice::Ioctl(uint32_t cmd, uint32_t, uint32_t, uint32_t, uint32_t) { Unhandled("ioctl", cmd); return 0; }
int32_t IosDevice::Ioctlv(uint32_t cmd, const std::vector<IosBuffer>&, const std::vector<IosBuffer>&) { Unhandled("ioctlv", cmd); return 0; }

// --- NAND ---------------------------------------------------------------------------
// The console's internal memory is a folder next to the executable: user/nand.

static fs::path NandRoot()
{
    static fs::path root = [] {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        fs::path p = fs::path(exe).parent_path() / "user" / "nand";
        fs::create_directories(p);
        LOG("[nand] root %s", p.string().c_str());
        return p;
    }();
    return root;
}

static fs::path NandPath(const std::string& guest)
{
    std::string rel = guest;
    while (!rel.empty() && rel[0] == '/')
        rel.erase(0, 1);
    return NandRoot() / fs::path(rel);
}

class NandFile : public IosDevice
{
public:
    NandFile(const std::string& path, FILE* f) : IosDevice(path), f_(f) {}
    ~NandFile() override { fclose(f_); }

    // C streams need a seek between reads and writes, so every call re-seeks to its position.
    int32_t Read(uint32_t buf, uint32_t len) override
    {
        fseek(f_, 0, SEEK_CUR);
        return int32_t(fread(g_base + buf, 1, len, f_));
    }
    int32_t Write(uint32_t buf, uint32_t len) override
    {
        fseek(f_, 0, SEEK_CUR);
        int32_t n = int32_t(fwrite(g_base + buf, 1, len, f_));
        fflush(f_);
        return n;
    }
    int32_t Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen) override
    {
        if (cmd == 0xB && outLen >= 8)  // GetFileStats -> (length, position)
        {
            long pos = ftell(f_);
            fseek(f_, 0, SEEK_END);
            long size = ftell(f_);
            fseek(f_, pos, SEEK_SET);
            Store32(out, uint32_t(size));
            Store32(out + 4, uint32_t(pos));
            return 0;
        }
        return IosDevice::Ioctl(cmd, in, inLen, out, outLen);
    }
    int32_t Seek(int32_t offset, int32_t whence) override
    {
        if (fseek(f_, offset, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET) != 0)
            return -101;  // FS_EINVAL
        return int32_t(ftell(f_));
    }

private:
    FILE* f_;
};

// /dev/fs: file system management (create, delete, attributes, directory listing).
class FsDevice : public IosDevice
{
public:
    FsDevice() : IosDevice("/dev/fs") {}

    int32_t Ioctl(uint32_t cmd, uint32_t in, uint32_t inLen, uint32_t out, uint32_t outLen) override
    {
        switch (cmd)
        {
        case 0x3:  // CreateDir: attributes block, path at +6
        {
            std::string path = Str(in + 6);
            std::error_code ec;
            fs::create_directories(NandPath(path), ec);
            LOG("[nand] create dir %s", path.c_str());
            return 0;
        }
        case 0x6:  // GetAttr: path in, attributes out
        {
            std::string path = Str(in);
            bool exists = fs::exists(NandPath(path));
            LOG("[nand] get attr %s -> %s", path.c_str(), exists ? "exists" : "not found");
            if (!exists)
                return kFsENoEnt;
            memset(g_base + out, 0, outLen);
            return 0;
        }
        case 0x7:  // Delete
        {
            std::string path = Str(in);
            std::error_code ec;
            bool removed = fs::remove_all(NandPath(path), ec) > 0;
            LOG("[nand] delete %s", path.c_str());
            return removed ? 0 : kFsENoEnt;
        }
        case 0x8:  // Rename: old path at 0, new path at 0x40
        {
            std::string from = Str(in), to = Str(in + 0x40);
            std::error_code ec;
            fs::create_directories(NandPath(to).parent_path(), ec);
            fs::rename(NandPath(from), NandPath(to), ec);
            LOG("[nand] rename %s -> %s", from.c_str(), to.c_str());
            return ec ? kFsENoEnt : 0;
        }
        case 0x9:  // CreateFile: attributes block, path at +6
        {
            std::string path = Str(in + 6);
            fs::path p = NandPath(path);
            if (fs::exists(p))
                return kFsEExist;
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            if (FILE* f = _wfopen(p.c_str(), L"wb"))
                fclose(f);
            LOG("[nand] create file %s", path.c_str());
            return 0;
        }
        default:
            Unhandled("ioctl", cmd);
            return 0;
        }
    }

    int32_t Ioctlv(uint32_t cmd, const std::vector<IosBuffer>& in, const std::vector<IosBuffer>& out) override
    {
        switch (cmd)
        {
        case 0x4:  // ReadDir(path [, max]) -> (names, count)
        {
            std::string path = in.empty() ? "" : Str(in[0].addr);
            fs::path p = NandPath(path);
            if (!fs::is_directory(p))
                return kFsENoEnt;
            uint32_t count = 0;
            uint32_t names = out.size() > 1 ? out[0].addr : 0;
            uint32_t cursor = names;
            for (auto& e : fs::directory_iterator(p))
            {
                if (names)
                {
                    std::string n = e.path().filename().string();
                    memcpy(g_base + cursor, n.c_str(), n.size() + 1);
                    cursor += uint32_t(n.size() + 1);
                }
                count++;
            }
            if (!out.empty())
                Store32(out.back().addr, count);
            LOG("[nand] read dir %s -> %u entries", path.c_str(), count);
            return 0;
        }
        case 0xC:  // GetUsage(path) -> (clusters, inodes)
            for (auto& b : out)
                Store32(b.addr, 0);
            return 0;
        default:
            Unhandled("ioctlv", cmd);
            return 0;
        }
    }

private:
    static std::string Str(uint32_t a)
    {
        std::string s;
        for (uint32_t i = 0; i < 64 && Load8(a + i); i++)
            s += char(Load8(a + i));
        return s;
    }
};

// /dev/stm/immediate and /dev/stm/eventhook: power and reset. The event hook only
// answers when the power or reset button is pressed, so it stays pending.
class StmDevice : public IosDevice
{
public:
    explicit StmDevice(const std::string& path) : IosDevice(path), hook_(path.find("eventhook") != std::string::npos) {}
    int32_t Ioctl(uint32_t cmd, uint32_t, uint32_t, uint32_t, uint32_t) override
    {
        if (hook_ && cmd == 0x1000)
            return kIosPending;
        return 0;
    }

private:
    bool hook_;
};

// /dev/usb/oh1/57e/305: the Bluetooth adapter the Wii Remotes talk through. The SDK's
// Bluetooth stack sends HCI commands and waits for events here. Requests stay pending,
// so the stack sees no adapter activity and no remotes; input is taken at the KPAD
// level instead.
class BluetoothDevice : public IosDevice
{
public:
    BluetoothDevice() : IosDevice("/dev/usb/oh1/57e/305") {}
    int32_t Ioctl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return kIosPending; }
    int32_t Ioctlv(uint32_t, const std::vector<IosBuffer>&, const std::vector<IosBuffer>&) override { return kIosPending; }
};

// /dev/di: the disc drive. Commands take a 0x20-byte block (command << 24, then
// arguments) and return 1 for success. Reads (0x71: length, offset >> 2) come from the
// extracted partition; the rest (inquiry, cover and status registers, motor) succeed.
class DiDevice : public IosDevice
{
public:
    DiDevice() : IosDevice("/dev/di") {}

    int32_t Ioctl(uint32_t cmd, uint32_t in, uint32_t, uint32_t out, uint32_t outLen) override
    {
        switch (cmd)
        {
        case 0x71:  // Read
        case 0x8D:  // UnencryptedRead
        {
            uint32_t length = Load32(in + 4);
            uint64_t offset = uint64_t(Load32(in + 8)) << 2;
            uint32_t n = std::min(length, outLen);
            bool ok = DiscRead(offset, n, g_base + out);
            static int logged = 0;
            if (logged++ < 20 || !ok)
                LOG("[di] read 0x%llX + 0x%X -> 0x%08X%s", (unsigned long long)offset, n, out, ok ? "" : " (past the end)");
            return 1;
        }
        case 0x70:  // ReadDiskID: the first 0x20 bytes of the disc header
            DiscRead(0, std::min<uint32_t>(outLen, 0x20), g_base + out);
            return 1;
        default:
            if (outLen)
                memset(g_base + out, 0, outLen);
            return 1;
        }
    }

    int32_t Ioctlv(uint32_t cmd, const std::vector<IosBuffer>&, const std::vector<IosBuffer>&) override
    {
        // 0x8B OpenPartition and friends: the partition is already the extracted one.
        if (cmd != 0x8B)
            Unhandled("ioctlv", cmd);
        return 1;
    }
};

// /dev/es: the title manager. The NAND library asks for the running title's ID (0x20)
// and its data folder (0x1D) to know where saves go. The title is 00010000 + the game ID
// from the disc header ("RZJE").
class EsDevice : public IosDevice
{
public:
    EsDevice() : IosDevice("/dev/es") {}

    static uint64_t TitleId()
    {
        uint8_t id[4] = {};
        DiscRead(0, 4, id);
        return 0x0001000000000000ull | uint64_t(id[0]) << 24 | uint64_t(id[1]) << 16 | uint64_t(id[2]) << 8 | id[3];
    }

    int32_t Ioctlv(uint32_t cmd, const std::vector<IosBuffer>& in, const std::vector<IosBuffer>& out) override
    {
        switch (cmd)
        {
        case 0x20:  // GetTitleID -> u64
            if (!out.empty())
            {
                uint64_t t = TitleId();
                Store32(out[0].addr, uint32_t(t >> 32));
                Store32(out[0].addr + 4, uint32_t(t));
            }
            return 0;
        case 0x1D:  // GetTitleDir(u64 title) -> "/title/hi/lo/data"
        {
            uint64_t t = in.empty() ? TitleId() : (uint64_t(Load32(in[0].addr)) << 32 | Load32(in[0].addr + 4));
            char dir[64];
            snprintf(dir, sizeof(dir), "/title/%08x/%08x/data", uint32_t(t >> 32), uint32_t(t));
            if (!out.empty())
                memcpy(g_base + out[0].addr, dir, std::min<size_t>(strlen(dir) + 1, out[0].size));
            std::error_code ec;
            fs::create_directories(NandPath(dir), ec);
            LOG("[es] title dir %s", dir);
            return 0;
        }
        default:
            // Ticket views, consumption and the rest: zeroed answers.
            for (const IosBuffer& b : out)
                memset(g_base + b.addr, 0, b.size);
            if (cmd != 0x1B && cmd != 0x16)
                Unhandled("ioctlv", cmd);
            return 0;
        }
    }
};

class GenericDevice : public IosDevice
{
public:
    explicit GenericDevice(const std::string& path) : IosDevice(path) {}
};

std::unique_ptr<IosDevice> CreateIosDevice(const std::string& path, uint32_t mode, int32_t& error)
{
    if (path == "/dev/fs")
        return std::make_unique<FsDevice>();
    if (path.rfind("/dev/stm/", 0) == 0)
        return std::make_unique<StmDevice>(path);
    if (path == "/dev/usb/oh1/57e/305")
        return std::make_unique<BluetoothDevice>();
    if (path == "/dev/di")
        return std::make_unique<DiDevice>();
    if (path == "/dev/es")
        return std::make_unique<EsDevice>();
    if (path.rfind("/dev/", 0) == 0)
        return std::make_unique<GenericDevice>(path);

    // Anything else is a NAND file. Mode: 1 read, 2 write, 3 read/write.
    fs::path p = NandPath(path);
    if (!fs::exists(p))
    {
        error = kFsENoEnt;
        return nullptr;
    }
    FILE* f = _wfopen(p.c_str(), (mode & 2) ? L"r+b" : L"rb");
    if (!f)
    {
        error = -102;  // FS_EACCESS
        return nullptr;
    }
    return std::make_unique<NandFile>(path, f);
}
