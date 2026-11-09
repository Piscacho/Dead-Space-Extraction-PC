// Force-included into the generated PPC code. Declares the helpers the recompiler emits
// calls to; the runtime implements the non-inline ones.
#pragma once
#include <cstdint>
struct PPCContext;

// Indirect calls (bctrl, blrl) go through a runtime check that reports a target with no
// recompiled function instead of jumping to 0.
void GuestCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x) GuestCallIndirect(ctx, base, (x))

// Division that never faults: PowerPC leaves the result undefined, x86 would crash.
static inline int32_t ppc_divw(int32_t a, int32_t b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? 0 : a / b; }
static inline uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b == 0 ? 0 : a / b; }

// lwarx/stwcx. reservation (only used by the SDK's OS code on the single-core Wii).
void ppc_reserve(uint32_t ea);
bool ppc_stwcx32(uint8_t* base, uint32_t ea, uint32_t expectedRaw, uint32_t newRaw);

// mftb: Wii time base, which runs at 1/4 of the 243 MHz bus clock (60.75 MHz).
uint64_t ppc_mftb();

// GX write-gather pipe: the game sends graphics commands by storing to 0xCC008000, often
// from code inlined into game functions. Those stores go to the runtime's command
// processor (gx.cpp); every other store is a plain big-endian memory write.
void GxPipeWrite8(uint8_t v);
void GxPipeWrite16(uint16_t v);
void GxPipeWrite32(uint32_t v);
#define PPC_IS_GX_PIPE(a) __builtin_expect(((a) & 0xFFFFFFF8u) == 0xCC008000u, 0)
#define PPC_STORE_U8(x, y) do { uint32_t a_ = uint32_t(x); uint8_t v_ = uint8_t(y); \
    if (PPC_IS_GX_PIPE(a_)) GxPipeWrite8(v_); else *(volatile uint8_t*)(base + a_) = v_; } while (0)
#define PPC_STORE_U16(x, y) do { uint32_t a_ = uint32_t(x); uint16_t v_ = uint16_t(y); \
    if (PPC_IS_GX_PIPE(a_)) GxPipeWrite16(v_); else *(volatile uint16_t*)(base + a_) = __builtin_bswap16(v_); } while (0)
#define PPC_STORE_U32(x, y) do { uint32_t a_ = uint32_t(x); uint32_t v_ = uint32_t(y); \
    if (PPC_IS_GX_PIPE(a_)) GxPipeWrite32(v_); else *(volatile uint32_t*)(base + a_) = __builtin_bswap32(v_); } while (0)
#define PPC_STORE_U64(x, y) do { uint32_t a_ = uint32_t(x); uint64_t v_ = uint64_t(y); \
    if (PPC_IS_GX_PIPE(a_)) { GxPipeWrite32(uint32_t(v_ >> 32)); GxPipeWrite32(uint32_t(v_)); } \
    else *(volatile uint64_t*)(base + a_) = __builtin_bswap64(v_); } while (0)

// tw/twi and sc: implemented by the runtime (report and stop).
void ppc_trap(uint32_t address);
void ppc_syscall(PPCContext& ctx, uint8_t* base, uint32_t address);
#define PPC_TRAP(address) ppc_trap(address)
#define PPC_SYSCALL(address) ppc_syscall(ctx, base, address)
