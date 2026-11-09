// dse.ini: the port's settings (see config.h).
#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>

namespace
{
std::map<std::string, std::string> g_values;
bool g_loaded = false;

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "key = value" (not a comment or a [section]): key and value.
bool ParseLine(const std::string& line, std::string& key, std::string& value)
{
    std::string t = Trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[')
        return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos)
        return false;
    key = Trim(t.substr(0, eq));
    value = Trim(t.substr(eq + 1));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return !key.empty();
}
} // namespace

std::string LogFolder()
{
    char full[MAX_PATH];
    std::string dev = ExeFolder() + "/../../logs";
    if (GetFullPathNameA(dev.c_str(), MAX_PATH, full, nullptr))
        dev = full;  // plain backslashes (for Explorer)
    DWORD attr = GetFileAttributesA(dev.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return dev;
    std::string local = ExeFolder() + "\\logs";
    CreateDirectoryA(local.c_str(), nullptr);
    return local;
}

std::string ExeFolder()
{
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s = p;
    size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? "." : s.substr(0, k);
}

void ConfigLoad()
{
    g_values.clear();
    std::ifstream f(ExeFolder() + "\\dse.ini");
    std::string line, key, value;
    while (std::getline(f, line))
        if (ParseLine(line, key, value))
            g_values[key] = value;
    g_loaded = true;
}

std::string ConfigString(const char* key, const char* fallback)
{
    if (!g_loaded)
        ConfigLoad();
    std::string env = "DSE_";
    for (const char* c = key; *c; c++)
        env += char(std::toupper((unsigned char)*c));
    if (const char* v = getenv(env.c_str()))
        return v;
    auto it = g_values.find(key);
    return it != g_values.end() ? it->second : fallback;
}

int ConfigInt(const char* key, int fallback)
{
    std::string v = ConfigString(key, "");
    return v.empty() ? fallback : atoi(v.c_str());
}

float ConfigFloat(const char* key, float fallback)
{
    std::string v = ConfigString(key, "");
    return v.empty() ? fallback : float(atof(v.c_str()));
}

bool ConfigBool(const char* key, bool fallback)
{
    std::string v = ConfigString(key, "");
    if (v.empty())
        return fallback;
    return v == "1" || v == "si" || v == "sí" || v == "true" || v == "on";
}

// --- IniFile ---------------------------------------------------------------------------

bool IniFile::Load(const std::string& file)
{
    path = file;
    lines.clear();
    std::ifstream f(file);
    if (!f)
        return false;
    std::string line;
    while (std::getline(f, line))
        lines.push_back(line);
    return true;
}

std::string IniFile::Get(const std::string& key, const std::string& fallback) const
{
    std::string k, v;
    for (const std::string& line : lines)
        if (ParseLine(line, k, v) && k == key)
            return v;
    return fallback;
}

void IniFile::Set(const std::string& key, const std::string& value)
{
    std::string k, v;
    for (std::string& line : lines)
        if (ParseLine(line, k, v) && k == key)
        {
            line = key + " = " + value;
            return;
        }
    lines.push_back(key + " = " + value);
}

bool IniFile::Save() const
{
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f)
            return false;
        for (const std::string& line : lines)
            f << line << "\n";
    }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
