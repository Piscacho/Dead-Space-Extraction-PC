// AX DSP replacement: the Wii's audio DSP runs Nintendo's AX microcode, which mixes the
// voices AX describes in parameter blocks (PBs). Here the CPU side of that exchange is
// answered on the PC:
//   - DSPAssertTask(task): the DSP would resume the task and interrupt with "resumed";
//     the task's resume callback (DSPTaskInfo +0x2C) runs at once. For AX that callback
//     builds the next audio frame (__AXOutNewFrame).
//   - DSPSendMailToDSP: AX mails 0xBABExxxx and then the address of a command list;
//     the list is run here, as the microcode would.
//   - DSPCheckMailToDSP: the mailbox is always free.
//
// The command list and PB processing follow Dolphin's AXWii HLE (Source/Core/Core/HW/
// DSPHLE/UCodes/AXWii.cpp, AXVoice.h, DSPAccelerator.cpp). This game's AX uses the newer
// list (0x04 = process, 0x0B = output with volume) and PBs laid out contiguously in
// memory without the "updates" field, so PB offsets from 116 on are 10 bytes lower in
// memory than in Dolphin's AXPBWii struct.
//
// One frame is 3 ms: 96 samples at 32 kHz. Output: interleaved right/left big-endian s16.
//
// Aux buses (effects): each voice also sends to buses A, B and C (L/R/S) with its own
// volumes. MIX_AUXx and UPL_AUXx_MIX_LRSC hand those buses to the game, whose CPU code runs
// the effects (reverb, chorus...), and mix back what it returned (the previous frame's).
// Resampling: 4-point cubic (Catmull-Rom) for the "polyphase" type (not the console's own
// coefficient table), linear, or nearest, as the PB asks.
// Not done: biquad, the compressor, initial time delay, the Wii Remote speaker.
#include "runtime.h"
#include "audio.h"
#include "config.h"
#include "ios.h"
#include <ppc_recomp_shared.h>
#include <algorithm>
#include <cstring>

namespace
{
constexpr int kFrame = 96;

// PB fields as byte offsets in guest memory.
enum Pb : uint32_t
{
    kNextHi = 0, kNextLo = 2, kSrcType = 8, kMixerCtrlHi = 12, kMixerCtrlLo = 14, kRunning = 16, kIsStream = 18,
    // Mixer volumes (volume, delta): main, aux A, aux B, aux C for L and R, then the four S.
    kMainL = 20, kMainR = 24, kAuxAL = 28, kAuxAR = 32, kAuxBL = 36, kAuxBR = 40, kAuxCL = 44, kAuxCR = 48,
    kMainS = 52, kAuxAS = 56, kAuxBS = 60, kAuxCS = 64,
    kVolEnv = 116 - 10,                                    // cur_volume, cur_volume_delta
    kLooping = 120 - 10, kFormat = 122 - 10,
    kLoopHi = 124 - 10, kLoopLo = 126 - 10, kEndHi = 128 - 10, kEndLo = 130 - 10, kCurHi = 132 - 10, kCurLo = 134 - 10,
    kCoefs = 136 - 10, kGain = 168 - 10, kPredScale = 170 - 10, kYn1 = 172 - 10, kYn2 = 174 - 10,
    kRatioHi = 176 - 10, kRatioLo = 178 - 10, kFrac = 180 - 10, kLast = 182 - 10,
    kLoopPred = 190 - 10, kLoopYn1 = 192 - 10, kLoopYn2 = 194 - 10,
    kLpfOn = 196 - 10, kLpfYn1 = 198 - 10, kLpfA0 = 200 - 10, kLpfB0 = 202 - 10,
};

int32_t g_mainL[kFrame], g_mainR[kFrame], g_mainS[kFrame];
// Aux buses: [bus A/B/C][L/R/S].
int32_t g_aux[3][3][kFrame];
uint16_t g_lastAuxVolume[2] = { 0, 0 };
uint32_t g_auxCommands = 0;  // MIX_AUX / UPL_AUX seen (log)
bool g_expectList = false;
uint32_t g_lists = 0, g_voicesLastFrame = 0;

uint32_t Hilo(uint32_t pb, uint32_t hi) { return uint32_t(Load16(pb + hi)) << 16 | Load16(pb + hi + 2); }
void StoreHilo(uint32_t pb, uint32_t hi, uint32_t v) { Store16(pb + hi, uint16_t(v >> 16)); Store16(pb + hi + 2, uint16_t(v)); }
int16_t S16(uint32_t a) { return int16_t(Load16(a)); }
int16_t Clamp16(int32_t v) { return int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
uint32_t Mem(uint32_t physical) { return (physical & 0x1FFFFFFF) | 0x80000000; }

// The DSP's sample "accelerator": reads and decodes one sample at a time.
struct Accelerator
{
    uint32_t pb, start, end, cur, format;
    uint32_t pred;
    int32_t yn1, yn2;
    uint32_t gain;
    int16_t coefs[16];
    bool stopped = false;

    int16_t Read()
    {
        if (stopped)
            return 0;
        uint32_t size = format & 3, decode = (format >> 2) & 3, gainScale = (format >> 4) & 3;
        int32_t value;
        uint32_t step = 2;
        if (decode == 0)  // ADPCM, nibble addresses
        {
            uint8_t byte = Load8(Mem(cur >> 1));
            int32_t n = (cur & 1) ? (byte & 15) : (byte >> 4);
            if (n >= 8)
                n -= 16;
            uint32_t ci = (pred >> 4) & 7;
            int32_t c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
            value = Clamp16((n << (pred & 15)) + ((0x400 + c1 * yn1 + c2 * yn2) >> 11));
            yn2 = yn1;
            yn1 = value;
            cur++;
            if ((end & 15) == 0 && cur == end)
                cur = start + 1;
            else if ((end & 15) == 1 && cur == end - 1)
                cur = start;
            else if ((cur & 15) == 0)
            {
                pred = Load8(Mem(cur >> 1));
                cur += 2;
                step += 2;
            }
        }
        else  // PCM: 8-bit byte addresses, 16-bit sample addresses
        {
            int32_t raw = size == 1 ? int32_t(int8_t(Load8(Mem(cur)))) : int32_t(S16(Mem(cur * 2)));
            uint32_t shift = gainScale == 1 ? 0 : gainScale == 2 ? 16 : 11;
            int32_t c1 = coefs[0], c2 = coefs[1];
            int32_t g = gain ? int32_t(gain) : (shift == 0 ? (size == 1 ? 256 : 1) : (1 << shift));
            value = Clamp16(((g * raw) >> shift) + ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift));
            yn2 = yn1;
            yn1 = value;
            cur++;
        }
        if (cur == end + step - 1)
        {
            cur = start;
            stopped = true;
            OnEnd();
        }
        return int16_t(value);
    }

    // End of the sample: loop (restore loop predictor state) or stop the voice.
    void OnEnd()
    {
        if (Load16(pb + kLooping))
        {
            pred = Load16(pb + kLoopPred);
            if (Load16(pb + kIsStream) != 1)
            {
                yn1 = S16(pb + kLoopYn1);
                yn2 = S16(pb + kLoopYn2);
            }
            stopped = false;
        }
        else
            Store16(pb + kRunning, 0);
    }
};

void MixInto(int32_t* out, const int16_t* in, uint32_t pb, uint32_t vd, bool ramp)
{
    uint16_t volume = Load16(pb + vd);
    uint16_t delta = ramp ? Load16(pb + vd + 2) : 0;
    for (int i = 0; i < kFrame; i++)
    {
        out[i] += Clamp16(int32_t((int64_t(in[i]) * volume) >> 15));
        volume = uint16_t(volume + delta);
    }
    Store16(pb + vd, volume);
}

void ProcessVoice(uint32_t pb)
{
    if (Load16(pb + kRunning) != 1)
        return;
    g_voicesLastFrame++;
    static int logged = 0;
    if (logged < 6)
    {
        logged++;
        LOG("[ax] voice PB 0x%08X: format 0x%X loop %u cur 0x%08X end 0x%08X loopstart 0x%08X ratio 0x%08X vol %04X "
            "mixctrl 0x%08X mainL %04X mainR %04X gain %04X pred %02X",
            pb, Load16(pb + kFormat), Load16(pb + kLooping), Hilo(pb, kCurHi), Hilo(pb, kEndHi), Hilo(pb, kLoopHi),
            Hilo(pb, kRatioHi), Load16(pb + kVolEnv), Hilo(pb, kMixerCtrlHi), Load16(pb + kMainL), Load16(pb + kMainR),
            Load16(pb + kGain), Load16(pb + kPredScale));
    }

    Accelerator acc{};
    acc.pb = pb;
    acc.start = Hilo(pb, kLoopHi);
    acc.end = Hilo(pb, kEndHi);
    acc.cur = Hilo(pb, kCurHi);
    acc.format = Load16(pb + kFormat);
    acc.pred = Load16(pb + kPredScale);
    acc.yn1 = S16(pb + kYn1);
    acc.yn2 = S16(pb + kYn2);
    acc.gain = Load16(pb + kGain);
    for (int i = 0; i < 16; i++)
        acc.coefs[i] = S16(pb + kCoefs + 2 * i);

    // Resampling, 16.16 ratio. last_samples[0..3] keep the last four input samples.
    uint32_t ratio = Hilo(pb, kRatioHi);
    uint32_t pos = Load16(pb + kFrac);
    uint16_t srcType = Load16(pb + kSrcType);
    int32_t h[4] = { S16(pb + kLast), S16(pb + kLast + 2), S16(pb + kLast + 4), S16(pb + kLast + 6) };
    int16_t samples[kFrame];
    for (int i = 0; i < kFrame; i++)
    {
        pos += ratio;
        while (pos >= 0x10000)
        {
            h[0] = h[1];
            h[1] = h[2];
            h[2] = h[3];
            h[3] = acc.Read();
            pos -= 0x10000;
        }
        float t = float(pos & 0xFFFF) / 65536.0f;
        if (srcType == 2)  // nearest
            samples[i] = int16_t(h[3]);
        else if (srcType == 1)  // linear between the last two
            samples[i] = Clamp16(h[2] + int32_t(float(h[3] - h[2]) * t));
        else
        {
            // Catmull-Rom between h1 and h2, with h0 and h3 around them (one sample more
            // of delay than the linear case: all four are already read).
            float p0 = float(h[0]), p1 = float(h[1]), p2 = float(h[2]), p3 = float(h[3]);
            float v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                t * (3.0f * (p1 - p2) + p3 - p0)));
            samples[i] = Clamp16(int32_t(v));
        }
    }
    Store16(pb + kFrac, uint16_t(pos & 0xFFFF));
    for (int k = 0; k < 4; k++)
        Store16(pb + kLast + 2 * k, uint16_t(h[k]));
    StoreHilo(pb, kCurHi, acc.cur);
    Store16(pb + kYn1, uint16_t(acc.yn1));
    Store16(pb + kYn2, uint16_t(acc.yn2));
    Store16(pb + kPredScale, uint16_t(acc.pred));

    // Volume envelope (unsigned volume, signed per-sample delta).
    int16_t vol = S16(pb + kVolEnv), volDelta = S16(pb + kVolEnv + 2);
    for (int i = 0; i < kFrame; i++)
    {
        samples[i] = Clamp16((int32_t(samples[i]) * int32_t(uint16_t(vol))) >> 15);
        vol = int16_t(vol + volDelta);
    }
    Store16(pb + kVolEnv, uint16_t(vol));

    // One-pole low-pass filter.
    if (Load16(pb + kLpfOn))
    {
        int32_t yn1 = S16(pb + kLpfYn1), a0 = Load16(pb + kLpfA0), b0 = S16(pb + kLpfB0);
        for (int i = 0; i < kFrame; i++)
            yn1 = samples[i] = Clamp16((a0 * samples[i] + b0 * yn1) >> 15);
        Store16(pb + kLpfYn1, uint16_t(yn1));
    }

    // Buses. mixer_control (Dolphin AXWii): per bus L, R, L/R ramp, S, S ramp bits at
    // 0x1 (main), 0x100 (aux A), 0x10000 (aux B), 0x200000 (aux C).
    uint32_t ctrl = Hilo(pb, kMixerCtrlHi);
    struct Bus { int32_t* l; int32_t* r; int32_t* s; uint32_t vl, vr, vs, shift; };
    const Bus buses[4] = {
        { g_mainL, g_mainR, g_mainS, kMainL, kMainR, kMainS, 0 },
        { g_aux[0][0], g_aux[0][1], g_aux[0][2], kAuxAL, kAuxAR, kAuxAS, 8 },
        { g_aux[1][0], g_aux[1][1], g_aux[1][2], kAuxBL, kAuxBR, kAuxBS, 16 },
        { g_aux[2][0], g_aux[2][1], g_aux[2][2], kAuxCL, kAuxCR, kAuxCS, 21 },
    };
    for (const Bus& b : buses)
    {
        uint32_t bits = ctrl >> b.shift;
        bool lrRamp = bits & 4, sRamp = bits & 0x10;
        if (bits & 1)
            MixInto(b.l, samples, pb, b.vl, lrRamp);
        if (bits & 2)
            MixInto(b.r, samples, pb, b.vr, lrRamp);
        if (bits & 8)
            MixInto(b.s, samples, pb, b.vs, sRamp);
    }
}

void WriteBlock32(uint32_t addr, const int32_t* in)
{
    for (int i = 0; i < kFrame; i++)
        Store32(addr + i * 4, uint32_t(in[i]));
}

// MIX_AUXx: hand bus `bus` (L, R, S) to the game at `write` (if any), then mix what the
// game left at `read` (its effects output, L R S) into the main buses at `volume`.
void MixAux(int bus, uint32_t write, uint32_t read, uint16_t volume)
{
    if (write != 0x80000000)
        for (int c = 0; c < 3; c++)
            WriteBlock32(write + c * kFrame * 4, g_aux[bus][c]);
    static const bool effects = ConfigBool("efectos_audio", true);
    if (read == 0x80000000 || !effects)
        return;
    int32_t* mains[3] = { g_mainL, g_mainR, g_mainS };
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < kFrame; i++)
            mains[c][i] += int32_t((int64_t(int32_t(Load32(read + (c * kFrame + i) * 4))) * volume) >> 15);
}

// UPL_AUXx_MIX_LRSC: upload bus A or B (L, R, S at a[0]) and C (R for A, S for B, at a[1]),
// then mix the four returned blocks (a[2..5]) into main L, R, S and aux C L, with the
// volume ramping from the last one.
void UploadAuxMixLrsc(int bus, const uint32_t a[6], uint16_t volume)
{
    for (int c = 0; c < 3; c++)
        WriteBlock32(a[0] + c * kFrame * 4, g_aux[bus][c]);
    WriteBlock32(a[1], bus ? g_aux[2][2] : g_aux[2][1]);
    int32_t ramp[kFrame];
    int32_t from = g_lastAuxVolume[bus];
    for (int i = 0; i < kFrame; i++)
        ramp[i] = from + (int32_t(volume) - from) * i / kFrame;
    g_lastAuxVolume[bus] = volume;
    static const bool effects = ConfigBool("efectos_audio", true);
    if (!effects)
        return;
    int32_t* dest[4] = { g_mainL, g_mainR, g_mainS, g_aux[2][0] };
    for (int d = 0; d < 4; d++)
        for (int i = 0; i < kFrame; i++)
            dest[d][i] += int32_t((int64_t(int32_t(Load32(a[2 + d] + i * 4))) * ramp[i]) >> 15);
}
} // namespace

void AxRunCommandList(uint32_t address)
{
    g_lists++;
    g_voicesLastFrame = 0;
    uint32_t p = address;
    auto arg = [&]() { uint16_t v = Load16(p); p += 2; return v; };
    auto addr = [&]() { uint32_t hi = arg(); return Mem(hi << 16 | arg()); };
    for (int n = 0; n < 64; n++)
    {
        uint16_t cmd = arg();
        switch (cmd)
        {
        case 0x00:  // SETUP: new frame
            addr();
            memset(g_mainL, 0, sizeof(g_mainL));
            memset(g_mainR, 0, sizeof(g_mainR));
            memset(g_mainS, 0, sizeof(g_mainS));
            memset(g_aux, 0, sizeof(g_aux));
            break;
        case 0x01:  // ADD_TO_LR
        case 0x02:  // SUB_TO_LR
        {
            uint32_t a = addr();
            for (int i = 0; i < kFrame; i++)
            {
                int32_t v = int32_t(Load32(a + i * 4));
                g_mainL[i] += cmd == 1 ? v : -v;
                g_mainR[i] += cmd == 1 ? v : -v;
            }
            break;
        }
        case 0x03:  // ADD_SUB_TO_LR
        {
            uint32_t a = addr();
            for (int i = 0; i < kFrame; i++)
            {
                g_mainL[i] += int32_t(Load32(a + i * 4));
                g_mainR[i] -= int32_t(Load32(a + 384 + i * 4));
            }
            break;
        }
        case 0x04:  // PROCESS: walk the PB list
        {
            uint32_t pb = addr();
            for (int guard = 0; pb != 0x80000000 && guard < 512; guard++)
            {
                ProcessVoice(pb);
                pb = Mem(Hilo(pb, kNextHi));
            }
            break;
        }
        case 0x05: case 0x06: case 0x07:  // MIX_AUXA/B/C(volume, write address, read address)
        {
            uint16_t volume = arg();
            uint32_t write = addr(), read = addr();
            MixAux(cmd - 0x05, write, read, volume);
            g_auxCommands++;
            break;
        }
        case 0x08: case 0x09:  // UPL_AUXA/B_MIX_LRSC(volume, 6 addresses)
        {
            uint16_t volume = arg();
            uint32_t a[6];
            for (uint32_t& x : a)
                x = addr();
            UploadAuxMixLrsc(cmd - 0x08, a, volume);
            g_auxCommands++;
            break;
        }
        case 0x0A:  // COMPRESSOR
            p += 8;
            break;
        case 0x0B:  // OUTPUT(volume, surround address, L/R address)
        case 0x0C:  // OUTPUT_DPL2
        {
            uint16_t volume = arg();
            uint32_t surround = addr(), out = addr();
            WriteBlock32(surround, g_mainS);
            for (int i = 0; i < kFrame; i++)
            {
                int16_t l = Clamp16(int32_t((int64_t(g_mainL[i]) * volume) >> 15));
                int16_t r = Clamp16(int32_t((int64_t(g_mainR[i]) * volume) >> 15));
                Store16(out + i * 4, uint16_t(r));
                Store16(out + i * 4 + 2, uint16_t(l));
            }
            break;
        }
        case 0x0D:  // WM_OUTPUT: Wii Remote speaker, not played
            p += 16;
            break;
        case 0x0E:  // END
        default:
            if (cmd != 0x0E && g_lists < 4)
                LOG("[ax] unknown command 0x%04X in list 0x%08X", cmd, address);
            return;
        }
    }
}

PPC_FUNC(sub_803E1830)  // DSPSendMailToDSP(mail)
{
    uint32_t mail = ctx.r3.u32;
    if (g_expectList)
    {
        g_expectList = false;
        AxRunCommandList(Mem(mail));
        static uint32_t frames = 0;
        if (++frames % 3000 == 0)
            LOG("[ax] %u frames mixed, %u voices playing, %u aux bus commands", frames, g_voicesLastFrame, g_auxCommands);
        return;
    }
    if ((mail >> 16) == 0xBABE)
        g_expectList = true;
}

PPC_FUNC(sub_803E17FC)  // DSPCheckMailToDSP: the DSP has always taken the mail
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(sub_803E197C)  // DSPAssertTask(task): the DSP resumes it at once
{
    uint32_t task = ctx.r3.u32;
    uint32_t resume = Load32(task + 0x2C);
    if (resume)
        CallGuest(ctx, base, resume, task, 0);
}
