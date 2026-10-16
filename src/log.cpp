#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

std::mutex g_mu;
FILE* g_file = nullptr;
bool g_enabled = false;
bool g_verbose = false;
std::wstring g_path;

void write_unlocked(const char* text) {
    if (!g_file || !text) {
        return;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(
        g_file,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds,
        text);
    std::fflush(g_file);
}

}  // namespace

void log_configure(const std::wstring& file_path, bool enabled, bool verbose) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_verbose = verbose;
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_enabled = false;
    g_path = file_path;
    if (!enabled || file_path.empty()) {
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(file_path.c_str(), GetFileExInfoStandard, &info)) {
        const ULONGLONG size =
            (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > 2ull * 1024ull * 1024ull) {
            FILE* truncated = nullptr;
            if (_wfopen_s(&truncated, file_path.c_str(), L"w") == 0 && truncated) {
                std::fputs("log truncated\n", truncated);
                std::fclose(truncated);
            }
        }
    }
    if (_wfopen_s(&g_file, file_path.c_str(), L"a") != 0) {
        wchar_t local_app[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH) > 0) {
            const std::wstring fallback_dir = std::wstring(local_app) + L"\\in_hls";
            CreateDirectoryW(fallback_dir.c_str(), nullptr);
            const std::wstring fallback = fallback_dir + L"\\in_hls.log";
            if (_wfopen_s(&g_file, fallback.c_str(), L"a") != 0) {
                g_file = nullptr;
                return;
            }
        } else {
            g_file = nullptr;
            return;
        }
    }
    g_enabled = true;
    write_unlocked("log opened");
}

void log_shutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) {
        write_unlocked("log closed");
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_enabled = false;
}

void log_write(bool verbose_only, const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_enabled || !g_file) {
        return;
    }
    if (verbose_only && !g_verbose) {
        return;
    }
    char buffer[1200];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    for (char* p = buffer; *p; ++p) {
        if (*p == '\r' || *p == '\n') {
            *p = ' ';
        }
    }
    write_unlocked(buffer);
}
