#pragma once

#include <string>

struct Settings {
    bool log = false;
    bool verbose = false;
    int timeout_ms = 15000;
    int buffer_ms = 2000;
    bool reconnect = true;
    std::string user_agent = "WinampHLS/1.1.1";
};

Settings load_settings(const std::wstring& directory);
void update_settings(const std::wstring& directory, Settings settings);
Settings current_settings();
void set_current_settings(Settings settings);
