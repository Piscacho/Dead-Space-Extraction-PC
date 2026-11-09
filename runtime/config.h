// Settings: dse.ini next to DSE.exe (written by the launcher), key = value lines.
// An environment variable DSE_<KEY> (key in upper case) overrides the file, for tests.
#pragma once
#include <string>
#include <vector>

void ConfigLoad();
std::string ConfigString(const char* key, const char* fallback = "");
int ConfigInt(const char* key, int fallback);
float ConfigFloat(const char* key, float fallback);
bool ConfigBool(const char* key, bool fallback);
std::string ExeFolder();
// Where the log goes: Recomp/logs in the development layout (exe in project/build), else
// a logs folder next to the exe.
std::string LogFolder();

// dse.ini as lines, so saving keeps comments and keys it does not know (launcher).
struct IniFile
{
    std::vector<std::string> lines;
    std::string path;
    bool Load(const std::string& file);
    std::string Get(const std::string& key, const std::string& fallback) const;
    void Set(const std::string& key, const std::string& value);
    bool Save() const;
};
