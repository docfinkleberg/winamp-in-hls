#include "ffmpeg_loader.h"

#include "log.h"

#include <windows.h>
#include <delayimp.h>

#include <cstdio>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
}

namespace {

std::recursive_mutex g_mu;
HINSTANCE g_instance = nullptr;
std::wstring g_module_dir;
std::wstring g_search_dir;
bool g_loaded = false;
bool g_network = false;
std::string g_version;

const wchar_t* kLoadOrder[] = {
    L"avutil-61.dll",
    L"swresample-7.dll",
    L"swscale-10.dll",
    L"avcodec-63.dll",
    L"avformat-63.dll",
};

bool is_required(const wchar_t* name) {
    return wcscmp(name, L"swscale-10.dll") != 0;
}

std::wstring join_path(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) {
        return name;
    }
    if (dir.back() == L'\\' || dir.back() == L'/') {
        return dir + name;
    }
    return dir + L'\\' + name;
}

std::wstring directory_of_module(HINSTANCE instance) {
    wchar_t path[32768];
    const DWORD n = GetModuleFileNameW(instance, path, 32768);
    if (n == 0 || n >= 32767) {
        return {};
    }
    for (DWORD i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i - 1] = L'\0';
            break;
        }
    }
    return path;
}

void ffmpeg_log_callback(void*, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_ERROR || !fmt) {
        return;
    }
    char buffer[800];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    buffer[sizeof(buffer) - 1] = '\0';
    LOG_INFO("ffmpeg: %s", buffer);
}

HMODULE load_named(const std::wstring& directory, const char* ansi_name, std::string& error) {
    if (!ansi_name || !ansi_name[0]) {
        error = "FFmpeg DLL name was empty";
        return nullptr;
    }
    wchar_t wide_name[128] = {};
    MultiByteToWideChar(CP_ACP, 0, ansi_name, -1, wide_name, 128);
    if (HMODULE existing = GetModuleHandleW(wide_name)) {
        return existing;
    }
    const std::wstring full = join_path(directory, wide_name);
    HMODULE module = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        const DWORD code = GetLastError();
        char narrow[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, narrow, 512, nullptr, nullptr);
        error = std::string("Could not load ") + narrow + " (Win32 error " + std::to_string(code) + ")";
    }
    return module;
}

}  // namespace

void note_dll_instance(void* instance) {
    auto module = static_cast<HINSTANCE>(instance);
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    g_instance = module;
    g_module_dir = directory_of_module(module);
    if (g_search_dir.empty()) {
        g_search_dir = g_module_dir;
    }
}

void* loaded_module_instance() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_instance;
}

void set_ffmpeg_search_directory(const std::wstring& directory) {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    g_search_dir = directory;
}

std::wstring ffmpeg_search_directory() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_search_dir.empty() ? g_module_dir : g_search_dir;
}

std::wstring module_directory() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_module_dir;
}

bool load_ffmpeg_dlls(const std::wstring& directory, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (g_loaded) {
        return true;
    }
    if (directory.empty()) {
        error = "FFmpeg search directory is empty";
        return false;
    }
    g_search_dir = directory;

    const DWORD previous = GetDllDirectoryW(0, nullptr);
    std::vector<wchar_t> saved(previous + 1);
    if (previous > 0) {
        GetDllDirectoryW(previous, saved.data());
    }
    SetDllDirectoryW(directory.c_str());

    bool ok = true;
    for (const wchar_t* name : kLoadOrder) {
        const std::wstring full = join_path(directory, name);
        HMODULE module = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) {
            if (!is_required(name)) {
                continue;
            }
            const DWORD code = GetLastError();
            char narrow[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, narrow, 512, nullptr, nullptr);
            error = std::string("Could not load ") + narrow + " (Win32 error " + std::to_string(code) + ")";
            ok = false;
            break;
        }
    }

    if (previous > 0) {
        SetDllDirectoryW(saved.data());
    } else {
        SetDllDirectoryW(nullptr);
    }

    if (!ok) {
        return false;
    }

    g_version = av_version_info() ? av_version_info() : "";
    av_log_set_callback(ffmpeg_log_callback);
    av_log_set_level(AV_LOG_ERROR);
    g_loaded = true;
    return true;
}

std::string ffmpeg_runtime_version() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_version.empty() ? std::string("not loaded") : g_version;
}

void ffmpeg_network_startup() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_loaded || g_network) {
        return;
    }
    avformat_network_init();
    g_network = true;
}

void ffmpeg_network_shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_network) {
        return;
    }
    avformat_network_deinit();
    g_network = false;
}

extern "C" FARPROC WINAPI in_hls_delay_hook(unsigned notify, PDelayLoadInfo info) {
    if (notify != dliNotePreLoadLibrary || !info || !info->szDll) {
        return nullptr;
    }
    std::string error;
    const std::wstring directory = ffmpeg_search_directory();
    HMODULE module = load_named(directory, info->szDll, error);
    if (!module) {
        LOG_INFO("%s", error.c_str());
        return nullptr;
    }
    return reinterpret_cast<FARPROC>(module);
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = in_hls_delay_hook;
