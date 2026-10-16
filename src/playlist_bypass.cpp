#include "playlist_bypass.h"

#include "log.h"
#include "playlist_file.h"

#include <commdlg.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Published in Winamp's wa_ipc.h from the same interface set as in2.h.
constexpr UINT kWmWaIpc = WM_USER;
constexpr UINT kIpcPlayFile = 100;
constexpr UINT kIpcPlayFileW = 1100;
constexpr UINT kIpcDelete = 101;
constexpr UINT kIpcStartPlay = 102;
constexpr UINT kIpcGetWnd = 260;
constexpr UINT kIpcGetWndPe = 1;
constexpr UINT kWmLoadPendingPlaylists = WM_APP + 0x48L;
constexpr UINT kWmRehookFiles = WM_APP + 0x49L;
constexpr int kUrlControl = 1216;

HWND g_window = nullptr;
HWND g_pe_window = nullptr;
WNDPROC g_previous = nullptr;
WNDPROC g_pe_previous = nullptr;
HHOOK g_hook = nullptr;
std::vector<std::wstring> g_pending_playlists;
bool g_loading_playlist = false;
std::vector<std::wstring> g_temp_playlists;

using GetOpenFileNameW_fn = BOOL(WINAPI*)(LPOPENFILENAMEW);
using GetOpenFileNameA_fn = BOOL(WINAPI*)(LPOPENFILENAMEA);
using CreateFileW_fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_fn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using Wfopen_fn = FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
using Fopen_fn = FILE*(__cdecl*)(const char*, const char*);
GetOpenFileNameW_fn g_real_get_open_file_name_w = nullptr;
GetOpenFileNameA_fn g_real_get_open_file_name_a = nullptr;
CreateFileW_fn g_real_create_file_w = nullptr;
CreateFileA_fn g_real_create_file_a = nullptr;
Wfopen_fn g_real_wfopen = nullptr;
Fopen_fn g_real_fopen = nullptr;
thread_local int g_create_file_depth = 0;

bool extension_boundary(wchar_t next) {
    return next == L'\0' || next == L' ' || next == L'\t' || next == L'"' || next == L'\'' || next == L'?' ||
           next == L'#';
}

bool rewrite_m3u8_limited(wchar_t* text, size_t characters) {
    if (!text || characters == 0) {
        return false;
    }
    bool changed = false;
    for (size_t index = 0; index + 5 < characters && text[index] != L'\0'; ++index) {
        if (text[index] == L'.' && _wcsnicmp(text + index, L".m3u8", 5) == 0 && extension_boundary(text[index + 5])) {
            text[index + 4] = L'9';
            changed = true;
        }
    }
    return changed;
}

bool rewrite_m3u8(wchar_t* text) {
    if (!text) {
        return false;
    }
    return rewrite_m3u8_limited(text, wcslen(text) + 1);
}

bool rewrite_m3u8_ansi(char* text) {
    if (!text) {
        return false;
    }
    bool changed = false;
    for (char* cursor = text; *cursor; ++cursor) {
        if (*cursor == '.' && _strnicmp(cursor, ".m3u8", 5) == 0) {
            const char next = cursor[5];
            if (next == '\0' || next == ' ' || next == '\t' || next == '"' || next == '\'' || next == '?' ||
                next == '#') {
                cursor[4] = '9';
                changed = true;
            }
        }
    }
    return changed;
}

std::wstring unquote_path(const std::wstring& text) {
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

void remember_temp_playlist(const std::wstring& path) {
    g_temp_playlists.push_back(path);
}

bool is_our_temp_playlist(const wchar_t* path) {
    if (!path || !path[0]) {
        return false;
    }
    for (const std::wstring& temp : g_temp_playlists) {
        if (_wcsicmp(temp.c_str(), path) == 0) {
            return true;
        }
    }
    return false;
}

bool rewrite_playlist_path_exists(const wchar_t* path, std::wstring& rewritten) {
    if (!path || !path[0] || !is_external_playlist_path(path) || is_our_temp_playlist(path)) {
        return false;
    }
    if (!materialize_rewritten_playlist(path, rewritten)) {
        LOG_INFO("could not rewrite playlist: %ls", path);
        return false;
    }
    remember_temp_playlist(rewritten);
    LOG_INFO("rewrote playlist %ls -> %ls", path, rewritten.c_str());
    return true;
}

void enqueue_path_w(HWND window, WNDPROC previous, const std::wstring& path) {
    if (!window || !previous || path.empty()) {
        return;
    }
    std::wstring mutable_path = path;
    COPYDATASTRUCT play{};
    play.dwData = kIpcPlayFileW;
    play.lpData = mutable_path.data();
    play.cbData = static_cast<DWORD>((mutable_path.size() + 1) * sizeof(wchar_t));
    const bool nested = g_loading_playlist;
    g_loading_playlist = true;
    CallWindowProcW(previous, window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&play));
    g_loading_playlist = nested;
}

bool open_rewritten_playlist(HWND window, WNDPROC previous, const wchar_t* path, bool clear_playlist,
                             bool start_playback) {
    std::wstring rewritten;
    if (!rewrite_playlist_path_exists(path, rewritten)) {
        return false;
    }
    if (clear_playlist) {
        SendMessageW(window, kWmWaIpc, 0, kIpcDelete);
    }
    enqueue_path_w(window, previous ? previous : g_previous, rewritten);
    if (start_playback) {
        SendMessageW(window, kWmWaIpc, 0, kIpcStartPlay);
    }
    return true;
}

void flush_pending_playlists(HWND window) {
    if (!window || g_pending_playlists.empty() || g_loading_playlist) {
        return;
    }
    const std::vector<std::wstring> pending = std::move(g_pending_playlists);
    g_pending_playlists.clear();
    bool first = true;
    for (const std::wstring& path : pending) {
        open_rewritten_playlist(window, g_previous, path.c_str(), first, first);
        first = false;
    }
}

void capture_command_line_playlists() {
    wchar_t* command = GetCommandLineW();
    if (!command) {
        return;
    }
    for (wchar_t* cursor = command; *cursor; ++cursor) {
        if (cursor[0] != L'.') {
            continue;
        }
        const bool is_m3u = _wcsnicmp(cursor, L".m3u", 4) == 0 && extension_boundary(cursor[4]) &&
                            _wcsnicmp(cursor, L".m3u8", 5) != 0 && _wcsnicmp(cursor, L".m3u9", 5) != 0;
        const bool is_pls = _wcsnicmp(cursor, L".pls", 4) == 0 && extension_boundary(cursor[4]);
        if (!is_m3u && !is_pls) {
            continue;
        }

        wchar_t* ext = cursor;
        wchar_t* begin = cursor;
        while (begin > command && begin[-1] != L' ' && begin[-1] != L'\t') {
            --begin;
        }
        wchar_t* end = ext + 4;
        while (*end && *end != L' ' && *end != L'\t') {
            ++end;
        }

        std::wstring path = unquote_path(std::wstring(begin, end));
        if (!path.empty() && is_external_playlist_path(path.c_str())) {
            g_pending_playlists.push_back(path);
            ext[1] = L'~';
            ext[2] = L'~';
            ext[3] = L'~';
            LOG_INFO("deferred command-line playlist load: %ls", path.c_str());
        }
        cursor = end > cursor ? end - 1 : cursor;
    }
}

void rewrite_command_line() {
    wchar_t* command = GetCommandLineW();
    if (rewrite_m3u8(command)) {
        LOG_INFO("renamed .m3u8 on the Winamp command line so the playlist loader leaves it");
    }
    capture_command_line_playlists();
}

void rewrite_url_control(HWND window) {
    const HWND control = GetDlgItem(window, kUrlControl);
    if (!control) {
        return;
    }
    wchar_t text[4096] = {};
    if (GetWindowTextW(control, text, static_cast<int>(sizeof(text) / sizeof(text[0]))) <= 0) {
        return;
    }
    if (rewrite_m3u8(text)) {
        SetWindowTextW(control, text);
        LOG_INFO("renamed .m3u8 in the Open URL box so the playlist loader leaves it");
    }
}

LRESULT CALLBACK url_box_hook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (message && message->message == WM_COMMAND && LOWORD(message->wParam) == IDOK) {
            rewrite_url_control(message->hwnd);
        }
    }
    return CallNextHookEx(g_hook, code, wparam, lparam);
}

bool patch_iat_module(HMODULE module, const char* dll_name, const char* proc_name, void* hook, void** original) {
    if (!module || !dll_name || !proc_name || !hook) {
        return false;
    }
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) {
        return false;
    }
    auto* import = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<BYTE*>(module) + dir.VirtualAddress);
    for (; import->Name; ++import) {
        const char* name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(module) + import->Name);
        if (_stricmp(name, dll_name) != 0) {
            continue;
        }
        auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(module) + import->FirstThunk);
        auto* original_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<BYTE*>(module) +
            (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
        for (; original_thunk->u1.AddressOfData; ++thunk, ++original_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(original_thunk->u1.Ordinal)) {
                continue;
            }
            auto* by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                reinterpret_cast<BYTE*>(module) + original_thunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), proc_name) != 0) {
                continue;
            }
            DWORD protect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &protect)) {
                return false;
            }
            if (original && !*original) {
                *original = reinterpret_cast<void*>(static_cast<uintptr_t>(thunk->u1.Function));
            }
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&thunk->u1.Function, sizeof(void*), protect, &protect);
            return true;
        }
    }
    return false;
}

bool fopen_mode_is_read_only_w(const wchar_t* mode) {
    if (!mode || !mode[0]) {
        return false;
    }
    // playlist.w5s opens station lists with _wfopen(..., L"r") / L"rt".
    if (wcschr(mode, L'w') || wcschr(mode, L'a') || wcschr(mode, L'+')) {
        return false;
    }
    return wcschr(mode, L'r') != nullptr;
}

bool fopen_mode_is_read_only_a(const char* mode) {
    if (!mode || !mode[0]) {
        return false;
    }
    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) {
        return false;
    }
    return strchr(mode, 'r') != nullptr;
}

std::wstring ofn_selected_path_w(LPOPENFILENAMEW ofn) {
    if (!ofn || !ofn->lpstrFile || !ofn->lpstrFile[0]) {
        return {};
    }
    const wchar_t* buffer = ofn->lpstrFile;
    const size_t first = wcslen(buffer);
    // OFN_EXPLORER multi-select: directory\0file\0\0. Single-select (or a
    // single file with multi-select enabled) is a plain full path.
    if ((ofn->Flags & OFN_ALLOWMULTISELECT) != 0 && buffer[first + 1] != L'\0') {
        const wchar_t* file = buffer + first + 1;
        if (file[0] == L'\0' || file[wcslen(file) + 1] != L'\0') {
            return {};  // multiple files — leave alone
        }
        std::wstring full = buffer;
        if (!full.empty() && full.back() != L'\\' && full.back() != L'/') {
            full.push_back(L'\\');
        }
        full += file;
        return full;
    }
    return buffer;
}

bool apply_rewritten_path_to_ofn_w(LPOPENFILENAMEW ofn, const std::wstring& rewritten) {
    if (!ofn || !ofn->lpstrFile || rewritten.empty()) {
        return false;
    }
    // Need room for path + terminating NUL when clearing a multi-select buffer.
    if (rewritten.size() + 2 > ofn->nMaxFile) {
        LOG_INFO("open-file buffer too small for rewritten playlist path");
        return false;
    }
    wcsncpy_s(ofn->lpstrFile, ofn->nMaxFile, rewritten.c_str(), _TRUNCATE);
    if (rewritten.size() + 1 < ofn->nMaxFile) {
        ofn->lpstrFile[rewritten.size() + 1] = L'\0';
    }
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle > 0) {
        const wchar_t* slash = wcsrchr(rewritten.c_str(), L'\\');
        const wchar_t* name = slash ? slash + 1 : rewritten.c_str();
        wcsncpy_s(ofn->lpstrFileTitle, ofn->nMaxFileTitle, name, _TRUNCATE);
    }
    LOG_INFO("GetOpenFileNameW redirected to %ls", rewritten.c_str());
    return true;
}

BOOL WINAPI hooked_get_open_file_name_w(LPOPENFILENAMEW ofn) {
    if (!g_real_get_open_file_name_w) {
        return FALSE;
    }
    const BOOL ok = g_real_get_open_file_name_w(ofn);
    if (!ok || !ofn || !ofn->lpstrFile || g_loading_playlist) {
        return ok;
    }
    const std::wstring selected = ofn_selected_path_w(ofn);
    if (selected.empty()) {
        return ok;
    }
    std::wstring rewritten;
    if (!rewrite_playlist_path_exists(selected.c_str(), rewritten)) {
        return ok;
    }
    apply_rewritten_path_to_ofn_w(ofn, rewritten);
    return ok;
}

BOOL WINAPI hooked_get_open_file_name_a(LPOPENFILENAMEA ofn) {
    if (!g_real_get_open_file_name_a) {
        return FALSE;
    }
    const BOOL ok = g_real_get_open_file_name_a(ofn);
    if (!ok || !ofn || !ofn->lpstrFile || g_loading_playlist) {
        return ok;
    }
    const int chars = MultiByteToWideChar(CP_ACP, 0, ofn->lpstrFile, -1, nullptr, 0);
    if (chars <= 1) {
        return ok;
    }
    std::wstring wide(static_cast<size_t>(chars - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, ofn->lpstrFile, -1, wide.data(), chars);
    // ANSI multi-select with directory\0file is uncommon for Load list; handle
    // the plain single-path form which Winamp uses most of the time.
    if ((ofn->Flags & OFN_ALLOWMULTISELECT) != 0 && ofn->lpstrFile[strlen(ofn->lpstrFile) + 1] != '\0') {
        return ok;
    }
    std::wstring rewritten;
    if (!rewrite_playlist_path_exists(wide.c_str(), rewritten)) {
        return ok;
    }
    const int bytes = WideCharToMultiByte(CP_ACP, 0, rewritten.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1 || static_cast<DWORD>(bytes) + 1 > ofn->nMaxFile) {
        LOG_INFO("open-file ANSI buffer too small for rewritten playlist path");
        return ok;
    }
    WideCharToMultiByte(CP_ACP, 0, rewritten.c_str(), -1, ofn->lpstrFile, bytes, nullptr, nullptr);
    ofn->lpstrFile[bytes] = '\0';
    LOG_INFO("GetOpenFileNameA redirected to rewritten playlist");
    return ok;
}

HANDLE WINAPI hooked_create_file_w(
    LPCWSTR path,
    DWORD access,
    DWORD share,
    LPSECURITY_ATTRIBUTES security,
    DWORD disposition,
    DWORD flags,
    HANDLE template_file) {
    if (!g_real_create_file_w) {
        return INVALID_HANDLE_VALUE;
    }
    const bool read_existing = disposition == OPEN_EXISTING &&
                               (access & (GENERIC_WRITE | GENERIC_ALL | DELETE)) == 0 &&
                               path && path[0] && g_create_file_depth == 0 && !g_loading_playlist &&
                               !is_our_temp_playlist(path) && is_external_playlist_path(path);
    if (read_existing) {
        ++g_create_file_depth;
        std::wstring rewritten;
        const bool ok = rewrite_playlist_path_exists(path, rewritten);
        --g_create_file_depth;
        if (ok) {
            LOG_INFO("CreateFileW redirected playlist read");
            return g_real_create_file_w(
                rewritten.c_str(), access, share, security, disposition, flags, template_file);
        }
    }
    return g_real_create_file_w(path, access, share, security, disposition, flags, template_file);
}

HANDLE WINAPI hooked_create_file_a(
    LPCSTR path,
    DWORD access,
    DWORD share,
    LPSECURITY_ATTRIBUTES security,
    DWORD disposition,
    DWORD flags,
    HANDLE template_file) {
    if (!g_real_create_file_a) {
        return INVALID_HANDLE_VALUE;
    }
    if (path && path[0] && disposition == OPEN_EXISTING &&
        (access & (GENERIC_WRITE | GENERIC_ALL | DELETE)) == 0 && g_create_file_depth == 0 &&
        !g_loading_playlist) {
        const int chars = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
        if (chars > 1) {
            std::wstring wide(static_cast<size_t>(chars - 1), L'\0');
            MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), chars);
            if (!is_our_temp_playlist(wide.c_str()) && is_external_playlist_path(wide.c_str())) {
                ++g_create_file_depth;
                std::wstring rewritten;
                const bool ok = rewrite_playlist_path_exists(wide.c_str(), rewritten);
                --g_create_file_depth;
                if (ok) {
                    const int bytes =
                        WideCharToMultiByte(CP_ACP, 0, rewritten.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    if (bytes > 1) {
                        std::string ansi(static_cast<size_t>(bytes - 1), '\0');
                        WideCharToMultiByte(
                            CP_ACP, 0, rewritten.c_str(), -1, ansi.data(), bytes, nullptr, nullptr);
                        LOG_INFO("CreateFileA redirected playlist read");
                        return g_real_create_file_a(
                            ansi.c_str(), access, share, security, disposition, flags, template_file);
                    }
                }
            }
        }
    }
    return g_real_create_file_a(path, access, share, security, disposition, flags, template_file);
}

FILE* __cdecl hooked_wfopen(const wchar_t* path, const wchar_t* mode) {
    if (!g_real_wfopen) {
        return nullptr;
    }
    // List Opts > Load list reads the .m3u through playlist.w5s via _wfopen.
    if (path && path[0] && fopen_mode_is_read_only_w(mode) && g_create_file_depth == 0 &&
        !g_loading_playlist && !is_our_temp_playlist(path) && is_external_playlist_path(path)) {
        ++g_create_file_depth;
        std::wstring rewritten;
        const bool ok = rewrite_playlist_path_exists(path, rewritten);
        --g_create_file_depth;
        if (ok) {
            LOG_INFO("_wfopen redirected playlist read: %ls", path);
            return g_real_wfopen(rewritten.c_str(), mode);
        }
    }
    return g_real_wfopen(path, mode);
}

FILE* __cdecl hooked_fopen(const char* path, const char* mode) {
    if (!g_real_fopen) {
        return nullptr;
    }
    if (path && path[0] && fopen_mode_is_read_only_a(mode) && g_create_file_depth == 0 &&
        !g_loading_playlist) {
        const int chars = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
        if (chars > 1) {
            std::wstring wide(static_cast<size_t>(chars - 1), L'\0');
            MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), chars);
            if (!is_our_temp_playlist(wide.c_str()) && is_external_playlist_path(wide.c_str())) {
                ++g_create_file_depth;
                std::wstring rewritten;
                const bool ok = rewrite_playlist_path_exists(wide.c_str(), rewritten);
                --g_create_file_depth;
                if (ok) {
                    const int bytes =
                        WideCharToMultiByte(CP_ACP, 0, rewritten.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    if (bytes > 1) {
                        std::string ansi(static_cast<size_t>(bytes - 1), '\0');
                        WideCharToMultiByte(
                            CP_ACP, 0, rewritten.c_str(), -1, ansi.data(), bytes, nullptr, nullptr);
                        LOG_INFO("fopen redirected playlist read");
                        return g_real_fopen(ansi.c_str(), mode);
                    }
                }
            }
        }
    }
    return g_real_fopen(path, mode);
}

bool patch_create_file_imports(HMODULE module) {
    bool patched = false;
    const char* kernels[] = {
        "kernel32.dll",
        "api-ms-win-core-file-l1-1-0.dll",
        "api-ms-win-core-file-l1-2-0.dll",
        "api-ms-win-core-file-l1-2-1.dll",
    };
    for (const char* dll : kernels) {
        if (patch_iat_module(
                module, dll, "CreateFileW", reinterpret_cast<void*>(&hooked_create_file_w),
                reinterpret_cast<void**>(&g_real_create_file_w))) {
            patched = true;
        }
        if (patch_iat_module(
                module, dll, "CreateFileA", reinterpret_cast<void*>(&hooked_create_file_a),
                reinterpret_cast<void**>(&g_real_create_file_a))) {
            patched = true;
        }
    }
    return patched;
}

bool patch_stdio_imports(HMODULE module) {
    bool patched = false;
    const char* crts[] = {
        "api-ms-win-crt-stdio-l1-1-0.dll",
        "ucrtbase.dll",
        "msvcrt.dll",
    };
    for (const char* dll : crts) {
        if (patch_iat_module(
                module, dll, "_wfopen", reinterpret_cast<void*>(&hooked_wfopen),
                reinterpret_cast<void**>(&g_real_wfopen))) {
            patched = true;
        }
        if (patch_iat_module(
                module, dll, "fopen", reinterpret_cast<void*>(&hooked_fopen),
                reinterpret_cast<void**>(&g_real_fopen))) {
            patched = true;
        }
    }
    return patched;
}

void install_open_file_hooks() {
    HMODULE comdlg = GetModuleHandleW(L"comdlg32.dll");
    if (!comdlg) {
        comdlg = LoadLibraryW(L"comdlg32.dll");
    }
    if (comdlg) {
        g_real_get_open_file_name_w =
            reinterpret_cast<GetOpenFileNameW_fn>(GetProcAddress(comdlg, "GetOpenFileNameW"));
        g_real_get_open_file_name_a =
            reinterpret_cast<GetOpenFileNameA_fn>(GetProcAddress(comdlg, "GetOpenFileNameA"));
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel) {
        g_real_create_file_w = reinterpret_cast<CreateFileW_fn>(GetProcAddress(kernel, "CreateFileW"));
        g_real_create_file_a = reinterpret_cast<CreateFileA_fn>(GetProcAddress(kernel, "CreateFileA"));
    }
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    if (!ucrt) {
        ucrt = LoadLibraryW(L"ucrtbase.dll");
    }
    if (ucrt) {
        g_real_wfopen = reinterpret_cast<Wfopen_fn>(GetProcAddress(ucrt, "_wfopen"));
        g_real_fopen = reinterpret_cast<Fopen_fn>(GetProcAddress(ucrt, "fopen"));
    }

    wchar_t winamp_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, winamp_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(winamp_dir, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    std::wstring playlist_path = std::wstring(winamp_dir) + L"\\System\\playlist.w5s";
    HMODULE playlist_mod = GetModuleHandleW(L"playlist.w5s");
    if (!playlist_mod) {
        playlist_mod = GetModuleHandleW(playlist_path.c_str());
    }

    HMODULE modules[] = {
        GetModuleHandleW(nullptr),
        playlist_mod,
        ucrt,
    };
    bool open_patched = false;
    bool create_patched = false;
    bool stdio_patched = false;
    for (HMODULE module : modules) {
        if (!module) {
            continue;
        }
        if (patch_iat_module(
                module, "comdlg32.dll", "GetOpenFileNameW", reinterpret_cast<void*>(&hooked_get_open_file_name_w),
                reinterpret_cast<void**>(&g_real_get_open_file_name_w))) {
            open_patched = true;
        }
        if (patch_iat_module(
                module, "comdlg32.dll", "GetOpenFileNameA", reinterpret_cast<void*>(&hooked_get_open_file_name_a),
                reinterpret_cast<void**>(&g_real_get_open_file_name_a))) {
            open_patched = true;
        }
        if (patch_create_file_imports(module)) {
            create_patched = true;
        }
        if (patch_stdio_imports(module)) {
            stdio_patched = true;
        }
    }
    LOG_INFO(
        "playlist open hooks: GetOpenFileName=%s CreateFile=%s stdio(_wfopen)=%s playlist.w5s=%s",
        open_patched ? "yes" : "no",
        create_patched ? "yes" : "no",
        stdio_patched ? "yes" : "no",
        playlist_mod ? "loaded" : "missing");
}

bool handle_copydata_playlist(HWND window, WNDPROC previous, const COPYDATASTRUCT* data) {
    if (!data || !data->lpData || g_loading_playlist) {
        return false;
    }
    if (data->dwData == kIpcPlayFileW && data->cbData >= sizeof(wchar_t)) {
        const auto* path = static_cast<const wchar_t*>(data->lpData);
        if (!is_external_playlist_path(path)) {
            return false;
        }
        // Explorer / second-instance open: replace playlist and play.
        open_rewritten_playlist(window, previous, path, true, true);
        return true;
    }
    if (data->dwData == kIpcPlayFile && data->cbData > 0) {
        const auto* path_a = static_cast<const char*>(data->lpData);
        const int chars = MultiByteToWideChar(CP_ACP, 0, path_a, -1, nullptr, 0);
        if (chars <= 1) {
            return false;
        }
        std::wstring path(static_cast<size_t>(chars - 1), L'\0');
        MultiByteToWideChar(CP_ACP, 0, path_a, -1, path.data(), chars);
        if (!is_external_playlist_path(path.c_str())) {
            return false;
        }
        open_rewritten_playlist(window, previous, path.c_str(), true, true);
        return true;
    }
    return false;
}

LRESULT handle_dropfiles(HWND window, WNDPROC previous, WPARAM wparam) {
    const HDROP drop = reinterpret_cast<HDROP>(wparam);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> names;
    names.reserve(count);
    bool saw_special = false;
    for (UINT index = 0; index < count; ++index) {
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, index, path, MAX_PATH) == 0) {
            continue;
        }
        if (is_external_playlist_path(path)) {
            std::wstring rewritten;
            if (rewrite_playlist_path_exists(path, rewritten)) {
                names.push_back(std::move(rewritten));
                saw_special = true;
                continue;
            }
        }
        if (rewrite_m3u8(path)) {
            saw_special = true;
        }
        names.emplace_back(path);
    }

    if (!saw_special) {
        return CallWindowProcW(previous, window, WM_DROPFILES, wparam, 0);
    }

    const bool enqueue_only = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    DragFinish(drop);
    if (!enqueue_only) {
        SendMessageW(window, kWmWaIpc, 0, kIpcDelete);
    }
    for (const std::wstring& name : names) {
        enqueue_path_w(window, previous, name);
    }
    if (!enqueue_only) {
        SendMessageW(window, kWmWaIpc, 0, kIpcStartPlay);
    }
    return 1;
}

LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kWmLoadPendingPlaylists) {
        flush_pending_playlists(window);
        return 0;
    }
    if (message == kWmRehookFiles) {
        // playlist.w5s may finish loading after input plug-ins; re-apply IAT hooks.
        install_open_file_hooks();
        return 0;
    }

    if (message == WM_COPYDATA && g_previous) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (handle_copydata_playlist(window, g_previous, data)) {
            return TRUE;
        }
        if (data && data->lpData && (data->dwData == kIpcPlayFileW || data->dwData == kIpcPlayFile)) {
            if (data->dwData == kIpcPlayFileW && data->cbData >= sizeof(wchar_t)) {
                rewrite_m3u8_limited(
                    static_cast<wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            } else {
                rewrite_m3u8_ansi(static_cast<char*>(data->lpData));
            }
        }
    }

    if (message == WM_DROPFILES && g_previous) {
        return handle_dropfiles(window, g_previous, wparam);
    }

    return CallWindowProcW(g_previous, window, message, wparam, lparam);
}

LRESULT CALLBACK pe_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DROPFILES && g_pe_previous) {
        // Route playlist drops on the playlist editor through the same rewrite path.
        return handle_dropfiles(g_window ? g_window : window, g_previous ? g_previous : g_pe_previous, wparam);
    }
    return CallWindowProcW(g_pe_previous, window, message, wparam, lparam);
}

void subclass_playlist_editor(HWND main_window) {
    const HWND pe = reinterpret_cast<HWND>(SendMessageW(main_window, kWmWaIpc, kIpcGetWndPe, kIpcGetWnd));
    if (!pe) {
        return;
    }
    g_pe_window = pe;
    g_pe_previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(pe, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pe_proc)));
    LOG_INFO("playlist editor subclass %s", g_pe_previous ? "installed" : "failed");
}

void cleanup_temp_playlists() {
    for (const std::wstring& path : g_temp_playlists) {
        DeleteFileW(path.c_str());
    }
    g_temp_playlists.clear();
}

}  // namespace

void install_playlist_bypass(HWND main_window) {
    rewrite_command_line();
    install_open_file_hooks();
    if (!main_window) {
        return;
    }
    g_window = main_window;
    g_previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(main_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(main_proc)));
    g_hook = SetWindowsHookExW(WH_CALLWNDPROC, url_box_hook, nullptr, GetCurrentThreadId());
    subclass_playlist_editor(main_window);
    LOG_INFO("playlist bypass %s", g_previous ? "installed" : "could not subclass the main window");
    PostMessageW(main_window, kWmRehookFiles, 0, 0);
    if (!g_pending_playlists.empty()) {
        PostMessageW(main_window, kWmLoadPendingPlaylists, 0, 0);
    }
}

void remove_playlist_bypass() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_pe_window && g_pe_previous) {
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_pe_window, GWLP_WNDPROC));
        if (current == pe_proc) {
            SetWindowLongPtrW(g_pe_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_pe_previous));
        }
    }
    g_pe_window = nullptr;
    g_pe_previous = nullptr;
    if (g_window && g_previous) {
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
        if (current == main_proc) {
            SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_previous));
        }
    }
    g_window = nullptr;
    g_previous = nullptr;
    g_pending_playlists.clear();
    cleanup_temp_playlists();
}
