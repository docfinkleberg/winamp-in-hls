#include "playlist_file.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

bool ends_with_ci(const std::wstring& text, const wchar_t* ext) {
    const size_t n = wcslen(ext);
    if (text.size() < n) {
        return false;
    }
    return _wcsnicmp(text.c_str() + text.size() - n, ext, n) == 0;
}

std::wstring path_without_query(const std::wstring& path) {
    const size_t cut = path.find_first_of(L"?#");
    return cut == std::wstring::npos ? path : path.substr(0, cut);
}

void trim_inplace(std::string& line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        line.erase(0, start);
    }
}

void rewrite_m3u8_to_m3u9(std::string& url) {
    for (size_t i = 0; i + 5 <= url.size(); ++i) {
        if (url[i] == '.' && _strnicmp(url.c_str() + i, ".m3u8", 5) == 0) {
            const char next = (i + 5 < url.size()) ? url[i + 5] : '\0';
            if (next == '\0' || next == '?' || next == '#' || next == ' ' || next == '\t') {
                url[i + 4] = '9';
            }
        }
    }
}

std::wstring to_wide_acp_or_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    // Prefer UTF-8 (playlist files are often UTF-8); fall back to ACP.
    int chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    if (chars <= 0) {
        code_page = CP_ACP;
        chars = MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    return out;
}

bool looks_like_url_or_path(const std::string& line) {
    if (line.empty() || line[0] == '#') {
        return false;
    }
    if (line.size() >= 7 && _strnicmp(line.c_str(), "http://", 7) == 0) {
        return true;
    }
    if (line.size() >= 8 && _strnicmp(line.c_str(), "https://", 8) == 0) {
        return true;
    }
    // Relative / local paths ending in known audio playlist extensions.
    return true;
}

}  // namespace

bool is_external_playlist_path(const wchar_t* path) {
    if (!path || !path[0]) {
        return false;
    }
    std::wstring clean = path_without_query(path);
    while (!clean.empty() && (clean.back() == L'/' || clean.back() == L'\\')) {
        clean.pop_back();
    }
    // Remote .m3u8 playlists are streams for us (via .m3u9). Local .m3u / .pls
    // station lists must be expanded by us with titles preserved.
    if (ends_with_ci(clean, L".m3u") && !ends_with_ci(clean, L".m3u8") && !ends_with_ci(clean, L".m3u9")) {
        return true;
    }
    if (ends_with_ci(clean, L".pls")) {
        return true;
    }
    return false;
}

bool parse_m3u_bytes(const char* data, size_t size, std::vector<PlaylistEntry>& out) {
    out.clear();
    if (!data || size == 0) {
        return false;
    }
    size_t offset = 0;
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        offset = 3;
    }

    std::string pending_title;
    std::string chunk(data + offset, data + size);
    std::istringstream stream(chunk);
    std::string line;
    while (std::getline(stream, line)) {
        trim_inplace(line);
        if (line.empty()) {
            continue;
        }
        if (line.size() >= 8 && _strnicmp(line.c_str(), "#EXTINF:", 8) == 0) {
            pending_title.clear();
            const size_t comma = line.find(',');
            if (comma != std::string::npos && comma + 1 < line.size()) {
                pending_title = line.substr(comma + 1);
                trim_inplace(pending_title);
            }
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        if (!looks_like_url_or_path(line)) {
            pending_title.clear();
            continue;
        }
        rewrite_m3u8_to_m3u9(line);
        PlaylistEntry entry;
        entry.url = to_wide_acp_or_utf8(line);
        entry.title = to_wide_acp_or_utf8(pending_title);
        pending_title.clear();
        if (!entry.url.empty()) {
            out.push_back(std::move(entry));
        }
    }
    return !out.empty();
}

bool load_playlist_file(const wchar_t* path, std::vector<PlaylistEntry>& out) {
    out.clear();
    if (!path || !path[0]) {
        return false;
    }

    const std::wstring clean = path_without_query(path);
    if (ends_with_ci(clean, L".pls")) {
        // Minimal PLS support: FileN= / TitleN=
        std::ifstream file(clean, std::ios::binary);
        if (!file) {
            return false;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        std::vector<std::string> files(64);
        std::vector<std::string> titles(64);
        int max_index = 0;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            trim_inplace(line);
            if (line.size() >= 5 && _strnicmp(line.c_str(), "File", 4) == 0) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const int index = atoi(line.c_str() + 4);
                if (index < 1 || index > 256) {
                    continue;
                }
                if (static_cast<int>(files.size()) < index + 1) {
                    files.resize(static_cast<size_t>(index + 1));
                    titles.resize(static_cast<size_t>(index + 1));
                }
                files[static_cast<size_t>(index)] = line.substr(eq + 1);
                max_index = (std::max)(max_index, index);
            } else if (line.size() >= 6 && _strnicmp(line.c_str(), "Title", 5) == 0) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const int index = atoi(line.c_str() + 5);
                if (index < 1 || index > 256) {
                    continue;
                }
                if (static_cast<int>(titles.size()) < index + 1) {
                    files.resize(static_cast<size_t>(index + 1));
                    titles.resize(static_cast<size_t>(index + 1));
                }
                titles[static_cast<size_t>(index)] = line.substr(eq + 1);
                max_index = (std::max)(max_index, index);
            }
        }
        for (int i = 1; i <= max_index; ++i) {
            if (files[static_cast<size_t>(i)].empty()) {
                continue;
            }
            std::string url = files[static_cast<size_t>(i)];
            rewrite_m3u8_to_m3u9(url);
            PlaylistEntry entry;
            entry.url = to_wide_acp_or_utf8(url);
            entry.title = to_wide_acp_or_utf8(titles[static_cast<size_t>(i)]);
            if (!entry.url.empty()) {
                out.push_back(std::move(entry));
            }
        }
        return !out.empty();
    }

    std::ifstream file(clean, std::ios::binary);
    if (!file) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    return parse_m3u_bytes(text.data(), text.size(), out);
}

bool materialize_rewritten_playlist(const wchar_t* path, std::wstring& out_temp_path) {
    out_temp_path.clear();
    std::vector<PlaylistEntry> entries;
    if (!load_playlist_file(path, entries)) {
        return false;
    }

    wchar_t temp_dir[MAX_PATH] = {};
    const DWORD dir_len = GetTempPathW(MAX_PATH, temp_dir);
    if (dir_len == 0 || dir_len >= MAX_PATH) {
        return false;
    }
    wchar_t temp_file[MAX_PATH] = {};
    if (GetTempFileNameW(temp_dir, L"hls", 0, temp_file) == 0) {
        return false;
    }
    // GetTempFileName creates a .tmp file; replace extension with .m3u so Winamp
    // treats it as a playlist.
    std::wstring target = temp_file;
    const size_t dot = target.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        target.erase(dot);
    }
    target += L".m3u";
    DeleteFileW(temp_file);

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "\xEF\xBB\xBF#EXTM3U\r\n";
    for (const PlaylistEntry& entry : entries) {
        out << "#EXTINF:-1,";
        if (!entry.title.empty()) {
            const int bytes = WideCharToMultiByte(
                CP_UTF8, 0, entry.title.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string utf8(static_cast<size_t>(bytes - 1), '\0');
                WideCharToMultiByte(
                    CP_UTF8, 0, entry.title.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
                out << utf8;
            }
        }
        out << "\r\n";
        if (!entry.url.empty()) {
            const int bytes = WideCharToMultiByte(
                CP_UTF8, 0, entry.url.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string utf8(static_cast<size_t>(bytes - 1), '\0');
                WideCharToMultiByte(
                    CP_UTF8, 0, entry.url.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
                out << utf8;
            }
        }
        out << "\r\n";
    }
    out.close();
    if (!out) {
        DeleteFileW(target.c_str());
        return false;
    }
    out_temp_path = std::move(target);
    return true;
}
