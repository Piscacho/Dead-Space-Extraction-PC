// Boot: load main.dol into guest memory and set the low-memory globals.
//
// On a real Wii, the disc boot code (apploader) and IOS leave a block of values at
// 0x80000000-0x80003200 before jumping to the game: memory sizes, clock speeds, MEM2
// limits, IOS version... The SDK's OSInit reads them. Values follow Dolphin's HLE boot
// (Core/Boot/Boot_BS2Emu.cpp), for a retail NTSC-U Wii running IOS53.
#include "runtime.h"
#include "disc.h"
#include <cstring>
#include <fstream>
#include <vector>

bool LoadDol(const char* path, uint32_t& entry)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> dol((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (dol.size() < 0x100)
    {
        LOG("[boot] cannot read %s", path);
        return false;
    }
    auto u32 = [&](size_t o) { return __builtin_bswap32(*(uint32_t*)&dol[o]); };
    // 7 text and 11 data sections: file offsets at 0x00, addresses at 0x48, sizes at 0x90.
    for (int i = 0; i < 18; i++)
    {
        uint32_t off = u32(0x00 + 4 * i), addr = u32(0x48 + 4 * i), size = u32(0x90 + 4 * i);
        if (!size)
            continue;
        if (off + size > dol.size())
        {
            LOG("[boot] section %d is out of the file", i);
            return false;
        }
        memcpy(g_base + addr, &dol[off], size);
        LOG("[boot] %s%d 0x%08X size 0x%X", i < 7 ? "text" : "data", i < 7 ? i : i - 7, addr, size);
    }
    // The DOL's bss range runs from the first bss section to the last one, so it also
    // covers .sdata and .sdata2 in between: clearing it would wipe them. Guest memory
    // starts zeroed, and the game's __init_data clears the real bss sections itself.
    uint32_t bss = u32(0xD8), bssSize = u32(0xDC);
    entry = u32(0xE0);
    LOG("[boot] bss 0x%08X size 0x%X, entry 0x%08X", bss, bssSize, entry);
    return true;
}

void InitLowMemory()
{
    // Disc header: game ID "RZJE69", disc number, version; Wii disc magic at 0x18.
    static const uint8_t header[8] = { 'R', 'Z', 'J', 'E', '6', '9', 0, 0 };
    memcpy(g_base + 0x80000000, header, sizeof(header));
    Store32(0x80000018, 0x5D1C9EA3);  // Wii disc magic
    Store32(0x8000001C, 0x00000000);  // GameCube disc magic (none)

    Store32(0x80000020, 0x0D15EA5E);  // boot magic: normal boot
    Store32(0x80000024, 0x00000001);  // boot info version
    Store32(0x80000028, kMem1Size);   // physical MEM1 size
    Store32(0x8000002C, 0x00000023);  // console type: retail Wii
    Store32(0x80000030, 0x00000000);  // arena low (OSInit takes it from the linker map)
    Store32(0x80000034, 0x817FEC60);  // arena high
    Store32(0x800000CC, 0x00000000);  // video mode: NTSC
    Store32(0x800000F8, 243000000);   // bus clock
    Store32(0x800000FC, 729000000);   // CPU clock

    Store32(0x80003100, kMem1Size);   // physical MEM1 size
    Store32(0x80003104, kMem1Size);   // simulated MEM1 size
    Store32(0x80003118, kMem2Size);   // physical MEM2 size
    Store32(0x8000311C, kMem2Size);   // simulated MEM2 size
    Store32(0x80003120, 0x93400000);  // end of MEM2 usable by the PPC
    Store32(0x80003124, 0x90000800);  // MEM2 arena low
    Store32(0x80003128, 0x933E0000);  // MEM2 arena high
    Store32(0x80003130, 0x933E0000);  // IOS IPC buffer low
    Store32(0x80003134, 0x93400000);  // IOS IPC buffer high
    Store32(0x80003138, 0x00000011);  // Hollywood revision
    Store32(0x80003140, 0x00350000);  // IOS version: IOS53 (from the disc TMD)
    Store32(0x80003144, 0x00062507);  // IOS build date
    Store32(0x80003158, 0x0000FF16);  // DDR vendor code
    Store32(0x80003180, 0x525A4A45);  // game ID "RZJE"
    Store32(0x80003188, 0x00350000);  // IOS version (copy)

    // The apploader leaves the disc's file table (FST) at the top of MEM1, lowers the
    // arena below it, and records where it is: DVDOpen looks paths up there.
    const std::vector<uint8_t>& fst = DiscFst();
    if (!fst.empty())
    {
        uint32_t fstAddr = (0x817FEC60 - uint32_t(fst.size())) & ~31u;
        memcpy(g_base + fstAddr, fst.data(), fst.size());
        Store32(0x80000034, fstAddr);              // arena high
        Store32(0x80000038, fstAddr);              // FST location
        Store32(0x8000003C, uint32_t(fst.size())); // FST maximum size
        LOG("[boot] FST at 0x%08X (%zu bytes)", fstAddr, fst.size());
    }
    LOG("[boot] low memory globals set (IOS53, MEM1 24 MB, MEM2 64 MB)");
}
