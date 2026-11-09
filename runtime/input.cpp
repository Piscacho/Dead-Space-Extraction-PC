// Wii Remote + Nunchuk from mouse, keyboard and XInput gamepad, at the KPAD level.
//
// The game reads the remotes with KPADRead(chan, samples, max), which fills KPADStatus
// samples (0xF0 bytes each in this SDK). Layout of the fields used, as the game reads them:
//   0x00 hold, 0x04 trigger (pressed this frame), 0x08 release      buttons
//   0x0C acc (Vec3), 0x18 acc_value, 0x1C acc_speed                  accelerometer
//   0x20 pos (Vec2: -1..1, y down), 0x28 vec, 0x30 speed             pointer
//   0x34 horizon (Vec2), 0x48 dist                                   pointer orientation
//   0x5C dev_type (1 = remote + Nunchuk), 0x5D wpad_err (0 ok, -1 none),
//   0x5E dpd_valid_fg (2 = pointer seen), 0x5F data_format
//   0x60 Nunchuk stick (Vec2), 0x68 Nunchuk acc (Vec3), 0x74 acc_value, 0x78 acc_speed
// Keys: mouse/Space/Enter/Tab/1/2/Q/E/arrows/WASD as buttons and stick; F shakes the
// remote, G shakes the Nunchuk, R (held) tilts the remote on its side.
// Channel 0 is a remote with a Nunchuk (the Wii Zapper is exactly that); 1-3 are absent.
#include "runtime.h"
#include "bindings.h"
#include "config.h"
#include "gx.h"
#include "ios.h"
#include <ppc_recomp_shared.h>
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
// WPAD button bits (KPAD hold/trigger words). Nunchuk Z and C share the word.
constexpr uint32_t kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kPlus = 0x0010;
constexpr uint32_t kTwo = 0x0100, kOne = 0x0200, kB = 0x0400, kA = 0x0800, kMinus = 0x1000;
constexpr uint32_t kZ = 0x2000, kC = 0x4000;
constexpr uint32_t kSampleSize = 0xF0;
constexpr uint32_t kDevNunchuk = 1;  // WPAD device types: 0 remote, 1 + Nunchuk, 2 + Classic

uint32_t g_prevHold[2] = {};  // per player
float g_prevPos[2][2] = {};
float g_padPointer[2][2] = {};  // per player: pointer moved by the gamepad's right stick

void StoreF(uint32_t addr, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    Store32(addr, bits);
}

bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

float Deadzone(SHORT v, SHORT dz)
{
    float f = float(v) / 32767.0f;
    float d = float(dz) / 32767.0f;
    if (std::fabs(f) < d)
        return 0.0f;
    return (f - (f > 0 ? d : -d)) / (1.0f - d);
}

struct HostInput
{
    uint32_t buttons = 0;
    float pointer[2] = { 0, 0 };
    bool pointerValid = false;
    float stick[2] = { 0, 0 };  // Nunchuk: x right, y up
    bool shakeRemote = false, shakeNunchuk = false, tilt = false;
};

// Accelerometer (in g) for one KPAD sample. At rest the remote points at the screen with
// gravity along -y; tilted on its side, gravity moves to x (ramped over a few reads so the
// turn does not look like a swing).
//
// The game detects swings (fn_8025E5D0): it integrates the change of acceleration in x and
// y between samples (ignoring samples where z changes by 1 g or more) and, past a
// threshold, records a swing left/right/up/down (fn_8025E1B0 for the remote, fn_8025E270
// for the Nunchuk). So a shake here is a square wave of +-2.5 g switching every 2 reads,
// z untouched. Presses alternate between x (a sideways swing) and y (an up/down swing), so
// either kind the game wants comes with a second press. A press lasts at least kShakeReads.
constexpr int kShakeReads = 16;
struct Motion
{
    int burst = 0;
    uint32_t phase = 0;
    int axis = 1;                 // 0 x, 1 y; flipped on each press (the first one is x)
    float tilt = 0.0f;            // 0 level .. 1 on its side
    float gravity = 0.0f;         // ramps up at start: the game's filter starts from 0 g
    float prev[3] = { 0, 0, 0 };

    void Update(bool pressed, bool tilted, float out[3])
    {
        if (pressed && burst == 0)
        {
            phase = 0;
            axis ^= 1;
        }
        if (pressed)
            burst = std::max(burst, kShakeReads);
        tilt += tilted ? 0.125f : -0.125f;
        tilt = std::fmax(0.0f, std::fmin(1.0f, tilt));
        gravity = std::fmin(1.0f, gravity + 0.005f);  // ~7 s: slow enough for the swing filter
        float base[3] = { tilt * gravity, -(1.0f - tilt) * gravity, 0.0f };
        if (burst > 0)
        {
            burst--;
            float pulse = (phase / 2) % 2 ? -2.5f : 2.5f;
            base[axis] += pulse;
            phase++;
        }
        memcpy(out, base, sizeof(base));
    }
};
Motion g_remoteMotion[2], g_nunchukMotion[2];  // per player

void StoreAcc(uint32_t addr, const float acc[3], float prev[3])
{
    // acc (Vec3), then acc_value (magnitude) and acc_speed (change since the last read).
    float dx = acc[0] - prev[0], dy = acc[1] - prev[1], dz = acc[2] - prev[2];
    for (int k = 0; k < 3; k++)
    {
        uint32_t bits;
        memcpy(&bits, &acc[k], 4);
        Store32(addr + 4 * k, bits);
    }
    float value = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    float speed = std::sqrt(dx * dx + dy * dy + dz * dz);
    uint32_t bits;
    memcpy(&bits, &value, 4);
    Store32(addr + 12, bits);
    memcpy(&bits, &speed, 4);
    Store32(addr + 16, bits);
    memcpy(prev, acc, 3 * sizeof(float));
}

// The bindings from dse.ini (loaded once): virtual keys and gamepad bits per action.
struct BindingTable
{
    std::vector<int> keys[kControlActionCount];
    std::vector<uint32_t> pad[kControlActionCount];
};
const BindingTable& Bindings()
{
    static BindingTable t = [] {
        BindingTable b;
        for (int i = 0; i < kControlActionCount; i++)
        {
            const ControlAction& a = kControlActions[i];
            for (const std::string& n : SplitBinding(ConfigString((std::string("tecla_") + a.id).c_str(), a.keys)))
                if (int vk = KeyFromName(n))
                    b.keys[i].push_back(vk);
            for (const std::string& n : SplitBinding(ConfigString((std::string("mando_") + a.id).c_str(), a.pad)))
                if (uint32_t bit = PadFromName(n))
                    b.pad[i].push_back(bit);
        }
        return b;
    }();
    return t;
}

void Apply(int action, struct HostInput& in);

// Local co-op: jugador2 = 1 in dse.ini adds a second Wii Remote + Nunchuk on channel 1,
// played with the Xbox controller mando_jugador2 (0-3, default 1, i.e. the second one).
// Player 1 keeps keyboard, mouse and controller mando_jugador1 (default 0; -1 none, and none
// when both settings name the same controller).
bool SecondPlayer() { static const bool on = ConfigBool("jugador2", false); return on; }
int PadOf(int player)
{
    static const int p2 = std::clamp(ConfigInt("mando_jugador2", 1), 0, 3);
    static const int p1 = [] {
        int v = ConfigInt("mando_jugador1", 0);
        return (v < 0 || v > 3 || (SecondPlayer() && v == p2)) ? -1 : v;
    }();
    return player == 0 ? p1 : (SecondPlayer() ? p2 : -1);
}

HostInput ReadHost(int player)
{
    HostInput in;
    HWND window = (HWND)VideoGetWindow();
    bool focused = window && GetForegroundWindow() == window;

    if (focused && player == 0)
    {
        // Mouse: pointer over the window's client area.
        POINT p;
        RECT rc;
        if (GetCursorPos(&p) && ScreenToClient(window, &p) && GetClientRect(window, &rc) && rc.right > 0 && rc.bottom > 0)
        {
            // Relative to the image, not the window (black bars around it).
            int x, y, w, h;
            VideoImageRect(rc.right, rc.bottom, x, y, w, h);
            in.pointer[0] = float(p.x - x) / float(std::max(1, w)) * 2.0f - 1.0f;
            // 21:9 / 32:9: the game's own 16:9 screen is the middle of the image.
            in.pointer[0] = std::fmax(-1.0f, std::fmin(1.0f, in.pointer[0] / VideoWideFactor()));
            in.pointer[1] = float(p.y - y) / float(std::max(1, h)) * 2.0f - 1.0f;
            in.pointerValid = in.pointer[0] >= -1.0f && in.pointer[0] <= 1.0f && in.pointer[1] >= -1.0f && in.pointer[1] <= 1.0f;
        }
        // Keyboard and mouse, as dse.ini binds them (bindings.h).
        for (int i = 0; i < kControlActionCount; i++)
            for (int vk : Bindings().keys[i])
                if (Key(vk))
                    Apply(i, in);
    }

    // Gamepad (works without focus, like most PC games with controllers).
    XINPUT_STATE xs{};
    int pad = PadOf(player);
    if (pad >= 0 && XInputGetState(DWORD(pad), &xs) == ERROR_SUCCESS)
    {
        const XINPUT_GAMEPAD& g = xs.Gamepad;
        uint32_t held = g.wButtons | (g.bLeftTrigger > 64 ? 0x10000u : 0u) | (g.bRightTrigger > 64 ? 0x20000u : 0u);
        for (int i = 0; i < kControlActionCount; i++)
            for (uint32_t bit : Bindings().pad[i])
                if (held & bit)
                    Apply(i, in);
        in.stick[0] += Deadzone(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        in.stick[1] += Deadzone(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        // Right stick moves the pointer (about one screen width per second at full tilt).
        float rx = Deadzone(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        float ry = Deadzone(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        if (rx != 0.0f || ry != 0.0f)
        {
            static const float speed = 0.035f * ConfigFloat("sens_mando", 1.0f);
            float* pp = g_padPointer[player];
            pp[0] = std::fmax(-1.0f, std::fmin(1.0f, pp[0] + rx * speed));
            pp[1] = std::fmax(-1.0f, std::fmin(1.0f, pp[1] - ry * speed));
            in.pointer[0] = pp[0];
            in.pointer[1] = pp[1];
            in.pointerValid = true;
        }
        else if (!in.pointerValid)
        {
            in.pointer[0] = g_padPointer[player][0];
            in.pointer[1] = g_padPointer[player][1];
            in.pointerValid = true;
        }
    }

    for (float& s : in.stick)
        s = std::fmax(-1.0f, std::fmin(1.0f, s));
    return in;
}

// What action i (kControlActions order) does to the remote's state.
void Apply(int action, HostInput& in)
{
    static const uint32_t kButtons[] = { kA, kB, kZ, kC, kOne, kTwo, kPlus, kMinus, kUp, kDown, kLeft, kRight };
    std::string id = kControlActions[action].id;
    if (action < int(sizeof(kButtons) / sizeof(kButtons[0])))
        in.buttons |= kButtons[action];
    else if (id == "stick_arriba") in.stick[1] += 1.0f;
    else if (id == "stick_abajo") in.stick[1] -= 1.0f;
    else if (id == "stick_izquierda") in.stick[0] -= 1.0f;
    else if (id == "stick_derecha") in.stick[0] += 1.0f;
    else if (id == "agitar_mando") in.shakeRemote = true;
    else if (id == "agitar_nunchuk") in.shakeNunchuk = true;
    else if (id == "girar") in.tilt = true;
}
} // namespace

// The game learns about remotes through callbacks that the Bluetooth stack would call:
// KPADSetConnectCallback(chan, cb) -> cb(chan, 0) when a remote connects, and
// WPADSetExtensionCallback(chan, cb) -> cb(chan, type) when an extension is plugged in.
// Channel 0 "connects" right after the game registers its callback, with a Nunchuk.
extern "C" PPC_FUNC(__imp__sub_803FFE98);
PPC_FUNC(sub_803FFE98)  // KPADSetConnectCallback(chan, callback)
{
    uint32_t chan = ctx.r3.u32, callback = ctx.r4.u32;
    __imp__sub_803FFE98(ctx, base);
    if ((chan == 0 || (chan == 1 && SecondPlayer())) && callback)
    {
        LOG("[input] remote %u connected (callback 0x%08X)", chan, callback);
        QueueGuestCallback(callback, chan, 0);
    }
}

extern "C" PPC_FUNC(__imp__sub_803EECC8);
PPC_FUNC(sub_803EECC8)  // WPADSetExtensionCallback(chan, callback): returns the previous one
{
    uint32_t chan = ctx.r3.u32, callback = ctx.r4.u32;
    __imp__sub_803EECC8(ctx, base);
    if ((chan == 0 || (chan == 1 && SecondPlayer())) && callback)
    {
        LOG("[input] Nunchuk plugged into remote %u (callback 0x%08X)", chan, callback);
        QueueGuestCallback(callback, chan, kDevNunchuk);
    }
}

// WPADControlMotor(chan, command): the remote's rumble (1 on, 0 off) goes to the Xbox
// controller of that player. vibracion = 0 in dse.ini turns it off. Returns nothing.
PPC_FUNC(sub_803EF0A0)
{
    static const bool enabled = ConfigBool("vibracion", true);
    uint32_t chan = ctx.r3.u32, on = ctx.r4.u32;
    if (!enabled || chan > 1)
        return;
    int pad = PadOf(int(chan));
    if (pad < 0)
        return;
    static uint32_t state[2] = { 2, 2 };
    if (state[chan] == on)
        return;
    state[chan] = on;
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed = on ? 30000 : 0;   // the remote has one small motor: mostly the light one
    v.wRightMotorSpeed = on ? 45000 : 0;
    XInputSetState(DWORD(pad), &v);
}

// KPADRead(chan, samples, max): returns the number of samples written.
PPC_FUNC(sub_803FEF18)
{
    uint32_t chan = ctx.r3.u32, buf = ctx.r4.u32, max = ctx.r5.u32;
    if (!buf || !max)
    {
        ctx.r3.s64 = 0;
        return;
    }
    memset(g_base + buf, 0, kSampleSize);
    if (chan > 1 || (chan == 1 && !SecondPlayer()))
    {
        Store8(buf + 0x5D, uint8_t(-1));  // WPAD_ERR_NO_CONTROLLER
        ctx.r3.s64 = 0;
        return;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        LOG("[input] KPADRead: channel 0 = Wii Remote + Nunchuk (mouse, keyboard, XInput)");
    }

    const int player = int(chan);
    HostInput in = ReadHost(player);
    if (player == 0)  // the test helpers are player 1's
    {
    // DSE_AUTOPRESS=1 (testing): press A for 6 reads out of every 120; =B presses B (the
    // Zapper trigger), =AB alternates A and B.
    // DSE_AUTOPRESS_STOP=<s> stops pressing after that many seconds; DSE_AUTOPOINT_AFTER=<s>
    // applies DSE_AUTOPOINT only from then on.
    static const char* autoPress = getenv("DSE_AUTOPRESS");
    static uint32_t reads = 0;
    static ULONGLONG start = GetTickCount64();
    double seconds = double(GetTickCount64() - start) / 1000.0;
    static double pressStop = getenv("DSE_AUTOPRESS_STOP") ? atof(getenv("DSE_AUTOPRESS_STOP")) : 1e30;
    static double pointAfter = getenv("DSE_AUTOPOINT_AFTER") ? atof(getenv("DSE_AUTOPOINT_AFTER")) : 0.0;
    if (autoPress && seconds < pressStop && (reads++ % 120) < 6)
    {
        bool b = strcmp(autoPress, "B") == 0 || (strcmp(autoPress, "AB") == 0 && (reads / 120) % 2);
        in.buttons |= b ? kB : kA;
    }
    // DSE_AUTOPOINT="x,y" (testing): pointer fixed at that position (-1..1, y down).
    static const char* autoPoint = getenv("DSE_AUTOPOINT");
    if (autoPoint && seconds >= pointAfter)
    {
        float x = 0, y = 0;
        if (sscanf(autoPoint, "%f,%f", &x, &y) == 2)
        {
            in.pointer[0] = x;
            in.pointer[1] = y;
            in.pointerValid = true;
        }
    }
    // DSE_AUTOSCRIPT="t,x,y,BTN;t,x,y,BTN;..." (testing): from second t on, the pointer is at
    // (x, y) and BTN (any of A, B, F shake, G Nunchuk shake; R tilts for the whole step; - none)
// is pulsed like DSE_AUTOPRESS, starting with a press. Later
// steps replace earlier ones.
    static const char* script = getenv("DSE_AUTOSCRIPT");
    if (script)
    {
        static uint32_t pulses = 0;
        static int lastStep = -1;
        int step = -1, index = 0;
        float sx = 0, sy = 0;
        char btn[8] = "-";
        bool found = false;
        for (const char* q = script; *q;)
        {
            double t;
            float x, y;
            char b[8];
            if (sscanf(q, "%lf,%f,%f,%7[^;]", &t, &x, &y, b) == 4 && seconds >= t)
            {
                sx = x; sy = y;
                strcpy(btn, b);
                found = true;
                step = index;
            }
            index++;
            const char* next = strchr(q, ';');
            if (!next)
                break;
            q = next + 1;
        }
        if (step != lastStep)
        {
            lastStep = step;
            pulses = 0;  // each step starts with a press
        }
        if (found)
        {
            in.pointer[0] = sx;
            in.pointer[1] = sy;
            in.pointerValid = true;
            if (strchr(btn, 'R'))
                in.tilt = true;  // held for the whole step
            if ((pulses++ % 120) < 6)
            {
                if (strchr(btn, 'A')) in.buttons |= kA;
                if (strchr(btn, 'B')) in.buttons |= kB;
                if (strchr(btn, 'F')) in.shakeRemote = true;
                if (strchr(btn, 'G')) in.shakeNunchuk = true;
            }
        }
    }
    }
    uint32_t hold = in.buttons;
    Store32(buf + 0x00, hold);
    Store32(buf + 0x04, hold & ~g_prevHold[player]);   // pressed this frame
    Store32(buf + 0x08, g_prevHold[player] & ~hold);   // released this frame
    g_prevHold[player] = hold;

    // Accelerometer at rest, pointing at the screen: gravity along -y.
    float acc[3];
    Motion& rm = g_remoteMotion[player];
    rm.Update(in.shakeRemote, in.tilt, acc);
    StoreAcc(buf + 0x0C, acc, rm.prev);

    // Pointer and its change since the last read.
    StoreF(buf + 0x20, in.pointer[0]);
    StoreF(buf + 0x24, in.pointer[1]);
    float vx = in.pointer[0] - g_prevPos[player][0], vy = in.pointer[1] - g_prevPos[player][1];
    StoreF(buf + 0x28, vx);
    StoreF(buf + 0x2C, vy);
    StoreF(buf + 0x30, std::sqrt(vx * vx + vy * vy));
    g_prevPos[player][0] = in.pointer[0];
    g_prevPos[player][1] = in.pointer[1];
    // Horizon (the sensor bar dots' direction): level, or rolled 90 degrees when tilted.
    StoreF(buf + 0x34, std::cos(rm.tilt * 1.5707963f));
    StoreF(buf + 0x38, std::sin(rm.tilt * 1.5707963f));
    StoreF(buf + 0x48, 1.0f);  // distance to the sensor bar (roughly 1 m)

    Store8(buf + 0x5C, kDevNunchuk);                // remote + Nunchuk
    Store8(buf + 0x5D, 0);                          // no error
    Store8(buf + 0x5E, in.pointerValid ? 2 : 0);    // pointer seen (both sensor bar dots)
    Store8(buf + 0x5F, 0);

    // Nunchuk: stick and accelerometer.
    StoreF(buf + 0x60, in.stick[0]);
    StoreF(buf + 0x64, in.stick[1]);
    float nacc[3];
    Motion& nm = g_nunchukMotion[player];
    nm.Update(in.shakeNunchuk, false, nacc);
    StoreAcc(buf + 0x68, nacc, nm.prev);
    ctx.r3.s64 = 1;
}

// --- Gesture detection, logged (DSE_GESTURELOG=1) -------------------------------------------
// fn_8025E1B0 (remote) and fn_8025E270 (Nunchuk) OR a swing direction into the pad's
// gesture words at r5: bits 0x02000000/0x04000000 (remote x/y), 0x10000000/0x20000000
// (Nunchuk x/y), with the direction in the second word.
extern "C" PPC_FUNC(__imp__sub_8025E1B0);
extern "C" PPC_FUNC(__imp__sub_8025E270);
static void LogGesture(const char* who, PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*))
{
    // Logged once per gesture (the game reports one for up to 30 samples).
    uint32_t r5 = ctx.r5.u32;
    uint32_t before0 = Load32(r5), before1 = Load32(r5 + 4);
    fn(ctx, base);
    uint32_t after0 = Load32(r5), after1 = Load32(r5 + 4);
    static uint32_t last[2][2];
    int slot = who[0] == 'r' ? 0 : 1;
    if ((after0 != before0 || after1 != before1) && (last[slot][0] != after0 || last[slot][1] != after1))
    {
        last[slot][0] = after0;
        last[slot][1] = after1;
        LOG("[gesture] %s swing: 0x%08X 0x%08X", who, after0, after1);
    }
    else if (after0 == before0 && after1 == before1)
        last[slot][0] = last[slot][1] = 0;
}
PPC_FUNC(sub_8025E1B0) { LogGesture("remote", ctx, base, __imp__sub_8025E1B0); }
PPC_FUNC(sub_8025E270) { LogGesture("Nunchuk", ctx, base, __imp__sub_8025E270); }

// DSE_INPUTPROBE=1 (investigation): the input manager behind the swing filter, and the
// method its input loop calls through vtable +0x10 (four floats per remote).
extern "C" PPC_FUNC(__imp__sub_8025E5D0);
PPC_FUNC(sub_8025E5D0)
{
    static const bool probe = getenv("DSE_INPUTPROBE") != nullptr;
    static bool logged = false;
    if (probe && !logged)
    {
        logged = true;
        uint32_t manager = ctx.r3.u32 - 0xE48;
        uint32_t vtable = Load32(manager + 0xE40);
        LOG("[probe] input manager 0x%08X, object at +0xE40 vtable 0x%08X, method +0x10 = 0x%08X", manager, vtable,
            Load32(vtable + 0x10));
    }
    __imp__sub_8025E5D0(ctx, base);
}
