#include "url_util.h"

#include <windows.h>

#include <cctype>
#include <cstring>

namespace {

bool starts_with_scheme(const char* text) {
    if (!text) {
        return false;
    }
    int letters = 0;
    for (const char* p = text; *p; ++p) {
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            ++letters;
            continue;
        }
        if (letters > 0 && p[0] == ':' && p[1] == '/' && p[2] == '/') {
            return true;
        }
        return false;
    }
    return false;
}

const char* path_end(const char* path_start) {
    const char* end = path_start;
    while (*end && *end != '?' && *end != '#') {
        ++end;
    }
    while (end > path_start && end[-1] == '/') {
        --end;
    }
    return end;
}

bool ends_with_extension(const char* start, const char* end, const char* extension) {
    const size_t n = std::strlen(extension);
    if (static_cast<size_t>(end - start) < n) {
        return false;
    }
    return _strnicmp(end - n, extension, n) == 0;
}

const char* path_start_of(const char* text) {
    if (!starts_with_scheme(text)) {
        return text;
    }
    const char* scheme = std::strstr(text, "://");
    if (!scheme) {
        return nullptr;
    }
    return std::strchr(scheme + 3, '/');
}

}  // namespace

bool is_remote_url(const char* text) {
    return starts_with_scheme(text);
}

bool is_hls_url(const char* text) {
    const char* path = text ? path_start_of(text) : nullptr;
    if (!path || !is_remote_url(text)) {
        return false;
    }
    return ends_with_extension(path, path_end(path), ".m3u8");
}

bool claims_hls_name(const char* filename) {
    if (!filename || !filename[0]) {
        return false;
    }
    if (is_hls_url(filename)) {
        return true;
    }
    const char* path = path_start_of(filename);
    if (!path) {
        return false;
    }
    return ends_with_extension(path, path_end(path), ".m3u9");
}

std::string canonical_media_path(const char* winamp_path) {
    if (!winamp_path) {
        return {};
    }
    std::string out(winamp_path);
    const char* path = path_start_of(out.c_str());
    if (!path) {
        return out;
    }
    const size_t begin = static_cast<size_t>(path - out.c_str());
    size_t finish = begin;
    while (finish < out.size() && out[finish] != '?' && out[finish] != '#') {
        ++finish;
    }
    while (finish > begin && out[finish - 1] == '/') {
        --finish;
    }
    if (finish - begin >= 5 && _strnicmp(out.c_str() + finish - 5, ".m3u9", 5) == 0) {
        out[finish - 1] = '8';
    }
    return out;
}

std::string wide_to_utf8(const wchar_t* text) {
    if (!text || !text[0]) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), chars);
    return out;
}

std::string utf8_to_acp(const std::string& utf8) {
    const std::wstring wide = utf8_to_wide(utf8);
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string to_ffmpeg_input(const char* winamp_path) {
    if (!winamp_path) {
        return {};
    }
    if (is_remote_url(winamp_path)) {
        return std::string(winamp_path);
    }
    const int chars = MultiByteToWideChar(CP_ACP, 0, winamp_path, -1, nullptr, 0);
    if (chars <= 1) {
        return std::string(winamp_path);
    }
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_ACP, 0, winamp_path, -1, wide.data(), chars);
    std::string utf8 = wide_to_utf8(wide.c_str());
    if (utf8.empty()) {
        return std::string(winamp_path);
    }
    return utf8;
}
