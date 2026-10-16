#pragma once

#include <string>

// True when the string has a URI scheme, for example http:// or file://.
bool is_remote_url(const char* text);

// True for remote URLs whose path ends in .m3u8. Query strings and fragments
// are ignored and the original text is never modified.
bool is_hls_url(const char* text);

// True when this plug-in should accept the name Winamp is about to play.
// That includes remote .m3u8 paths and the .m3u9 alias used to get past
// Winamp's built-in playlist loader.
bool claims_hls_name(const char* filename);

// Turns the .m3u9 alias back into .m3u8. Every other string is copied unchanged.
std::string canonical_media_path(const char* winamp_path);

// Remote URLs are returned unchanged, including percent-encoding and signed
// query parameters. Local paths are converted from the system ANSI code page
// to UTF-8 for FFmpeg.
std::string to_ffmpeg_input(const char* winamp_path);

std::string utf8_to_acp(const std::string& utf8);
std::string wide_to_utf8(const wchar_t* text);
std::wstring utf8_to_wide(const std::string& utf8);
