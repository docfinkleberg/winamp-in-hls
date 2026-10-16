#pragma once

#include <string>

// Remember the plug-in directory. Safe to call from DllMain; it does not load libraries.
void note_dll_instance(void* instance);
void* loaded_module_instance();

void set_ffmpeg_search_directory(const std::wstring& directory);
std::wstring ffmpeg_search_directory();
std::wstring module_directory();

// Load FFmpeg from directory using the plug-in folder, not the process PATH.
// Safe to call more than once. Returns false with error set when a required DLL is missing.
bool load_ffmpeg_dlls(const std::wstring& directory, std::string& error);

std::string ffmpeg_runtime_version();
void ffmpeg_network_startup();
void ffmpeg_network_shutdown();
