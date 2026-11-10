// Online play by lockstep (netplay.cpp): both PCs run the game in deterministic mode and
// only exchange each frame's controls. The host is player 1 (Wii Remote 0), the guest is
// player 2 (Wii Remote 1). See NOTAS_SESION11.md.
#pragma once
#include <cstdint>
#include <string>

// One player's controls for one frame, quantized (both PCs must feed the game the same bits).
struct NetInput
{
    uint32_t buttons = 0;
    int16_t pointer[2] = { 0, 0 };  // -1..1 as -32767..32767
    int8_t stick[2] = { 0, 0 };     // -1..1 as -127..127
    uint8_t flags = 0;              // 1 pointer valid, 2 shake remote, 4 shake Nunchuk, 8 tilt
};

// dse.ini: online = no | crear | unirse, online_direccion = ip:puerto (to join),
// online_puerto (to host, default 7350), online_retraso (frames of input delay, default 3).
// Runs before the game boots: connects, checks both run the same build and copies the
// host's saves and settings to the guest. False when there is no online session (off,
// failed or cancelled: the game then runs alone).
bool NetplayStart();
bool NetplayActive();
bool NetplayIsHost();
int NetplayLocalPlayer();  // 0 host, 1 guest
// The host's "wide screen" setting (the game must see the same one on both PCs).
bool NetplayHostWide();
// Called once per video retrace with the local player's controls: returns both players'
// controls for this frame, waiting for the other PC if they have not arrived yet.
void NetplayExchange(uint32_t frame, const NetInput& local, NetInput out[2]);
// Desync check: both PCs report a memory hash every so often; a difference is reported.
void NetplayReportHash(uint32_t frame, uint64_t hash);
// Text for the window title ("Online · host · 45 ms"), empty when offline.
std::wstring NetplayStatusText();
