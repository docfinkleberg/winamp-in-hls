#pragma once

#include <string>
#include <vector>

struct PlaylistEntry {
    std::wstring url;
    std::wstring title;
};

// True for local playlist containers we should load ourselves (.m3u / .pls),
// so Winamp does not expand nested .m3u8 URLs into untitled segment rows.
bool is_external_playlist_path(const wchar_t* path);

// Parse M3U/M3U8 text (UTF-8 or ACP bytes). HTTP(S) entries ending in .m3u8 are
// rewritten to .m3u9 so Winamp's playlist loader will not expand them.
bool parse_m3u_bytes(const char* data, size_t size, std::vector<PlaylistEntry>& out);

bool load_playlist_file(const wchar_t* path, std::vector<PlaylistEntry>& out);

// Write a temp .m3u with the same titles whose .m3u8 URLs have been renamed to
// .m3u9 so Winamp's native playlist loader keeps titles and does not expand HLS.
bool materialize_rewritten_playlist(const wchar_t* path, std::wstring& out_temp_path);
