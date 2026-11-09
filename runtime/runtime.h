// Runtime for the Dead Space: Extraction (Wii) recompilation: guest memory and shared helpers.
#pragma once
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdint>
#include <cstdio>

// Guest memory: a 4 GB window where guest address A lives at g_base + A.
// Wii map: MEM1 0x80000000 (24 MB) and MEM2 0x90000000 (64 MB), with uncached mirrors
// 0xC0000000 and 0xD0000000 that point at the same memory. Hardware registers sit at
// 0xCC000000 (GameCube-era devices) and 0xCD000000 (Hollywood).
extern uint8_t* g_base;

constexpr uint32_t kMem1Base = 0x80000000;
constexpr uint32_t kMem1Size = 0x01800000;
constexpr uint32_t kMem2Base = 0x90000000;
constexpr uint32_t kMem2Size = 0x04000000;

inline void* GuestToHost(uint32_t addr) { return g_base + addr; }
inline uint8_t Load8(uint32_t a) { return g_base[a]; }
inline uint16_t Load16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(g_base + a)); }
inline uint32_t Load32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(g_base + a)); }
inline void Store8(uint32_t a, uint8_t v) { g_base[a] = v; }
inline void Store16(uint32_t a, uint16_t v) { *(uint16_t*)(g_base + a) = __builtin_bswap16(v); }
inline void Store32(uint32_t a, uint32_t v) { *(uint32_t*)(g_base + a) = __builtin_bswap32(v); }

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); std::fflush(stderr); } while (0)

// Recompiled function lookup (indirect calls). Returns nullptr when the address has none.
PPCFunc* FindGuestFunction(uint32_t guest);
// Recompiled function that contains a host code address (for crash reports), or 0.
uint32_t GuestFunctionAtHost(uintptr_t host);

// Boot (boot.cpp): loads main.dol into guest memory and fills the low-memory globals
// that the Wii's boot code and IOS normally leave for the game.
bool LoadDol(const char* path, uint32_t& entry);
void InitLowMemory();

// Threads (threads.cpp): turns the host thread into the first guest thread's fiber.
void InitGuestThreads(PPCContext* mainCtx);
