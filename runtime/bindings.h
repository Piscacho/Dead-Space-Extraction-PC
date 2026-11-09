// Controls: the Wii Remote + Nunchuk actions and what triggers each one on the PC.
// dse.ini holds "tecla_<action> = NAME, NAME" (keyboard keys and mouse buttons) and
// "mando_<action> = NAME, NAME" (Xbox-style gamepad buttons). Shared by the input code and
// the launcher.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ControlAction
{
    const char* id;          // dse.ini suffix
    const wchar_t* label;    // launcher text
    const char* keys;        // default keyboard/mouse binding
    const char* pad;         // default gamepad binding
};

// The order is the launcher's.
inline const ControlAction kControlActions[] = {
    { "a", L"A (acción, usar)", "MOUSE2, ESPACIO", "A" },
    { "b", L"B (gatillo: disparar)", "MOUSE1", "RT" },
    { "z", L"Z (Nunchuk: apuntar/estasis)", "Q", "LT" },
    { "c", L"C (Nunchuk: brillo/recargar)", "E", "LB" },
    { "uno", L"1", "1", "X" },
    { "dos", L"2", "2", "Y" },
    { "mas", L"+ (pausa)", "INTRO", "START" },
    { "menos", L"− (menú/inventario)", "TAB", "BACK" },
    { "arriba", L"Cruceta arriba", "ARRIBA", "DARRIBA" },
    { "abajo", L"Cruceta abajo", "ABAJO", "DABAJO" },
    { "izquierda", L"Cruceta izquierda", "IZQUIERDA", "DIZQUIERDA" },
    { "derecha", L"Cruceta derecha", "DERECHA", "DDERECHA" },
    { "stick_arriba", L"Stick arriba", "W", "" },
    { "stick_abajo", L"Stick abajo", "S", "" },
    { "stick_izquierda", L"Stick izquierda", "A", "" },
    { "stick_derecha", L"Stick derecha", "D", "" },
    // In this game shaking the Nunchuk is the melee strike, shaking the remote charges the
    // Glow Worm (the light), as its own tutorial texts say.
    { "agitar_nunchuk", L"Golpe cuerpo a cuerpo (agitar Nunchuk)", "F", "RB" },
    { "agitar_mando", L"Cargar la luz Glow Worm (agitar mando)", "G", "LS" },
    { "girar", L"Girar el mando de lado", "R", "RS" },
};
constexpr int kControlActionCount = int(sizeof(kControlActions) / sizeof(kControlActions[0]));

// Keyboard and mouse: virtual-key code for a name ("ESPACIO", "W", "MOUSE1", "F5"...), 0 if
// unknown. And the name of a virtual-key code.
int KeyFromName(const std::string& name);
std::string KeyName(int vk);
// Gamepad: XINPUT_GAMEPAD_* button bit, or 0x10000 / 0x20000 for the left / right trigger.
uint32_t PadFromName(const std::string& name);
std::string PadName(uint32_t bit);
// "A, B" -> {"A", "B"}.
std::vector<std::string> SplitBinding(const std::string& text);
