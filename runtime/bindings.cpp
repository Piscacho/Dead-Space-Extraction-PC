// Key and gamepad button names for dse.ini (see bindings.h).
#include "bindings.h"
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cctype>

namespace
{
struct Named
{
    const char* name;
    int code;
};

const Named kKeys[] = {
    { "ESPACIO", VK_SPACE }, { "INTRO", VK_RETURN }, { "TAB", VK_TAB }, { "ESC", VK_ESCAPE },
    { "RETROCESO", VK_BACK }, { "MAYUS", VK_SHIFT }, { "CTRL", VK_CONTROL }, { "ALT", VK_MENU },
    { "ARRIBA", VK_UP }, { "ABAJO", VK_DOWN }, { "IZQUIERDA", VK_LEFT }, { "DERECHA", VK_RIGHT },
    { "INSERT", VK_INSERT }, { "SUPR", VK_DELETE }, { "INICIO", VK_HOME }, { "FIN", VK_END },
    { "REPAG", VK_PRIOR }, { "AVPAG", VK_NEXT },
    { "MOUSE1", VK_LBUTTON }, { "MOUSE2", VK_RBUTTON }, { "MOUSE3", VK_MBUTTON },
    { "MOUSE4", VK_XBUTTON1 }, { "MOUSE5", VK_XBUTTON2 },
    { "NUM0", VK_NUMPAD0 }, { "NUM1", VK_NUMPAD1 }, { "NUM2", VK_NUMPAD2 }, { "NUM3", VK_NUMPAD3 },
    { "NUM4", VK_NUMPAD4 }, { "NUM5", VK_NUMPAD5 }, { "NUM6", VK_NUMPAD6 }, { "NUM7", VK_NUMPAD7 },
    { "NUM8", VK_NUMPAD8 }, { "NUM9", VK_NUMPAD9 },
    { "COMA", VK_OEM_COMMA }, { "PUNTO", VK_OEM_PERIOD }, { "MENOS", VK_OEM_MINUS }, { "MAS", VK_OEM_PLUS },
};

const Named kPad[] = {
    { "A", XINPUT_GAMEPAD_A }, { "B", XINPUT_GAMEPAD_B }, { "X", XINPUT_GAMEPAD_X }, { "Y", XINPUT_GAMEPAD_Y },
    { "LB", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XINPUT_GAMEPAD_RIGHT_SHOULDER },
    { "LT", 0x10000 }, { "RT", 0x20000 },
    { "START", XINPUT_GAMEPAD_START }, { "BACK", XINPUT_GAMEPAD_BACK },
    { "LS", XINPUT_GAMEPAD_LEFT_THUMB }, { "RS", XINPUT_GAMEPAD_RIGHT_THUMB },
    { "DARRIBA", XINPUT_GAMEPAD_DPAD_UP }, { "DABAJO", XINPUT_GAMEPAD_DPAD_DOWN },
    { "DIZQUIERDA", XINPUT_GAMEPAD_DPAD_LEFT }, { "DDERECHA", XINPUT_GAMEPAD_DPAD_RIGHT },
};

std::string Upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}
} // namespace

int KeyFromName(const std::string& name)
{
    std::string n = Upper(name);
    for (const Named& k : kKeys)
        if (n == k.name)
            return k.code;
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9')))
        return n[0];
    if (n.size() >= 2 && n[0] == 'F')
    {
        int f = atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12)
            return VK_F1 + f - 1;
    }
    return 0;
}

std::string KeyName(int vk)
{
    for (const Named& k : kKeys)
        if (vk == k.code)
            return k.name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F12)
        return "F" + std::to_string(vk - VK_F1 + 1);
    return "";
}

uint32_t PadFromName(const std::string& name)
{
    std::string n = Upper(name);
    for (const Named& k : kPad)
        if (n == k.name)
            return uint32_t(k.code);
    return 0;
}

std::string PadName(uint32_t bit)
{
    for (const Named& k : kPad)
        if (bit == uint32_t(k.code))
            return k.name;
    return "";
}

std::vector<std::string> SplitBinding(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text + ",")
    {
        if (c == ',')
        {
            size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
            if (a != std::string::npos)
                out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        }
        else
            cur += c;
    }
    return out;
}
