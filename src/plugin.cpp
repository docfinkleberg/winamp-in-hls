#include "plugin.h"

#include "ffmpeg_loader.h"
#include "info_box.h"
#include "log.h"
#include "playlist_bypass.h"
#include "resource.h"
#include "settings.h"
#include "url_util.h"
#include "version.h"

#include "in2.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

char g_description[] = "HLS Input Plug-in v" IN_HLS_VERSION " (x86)";
char g_extensions[] = "m3u8\0HTTP Live Streaming audio (*.m3u8)\0";

In_Module g_module{};
Playback* g_playback = nullptr;

Playback& engine() {
    if (!g_playback) {
        g_playback = new Playback(&g_module);
    }
    return *g_playback;
}

void configure_logging() {
    const std::wstring dir = module_directory();
    load_settings(dir);
}

INT_PTR CALLBACK config_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM) {
    switch (message) {
    case WM_INITDIALOG: {
        const Settings settings = current_settings();
        SetDlgItemInt(dialog, IDC_TIMEOUT, static_cast<UINT>(settings.timeout_ms), FALSE);
        SetDlgItemInt(dialog, IDC_BUFFER, static_cast<UINT>(settings.buffer_ms), FALSE);
        SetDlgItemTextA(dialog, IDC_UA, settings.user_agent.c_str());
        CheckDlgButton(dialog, IDC_LOG, settings.log ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_RECONNECT, settings.reconnect ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            Settings settings = current_settings();
            settings.timeout_ms = static_cast<int>(GetDlgItemInt(dialog, IDC_TIMEOUT, nullptr, FALSE));
            settings.buffer_ms = static_cast<int>(GetDlgItemInt(dialog, IDC_BUFFER, nullptr, FALSE));
            char agent[256] = {};
            GetDlgItemTextA(dialog, IDC_UA, agent, 255);
            settings.user_agent = agent;
            settings.log = IsDlgButtonChecked(dialog, IDC_LOG) == BST_CHECKED;
            settings.reconnect = IsDlgButtonChecked(dialog, IDC_RECONNECT) == BST_CHECKED;
            update_settings(module_directory(), settings);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

void __cdecl on_config(HWND parent) {
    DialogBoxW(reinterpret_cast<HINSTANCE>(plugin_instance()), MAKEINTRESOURCEW(IDD_CONFIG), parent, config_proc);
}

void __cdecl on_about(HWND parent) {
    std::string version = ffmpeg_runtime_version();
    char text[640];
    std::snprintf(
        text,
        sizeof(text),
        "HLS Input Plug-in %s\n"
        "Plays HTTP and HTTPS HLS audio (.m3u8) in 32-bit Winamp 5.x.\n\n"
        "Live streams report an unknown length. On-demand streams can be seeked when a duration is available.\n"
        "DRM-protected streams are not supported.\n\n"
        "FFmpeg: %s",
        IN_HLS_VERSION,
        version.c_str());
    MessageBoxA(parent, text, "HLS Input Plug-in", MB_OK | MB_ICONINFORMATION);
}

int __cdecl on_init() {
    configure_logging();
    std::string error;
    if (!load_ffmpeg_dlls(ffmpeg_search_directory(), error)) {
        LOG_INFO("FFmpeg was not loaded: %s", error.c_str());
    } else {
        ffmpeg_network_startup();
        LOG_INFO("initialised, ffmpeg %s", ffmpeg_runtime_version().c_str());
    }
    install_playlist_bypass(g_module.hMainWindow);
    return IN_INIT_SUCCESS;
}

void __cdecl on_quit() {
    remove_playlist_bypass();
    if (g_playback) {
        g_playback->stop();
    }
    ffmpeg_network_shutdown();
    log_shutdown();
}

void __cdecl on_get_file_info(const char* file, char* title, int* length_ms) {
    engine().file_info(file, title, length_ms);
}

int __cdecl on_info_box(const char* file, HWND parent) {
    return show_hls_info_box(file, parent);
}

int __cdecl on_is_our_file(const char* filename) {
    return claims_hls_name(filename) ? 1 : 0;
}

int __cdecl on_play(const char* filename) {
    const std::string media = canonical_media_path(filename);
    return engine().play(media.c_str());
}

void __cdecl on_pause() { engine().pause(); }
void __cdecl on_unpause() { engine().unpause(); }
int __cdecl on_is_paused() { return engine().is_paused(); }
void __cdecl on_stop() { engine().stop(); }
int __cdecl on_get_length() { return engine().length_ms(); }
int __cdecl on_get_output_time() { return engine().output_time_ms(); }
void __cdecl on_set_output_time(int time_ms) { engine().set_output_time_ms(time_ms); }
void __cdecl on_set_volume(int volume) { engine().set_volume(volume); }
void __cdecl on_set_pan(int pan) { engine().set_pan(pan); }
// Intentionally empty: Winamp applies the built-in EQ inside dsp_dosamples
// unless IN_MODULE_FLAG_EQ is set. We leave that flag clear and call
// dsp_dosamples from the decode loop so the native EQ is used.
void __cdecl on_eq_set(int, char[], int) {}

void fill_module() {
    g_module.version = IN_VER_OLD;
    g_module.description = g_description;
    g_module.hMainWindow = nullptr;
    g_module.hDllInstance = nullptr;
    g_module.FileExtensions = g_extensions;
    g_module.is_seekable = 1;
    g_module.UsesOutputPlug = IN_MODULE_FLAG_USES_OUTPUT_PLUGIN;
    g_module.Config = on_config;
    g_module.About = on_about;
    g_module.Init = on_init;
    g_module.Quit = on_quit;
    g_module.GetFileInfo = on_get_file_info;
    g_module.InfoBox = on_info_box;
    g_module.IsOurFile = on_is_our_file;
    g_module.Play = on_play;
    g_module.Pause = on_pause;
    g_module.UnPause = on_unpause;
    g_module.IsPaused = on_is_paused;
    g_module.Stop = on_stop;
    g_module.GetLength = on_get_length;
    g_module.GetOutputTime = on_get_output_time;
    g_module.SetOutputTime = on_set_output_time;
    g_module.SetVolume = on_set_volume;
    g_module.SetPan = on_set_pan;
    g_module.EQSet = on_eq_set;
    g_module.service = nullptr;
}

struct ModuleInitialiser {
    ModuleInitialiser() { fill_module(); }
};

ModuleInitialiser g_initialiser;

}  // namespace

In_Module* plugin_module() {
    return &g_module;
}

Playback* plugin_playback() {
    return &engine();
}

void* plugin_instance() {
    if (g_module.hDllInstance) {
        return g_module.hDllInstance;
    }
    return loaded_module_instance();
}

extern "C" __declspec(dllexport) In_Module* __cdecl winampGetInModule2() {
    return &g_module;
}

extern "C" __declspec(dllexport) int __cdecl winampUninstallPlugin(HINSTANCE, HWND, int) {
    return IN_PLUGIN_UNINSTALL_NOW;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        note_dll_instance(instance);
    }
    return TRUE;
}
