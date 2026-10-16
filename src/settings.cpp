#include "settings.h"

#include "log.h"
#include "version.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace {

std::mutex g_mu;
Settings g_settings;
std::wstring g_directory;

int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

void sanitise(Settings& settings) {
    settings.timeout_ms = clamp_int(settings.timeout_ms, 1000, 120000);
    settings.buffer_ms = clamp_int(settings.buffer_ms, 200, 30000);
    std::string ua;
    ua.reserve(settings.user_agent.size());
    for (char c : settings.user_agent) {
        if (c != '\r' && c != '\n' && c != '\0') {
            ua.push_back(c);
        }
    }
    if (ua.size() > 200) {
        ua.resize(200);
    }
    if (ua.empty()) {
        ua = IN_HLS_USER_AGENT;
    }
    settings.user_agent = std::move(ua);
}

int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

bool env_flag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    return value[0] == '1' || value[0] == 'y' || value[0] == 'Y' || value[0] == 't' || value[0] == 'T';
}

std::wstring ini_path(const std::wstring& directory) {
    if (directory.empty()) {
        return L"in_hls.ini";
    }
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + L"in_hls.ini";
    }
    return directory + L"\\in_hls.ini";
}

void apply_log(const Settings& settings) {
    std::wstring path;
    if (!g_directory.empty()) {
        path = (g_directory.back() == L'\\' || g_directory.back() == L'/')
                   ? g_directory + L"in_hls.log"
                   : g_directory + L"\\in_hls.log";
    }
    log_configure(path, settings.log, settings.verbose);
}

}  // namespace

Settings load_settings(const std::wstring& directory) {
    Settings settings;
    const std::wstring path = ini_path(directory);
    settings.log = GetPrivateProfileIntW(L"hls", L"log", 0, path.c_str()) != 0;
    settings.verbose = GetPrivateProfileIntW(L"hls", L"verbose", 0, path.c_str()) != 0;
    settings.timeout_ms = static_cast<int>(GetPrivateProfileIntW(L"hls", L"timeout_ms", 15000, path.c_str()));
    settings.buffer_ms = static_cast<int>(GetPrivateProfileIntW(L"hls", L"buffer_ms", 2000, path.c_str()));
    settings.reconnect = GetPrivateProfileIntW(L"hls", L"reconnect", 1, path.c_str()) != 0;
    wchar_t ua[256] = {};
    GetPrivateProfileStringW(L"hls", L"user_agent", L"WinampHLS/1.0", ua, 256, path.c_str());
    char narrow[256] = {};
    WideCharToMultiByte(CP_ACP, 0, ua, -1, narrow, 256, nullptr, nullptr);
    settings.user_agent = narrow;

    settings.log = env_flag("IN_HLS_LOG", settings.log);
    settings.verbose = env_flag("IN_HLS_VERBOSE", settings.verbose);
    settings.timeout_ms = env_int("IN_HLS_TIMEOUT_MS", settings.timeout_ms);
    settings.buffer_ms = env_int("IN_HLS_BUFFER_MS", settings.buffer_ms);
    sanitise(settings);

    std::lock_guard<std::mutex> lock(g_mu);
    g_directory = directory;
    g_settings = settings;
    apply_log(g_settings);
    return g_settings;
}

void update_settings(const std::wstring& directory, Settings settings) {
    sanitise(settings);
    const std::wstring path = ini_path(directory);
    WritePrivateProfileStringW(L"hls", L"log", settings.log ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"hls", L"verbose", settings.verbose ? L"1" : L"0", path.c_str());
    wchar_t number[32] = {};
    _snwprintf_s(number, _TRUNCATE, L"%d", settings.timeout_ms);
    WritePrivateProfileStringW(L"hls", L"timeout_ms", number, path.c_str());
    _snwprintf_s(number, _TRUNCATE, L"%d", settings.buffer_ms);
    WritePrivateProfileStringW(L"hls", L"buffer_ms", number, path.c_str());
    WritePrivateProfileStringW(L"hls", L"reconnect", settings.reconnect ? L"1" : L"0", path.c_str());
    wchar_t ua[256] = {};
    MultiByteToWideChar(CP_ACP, 0, settings.user_agent.c_str(), -1, ua, 256);
    WritePrivateProfileStringW(L"hls", L"user_agent", ua, path.c_str());

    std::lock_guard<std::mutex> lock(g_mu);
    g_directory = directory;
    g_settings = settings;
    apply_log(g_settings);
}

Settings current_settings() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_settings;
}

void set_current_settings(Settings settings) {
    sanitise(settings);
    std::lock_guard<std::mutex> lock(g_mu);
    g_settings = std::move(settings);
}
