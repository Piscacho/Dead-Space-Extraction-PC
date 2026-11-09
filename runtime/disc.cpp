// The game disc, read from the extracted data partition (juego/DATA: sys/ and files/).
//
// Disc offsets are partition-relative, as IOS's /dev/di serves them once the game
// partition is open. The partition is rebuilt from:
//   - sys/boot.bin at 0, sys/bi2.bin at 0x440, sys/apploader.img at 0x2440,
//   - sys/main.dol and sys/fst.bin at the offsets the header gives (0x420, 0x424, >> 2),
//   - every file of the FST at its own offset (Wii FST offsets are >> 2).
#include "runtime.h"
#include "disc.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
struct Extent
{
    uint64_t offset;
    uint64_t size;
    std::string path;  // host file
};

std::vector<Extent> g_extents;  // sorted by offset
std::vector<uint8_t> g_fst;
std::string g_root;

std::vector<uint8_t> ReadAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void AddFile(uint64_t offset, const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return;
    _fseeki64(f, 0, SEEK_END);
    uint64_t size = uint64_t(_ftelli64(f));
    fclose(f);
    g_extents.push_back({ offset, size, path });
}

// Walks the FST directory tree, adding each file under files/.
void AddFstFiles()
{
    if (g_fst.size() < 12)
        return;
    uint32_t count = Be32(&g_fst[8]);
    if (count * 12 > g_fst.size())
        return;
    const char* names = reinterpret_cast<const char*>(&g_fst[count * 12]);
    // Directory stack: (end index, path prefix).
    std::vector<std::pair<uint32_t, std::string>> dirs{ { count, "" } };
    for (uint32_t i = 1; i < count; i++)
    {
        while (dirs.size() > 1 && i >= dirs.back().first)
            dirs.pop_back();
        const uint8_t* e = &g_fst[i * 12];
        bool dir = e[0] != 0;
        std::string name = names + (Be32(e) & 0xFFFFFF);
        std::string path = dirs.back().second + name;
        if (dir)
            dirs.push_back({ Be32(e + 8), path + "/" });
        else
            g_extents.push_back({ uint64_t(Be32(e + 4)) << 2, Be32(e + 8), g_root + "/files/" + path });
    }
}
} // namespace

bool DiscInit(const std::string& partitionRoot)
{
    g_root = partitionRoot;
    std::vector<uint8_t> header = ReadAll(g_root + "/sys/boot.bin");
    g_fst = ReadAll(g_root + "/sys/fst.bin");
    if (header.size() < 0x440 || g_fst.empty())
    {
        LOG("[disc] cannot read %s/sys (boot.bin, fst.bin)", g_root.c_str());
        return false;
    }
    AddFile(0, g_root + "/sys/boot.bin");
    AddFile(0x440, g_root + "/sys/bi2.bin");
    AddFile(0x2440, g_root + "/sys/apploader.img");
    AddFile(uint64_t(Be32(&header[0x420])) << 2, g_root + "/sys/main.dol");
    AddFile(uint64_t(Be32(&header[0x424])) << 2, g_root + "/sys/fst.bin");
    AddFstFiles();
    std::sort(g_extents.begin(), g_extents.end(), [](const Extent& a, const Extent& b) { return a.offset < b.offset; });
    LOG("[disc] %zu files from %s", g_extents.size(), g_root.c_str());
    return true;
}

const std::vector<uint8_t>& DiscFst() { return g_fst; }

bool DiscRead(uint64_t offset, uint32_t length, uint8_t* dest)
{
    bool complete = true;
    while (length > 0)
    {
        // Last extent starting at or before the offset.
        auto it = std::upper_bound(g_extents.begin(), g_extents.end(), offset,
            [](uint64_t o, const Extent& e) { return o < e.offset; });
        if (it == g_extents.begin() || offset >= std::prev(it)->offset + std::prev(it)->size)
        {
            // Gap between files: zeros (the disc has padding there too).
            uint64_t next = it == g_extents.end() ? offset + length : it->offset;
            uint32_t n = uint32_t(std::min<uint64_t>(length, next - offset));
            memset(dest, 0, n);
            if (it == g_extents.end())
                complete = false;
            dest += n;
            offset += n;
            length -= n;
            continue;
        }
        const Extent& e = *std::prev(it);
        uint32_t n = uint32_t(std::min<uint64_t>(length, e.offset + e.size - offset));
        FILE* f = fopen(e.path.c_str(), "rb");
        if (!f)
        {
            memset(dest, 0, n);
            complete = false;
        }
        else
        {
            _fseeki64(f, int64_t(offset - e.offset), SEEK_SET);
            size_t got = fread(dest, 1, n, f);
            if (got < n)
                memset(dest + got, 0, n - got);
            fclose(f);
        }
        dest += n;
        offset += n;
        length -= n;
    }
    return complete;
}
