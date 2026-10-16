#include "playback.h"

#include "audio_buffer.h"
#include "ffmpeg_decoder.h"
#include "ffmpeg_loader.h"
#include "log.h"
#include "settings.h"
#include "url_util.h"
#include "winamp_eof.h"

#include "in2.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Env flags:
//   IN_HLS_DECODE_ONLY=1  → skip Write and visualisation (bisect)
//   IN_HLS_NO_VIS=1       → Write on, skip SA/VSA (bisect / emergency)
//   IN_HLS_SA_ONLY=1      → SA only (no VSAAddPCMData)
bool env_flag_set(const char* name) {
    char value[8] = {};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n > 0 && value[0] == '1';
}

bool decode_only_bisect() {
    return env_flag_set("IN_HLS_DECODE_ONLY");
}

bool vis_enabled() {
    return !decode_only_bisect() && !env_flag_set("IN_HLS_NO_VIS");
}

bool sa_only_bisect() {
    return env_flag_set("IN_HLS_SA_ONLY");
}

// Winamp playlist IPC (wa_ipc.h).
constexpr UINT kWmWaIpc = WM_USER;
constexpr UINT kIpcGetListLength = 124;
constexpr UINT kIpcGetListPos = 125;
constexpr UINT kIpcGetPlaylistFile = 211;
constexpr UINT kIpcGetPlaylistFileW = 214;
constexpr UINT kIpcGetPlaylistTitle = 212;
constexpr UINT kIpcGetPlaylistTitleW = 213;

std::string base_name(const std::string& path) {
    size_t end = path.find_first_of("?#");
    if (end == std::string::npos) {
        end = path.size();
    }
    size_t start = path.find_last_of("/\\", end == 0 ? 0 : end - 1);
    if (start == std::string::npos) {
        start = 0;
    } else {
        ++start;
    }
    if (start >= end) {
        return path;
    }
    return path.substr(start, end - start);
}

void copy_title(char* title, const std::string& text) {
    if (!title) {
        return;
    }
    lstrcpynA(title, text.c_str(), GETFILEINFO_TITLE_LENGTH);
}

std::string wide_to_acp(const wchar_t* text) {
    if (!text || !text[0]) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

bool same_media_path(const char* left, const char* right) {
    if (!left || !right) {
        return false;
    }
    const std::string a = canonical_media_path(left);
    const std::string b = canonical_media_path(right);
    return !a.empty() && _stricmp(a.c_str(), b.c_str()) == 0;
}

// True when the playlist title is just Winamp/plugin filler (URL or path leaf),
// not an #EXTINF station name.
bool title_looks_automatic(const std::string& title, const char* file) {
    if (title.empty() || !file || !file[0]) {
        return true;
    }
    if (_stricmp(title.c_str(), file) == 0) {
        return true;
    }
    const std::string leaf = base_name(file);
    if (!leaf.empty() && _stricmp(title.c_str(), leaf.c_str()) == 0) {
        return true;
    }
    // Playlist entries may still show the .m3u9 alias while we compare .m3u8.
    if (leaf.size() >= 5) {
        std::string alt = leaf;
        if (_strnicmp(alt.c_str() + alt.size() - 5, ".m3u8", 5) == 0) {
            alt[alt.size() - 1] = '9';
            if (_stricmp(title.c_str(), alt.c_str()) == 0) {
                return true;
            }
        } else if (_strnicmp(alt.c_str() + alt.size() - 5, ".m3u9", 5) == 0) {
            alt[alt.size() - 1] = '8';
            if (_stricmp(title.c_str(), alt.c_str()) == 0) {
                return true;
            }
        }
    }
    return false;
}

std::string playlist_station_title(In_Module* module, const char* file) {
    if (!module || !module->hMainWindow || !file || !file[0]) {
        return {};
    }
    const HWND window = module->hMainWindow;
    const int count = static_cast<int>(SendMessageW(window, kWmWaIpc, 0, kIpcGetListLength));
    if (count <= 0) {
        return {};
    }

    auto title_at = [&](int index) -> std::string {
        if (index < 0 || index >= count) {
            return {};
        }
        const auto* file_w =
            reinterpret_cast<const wchar_t*>(SendMessageW(window, kWmWaIpc, index, kIpcGetPlaylistFileW));
        std::string entry_file;
        if (file_w && file_w[0]) {
            entry_file = wide_to_acp(file_w);
        } else {
            const auto* file_a =
                reinterpret_cast<const char*>(SendMessageW(window, kWmWaIpc, index, kIpcGetPlaylistFile));
            if (!file_a || !file_a[0]) {
                return {};
            }
            entry_file = file_a;
        }
        if (!same_media_path(entry_file.c_str(), file)) {
            return {};
        }

        const auto* title_w =
            reinterpret_cast<const wchar_t*>(SendMessageW(window, kWmWaIpc, index, kIpcGetPlaylistTitleW));
        std::string title;
        if (title_w && title_w[0]) {
            title = wide_to_acp(title_w);
        } else {
            const auto* title_a =
                reinterpret_cast<const char*>(SendMessageW(window, kWmWaIpc, index, kIpcGetPlaylistTitle));
            if (title_a && title_a[0]) {
                title = title_a;
            }
        }
        if (title_looks_automatic(title, entry_file.c_str()) || title_looks_automatic(title, file)) {
            return {};
        }
        return title;
    };

    const int pos = static_cast<int>(SendMessageW(window, kWmWaIpc, 0, kIpcGetListPos));
    const std::string at_pos = title_at(pos);
    if (!at_pos.empty()) {
        return at_pos;
    }
    for (int index = 0; index < count; ++index) {
        if (index == pos) {
            continue;
        }
        const std::string found = title_at(index);
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

// Prefer #EXTINF / playlist station names over URL path leaves. Ctrl+L and
// untitled entries still fall back to the stream leaf name.
std::string resolve_display_title(In_Module* module, const char* file, const std::string& stream_title) {
    const std::string station = playlist_station_title(module, file);
    if (!station.empty()) {
        return station;
    }
    if (!stream_title.empty()) {
        return stream_title;
    }
    return file && file[0] ? base_name(file) : std::string{};
}

// Classic Winamp SetInfo: bitrate kbps, sample rate in kHz (not Hz),
// and channel count (1=mono, 2=stereo) — matching in_opus.
void set_winamp_info(In_Module* mod, int bitrate_kbps, int sample_rate_hz, int channels) {
    if (!mod || !mod->SetInfo) {
        return;
    }
    const int kbps = bitrate_kbps > 0 ? bitrate_kbps : -1;
    const int khz = sample_rate_hz > 0 ? sample_rate_hz / 1000 : -1;
    const int nch = channels > 0 ? channels : -1;
    mod->SetInfo(kbps, khz, nch, 1);
}

}  // namespace

struct Playback::Impl {
    In_Module* module = nullptr;
    FfmpegDecoder decoder;
    std::thread worker;
    mutable std::mutex state_mu;
    std::mutex out_mu;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> running{false};
    std::atomic<int> seek_to{-1};
    std::atomic<int> duration_ms{-1};
    std::atomic<int> sample_rate{0};
    std::atomic<int> channels{0};
    std::atomic<int> bitrate_kbps{0};
    std::atomic<bool> seekable{false};
    std::atomic<bool> output_open{false};
    // True after a successful outMod->Open until we Close it in stop().
    std::atomic<bool> output_device_open{false};
    // True after SAVSAInit until matching SAVSADeInit.
    std::atomic<bool> savsa_inited{false};
    // Bumped on each play start and after stop teardown so a late worker
    // cannot touch outMod after Close.
    std::atomic<uint32_t> play_generation{0};
    std::string title;
    std::string current_file;
    std::string error;
};

Playback::Playback(In_Module* module) : impl_(new Impl) {
    impl_->module = module;
}

Playback::~Playback() {
    stop();
    delete impl_;
    impl_ = nullptr;
}

void Playback::set_module(In_Module* module) {
    impl_->module = module;
}

int Playback::play(const char* filename) {
    stop();
    if (impl_->worker.joinable() || impl_->output_open.load()) {
        LOG_INFO("play aborted: previous session still active after stop");
        return -1;
    }
    if (!filename || !filename[0]) {
        return -1;
    }
    if (!is_remote_url(filename)) {
        const DWORD attributes = GetFileAttributesA(filename);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return -1;
        }
    }

    std::string load_error;
    if (!load_ffmpeg_dlls(ffmpeg_search_directory(), load_error)) {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->error = load_error;
        LOG_INFO("%s", load_error.c_str());
        return -1;
    }

    In_Module* mod = impl_->module;
    if (!mod || !mod->outMod || !mod->outMod->Open || !mod->outMod->Write || !mod->outMod->CanWrite) {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->error = "Winamp did not provide an output plug-in";
        LOG_INFO("%s", impl_->error.c_str());
        return -1;
    }

    impl_->stop.store(false);
    impl_->paused.store(false);
    impl_->seek_to.store(-1);
    impl_->duration_ms.store(-1);
    impl_->sample_rate.store(0);
    impl_->channels.store(0);
    impl_->bitrate_kbps.store(0);
    impl_->seekable.store(false);
    impl_->output_open.store(false);
    // output_device_open is cleared only in stop() after Close.
    {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->title.clear();
        impl_->error.clear();
        impl_->current_file = filename;
    }
    mod->is_seekable = 0;

    // Match in_opus: open the decoder and the output on the Play() thread
    // (Winamp's thread), then start a worker that only decodes and writes.
    const Settings settings = current_settings();
    DecoderSettings decode_settings;
    decode_settings.timeout_ms = settings.timeout_ms;
    decode_settings.reconnect = settings.reconnect;
    decode_settings.user_agent = settings.user_agent;
    impl_->decoder.set_stop_flag(&impl_->stop);

    std::string open_error;
    const std::string input = to_ffmpeg_input(filename);
    if (!impl_->decoder.open(input, decode_settings, open_error)) {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->error = open_error;
        impl_->title = open_error;
        LOG_INFO("open failed: %s", open_error.c_str());
        return -1;
    }

    // Resolve outside state_mu: playlist IPC can re-enter GetFileInfo.
    const std::string display_title = resolve_display_title(mod, filename, impl_->decoder.title());
    {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->title = display_title;
    }
    impl_->duration_ms.store(static_cast<int>(impl_->decoder.duration_ms()));
    impl_->seekable.store(impl_->decoder.seekable());
    impl_->bitrate_kbps.store(impl_->decoder.bitrate_kbps());
    mod->is_seekable = impl_->decoder.seekable() ? 1 : 0;

    const int rate = impl_->decoder.sample_rate();
    const int channels = impl_->decoder.channels();
    if (rate <= 0 || channels <= 0) {
        impl_->decoder.close();
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->error = "The stream did not report a sample rate";
        LOG_INFO("%s", impl_->error.c_str());
        return -1;
    }

    // in_opus uses -1,-1 for the default output buffer lengths.
    const int latency = mod->outMod->Open(rate, channels, 16, -1, -1);
    if (latency < 0) {
        impl_->decoder.close();
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        impl_->error = "The output plug-in rejected the audio format";
        LOG_INFO("%s", impl_->error.c_str());
        return -1;
    }

    const uint32_t generation = impl_->play_generation.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(impl_->out_mu);
        impl_->output_open.store(true);
        impl_->output_device_open.store(true);
    }
    impl_->sample_rate.store(rate);
    impl_->channels.store(channels);
    // Match in_opus: init vis with Open() latency, then feed SA/VSA from the
    // decode thread on every full 576-frame block (classic spectrum / osc).
    if (vis_enabled()) {
        if (mod->SAVSAInit) {
            mod->SAVSAInit(latency, rate);
            impl_->savsa_inited.store(true);
        }
        if (mod->VSASetInfo) {
            mod->VSASetInfo(rate, channels);
        }
    }
    if (mod->outMod->SetVolume) {
        mod->outMod->SetVolume(-666);
    }
    set_winamp_info(mod, impl_->bitrate_kbps.load(), rate, channels);
    LOG_INFO(
        "output open rate=%d channels=%d latency=%d gen=%u dsp=%d/%d",
        rate,
        channels,
        latency,
        static_cast<unsigned>(generation),
        mod->dsp_isactive ? 1 : 0,
        mod->dsp_dosamples ? 1 : 0);
    if (mod->dsp_isactive) {
        LOG_INFO("dsp_isactive()=%d (1=DSP plug-in, 2=EQ)", mod->dsp_isactive());
    }

    impl_->running.store(true);
    try {
        impl_->worker = std::thread([this, file = std::string(filename), generation] {
            thread_main(file, generation);
        });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(impl_->out_mu);
            impl_->output_open.store(false);
            if (impl_->output_device_open.exchange(false) && mod->outMod->Close) {
                mod->outMod->Close();
            }
            if (impl_->savsa_inited.exchange(false) && mod->SAVSADeInit) {
                mod->SAVSADeInit();
            }
        }
        impl_->decoder.close();
        impl_->running.store(false);
        impl_->play_generation.fetch_add(1);
        return -1;
    }
    return 0;
}

void Playback::pause() {
    impl_->paused.store(true);
    std::lock_guard<std::mutex> lock(impl_->out_mu);
    if (impl_->output_open.load() && impl_->module && impl_->module->outMod && impl_->module->outMod->Pause) {
        impl_->module->outMod->Pause(1);
    }
}

void Playback::unpause() {
    impl_->paused.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->out_mu);
        if (impl_->output_open.load() && impl_->module && impl_->module->outMod && impl_->module->outMod->Pause) {
            impl_->module->outMod->Pause(0);
        }
    }
    impl_->cv.notify_all();
}

int Playback::is_paused() const {
    return impl_->paused.load() ? 1 : 0;
}

void Playback::stop() {
    const DWORD t0 = GetTickCount();
    const uint32_t gen_at_stop = impl_->play_generation.load();
    impl_->stop.store(true);
    impl_->paused.store(false);
    impl_->decoder.request_stop();
    impl_->cv.notify_all();

    // Do NOT take out_mu here — the worker may hold it inside Write().
    // Taking the mutex before join deadlocks Stop against Write, and the
    // subsequent TerminateThread then corrupts out_ds / the CRT.
    impl_->output_open.store(false, std::memory_order_release);
    impl_->play_generation.fetch_add(1, std::memory_order_acq_rel);

    if (impl_->worker.joinable()) {
        if (impl_->worker.get_id() != std::this_thread::get_id()) {
            HANDLE handle = reinterpret_cast<HANDLE>(impl_->worker.native_handle());
            // Match in_opus: wait up to 5s. Prefer a clean exit (FFmpeg
            // interrupt) over TerminateThread, which often crashes Winamp.
            DWORD waited = WaitForSingleObject(handle, 5000);
            if (waited == WAIT_TIMEOUT) {
                LOG_INFO(
                    "stop: decode thread still running after 5s (gen=%u), waiting 3s more",
                    static_cast<unsigned>(gen_at_stop));
                // Re-assert interrupt / stop for stubborn network reads.
                impl_->decoder.request_stop();
                waited = WaitForSingleObject(handle, 3000);
            }
            if (waited == WAIT_TIMEOUT) {
                LOG_INFO(
                    "stop: terminating stuck decode thread (gen=%u)",
                    static_cast<unsigned>(gen_at_stop));
                TerminateThread(handle, 0);
            }
            impl_->worker.join();
        }
    }

    // Match in_opus: Close, then SAVSADeInit, then free decoder — only after
    // the worker is gone so no thread can touch outMod.
    In_Module* mod = impl_->module;
    {
        std::lock_guard<std::mutex> lock(impl_->out_mu);
        const bool device_was_open = impl_->output_device_open.exchange(false);
        if (device_was_open && mod && mod->outMod && mod->outMod->Close) {
            mod->outMod->Close();
        }
        if (impl_->savsa_inited.exchange(false) && mod && mod->SAVSADeInit) {
            mod->SAVSADeInit();
        }
    }
    impl_->decoder.close();
    impl_->running.store(false);
    LOG_INFO(
        "stop complete gen=%u join_ms=%u",
        static_cast<unsigned>(gen_at_stop),
        static_cast<unsigned>(GetTickCount() - t0));
}

int Playback::length_ms() const {
    return impl_->duration_ms.load();
}

int Playback::output_time_ms() const {
    std::lock_guard<std::mutex> lock(impl_->out_mu);
    if (!impl_->output_open.load() || !impl_->module || !impl_->module->outMod || !impl_->module->outMod->GetOutputTime) {
        return 0;
    }
    return impl_->module->outMod->GetOutputTime();
}

void Playback::set_output_time_ms(int time_ms) {
    if (time_ms < 0 || !impl_->seekable.load()) {
        return;
    }
    impl_->seek_to.store(time_ms);
    impl_->cv.notify_all();
}

int Playback::written_time_ms() const {
    std::lock_guard<std::mutex> lock(impl_->out_mu);
    if (!impl_->output_open.load() || !impl_->module || !impl_->module->outMod || !impl_->module->outMod->GetWrittenTime) {
        return 0;
    }
    return impl_->module->outMod->GetWrittenTime();
}

PlaybackInfoSnapshot Playback::info_snapshot(const char* file) const {
    PlaybackInfoSnapshot snap;
    std::string current_file;
    std::string stored_title;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        current_file = impl_->current_file;
        stored_title = impl_->title;
    }
    const std::string asked = file && file[0] ? canonical_media_path(file) : current_file;
    snap.file = asked.empty() && file ? std::string(file) : asked;
    snap.playing = impl_->running.load();
    snap.paused = impl_->paused.load() != 0;
    snap.current = snap.playing && !current_file.empty() &&
                   (asked.empty() || same_media_path(asked.c_str(), current_file.c_str()));
    snap.title = resolve_display_title(
        impl_->module, snap.current ? current_file.c_str() : snap.file.c_str(), stored_title);
    if (snap.current) {
        snap.position_ms = output_time_ms();
        snap.decoder = impl_->decoder.status();
        if (snap.title.empty() && !snap.decoder.title.empty()) {
            snap.title = snap.decoder.title;
        }
    }
    return snap;
}

void Playback::file_info(const char* file, char* title, int* length_ms) const {
    std::string current_file;
    std::string stored_title;
    bool current = false;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mu);
        current_file = impl_->current_file;
        stored_title = impl_->title;
        const std::string asked = canonical_media_path(file);
        const std::string current_path = canonical_media_path(current_file.c_str());
        current = !file || !file[0] ||
                  (!current_path.empty() && _stricmp(asked.c_str(), current_path.c_str()) == 0);
    }

    if (current && !current_file.empty()) {
        if (title) {
            // Resolve outside the mutex: playlist IPC can re-enter GetFileInfo.
            copy_title(title, resolve_display_title(impl_->module, current_file.c_str(), stored_title));
        }
        if (length_ms) {
            *length_ms = impl_->duration_ms.load();
        }
        return;
    }
    if (title) {
        copy_title(title, resolve_display_title(impl_->module, file, {}));
    }
    if (length_ms) {
        *length_ms = -1;
    }
}

void Playback::set_volume(int volume) {
    std::lock_guard<std::mutex> lock(impl_->out_mu);
    if (impl_->module && impl_->module->outMod && impl_->module->outMod->SetVolume) {
        impl_->module->outMod->SetVolume(volume);
    }
}

void Playback::set_pan(int pan) {
    std::lock_guard<std::mutex> lock(impl_->out_mu);
    if (impl_->module && impl_->module->outMod && impl_->module->outMod->SetPan) {
        impl_->module->outMod->SetPan(pan);
    }
}

bool Playback::running() const {
    return impl_->running.load();
}

std::string Playback::last_error() const {
    std::lock_guard<std::mutex> lock(impl_->state_mu);
    return impl_->error;
}

void Playback::thread_main(std::string filename, uint32_t play_generation) {
    struct ClearRunning {
        std::atomic<bool>& flag;
        ~ClearRunning() { flag.store(false); }
    } clear{impl_->running};

    try {
        (void)filename;
        const Settings settings = current_settings();
        In_Module* mod = impl_->module;
        FfmpegDecoder& decoder = impl_->decoder;

        if (decode_only_bisect()) {
            LOG_INFO("decode-only bisect enabled (no Write, no visualisation)");
        } else if (!vis_enabled()) {
            LOG_INFO("visualisation disabled (IN_HLS_NO_VIS=1)");
        } else if (sa_only_bisect()) {
            LOG_INFO("visualisation: SA only (IN_HLS_SA_ONLY=1)");
        } else {
            LOG_INFO("visualisation enabled (SA + VSA)");
        }

        std::vector<uint8_t> pending;
        // in_opus / in_ffsox: one long-lived block, twice 576 frames so DSP
        // can expand. Winamp vis reads 576 frames with no length — only call
        // SA/VSA when this block is full.
        constexpr int k_vis_frames = 576;
        const int max_channels = 2;
        std::vector<int16_t> sample_buffer(
            static_cast<size_t>(k_vis_frames) * static_cast<size_t>(max_channels) * 2u);
        bool decoder_eof = false;
        bool posted_eof = false;
        DWORD last_info = 0;
        int decode_pos_ms = 0;
        // Cap the ring so stop does not leave megabytes of pending PCM.
        // Keep at least a few 576-frame blocks; cap buffer_ms at 2s.
        const int buffer_ms = std::max(100, std::min(settings.buffer_ms, 2000));
        const size_t capacity = std::max<size_t>(
            static_cast<size_t>(k_vis_frames) * 2u * 2u * 8u,
            static_cast<size_t>(buffer_ms) * 48000u * 2u * 2u / 1000u);
        AudioBuffer buffer(capacity);

        auto generation_live = [&]() {
            return impl_->play_generation.load() == play_generation;
        };

        auto fail = [&](const std::string& message) {
            if (impl_->stop.load()) {
                return;
            }
            std::lock_guard<std::mutex> lock(impl_->state_mu);
            impl_->error = message;
            LOG_INFO("%s", message.c_str());
        };

        while (!impl_->stop.load() && generation_live()) {
            if (impl_->paused.load()) {
                std::unique_lock<std::mutex> lock(impl_->state_mu);
                impl_->cv.wait(lock, [&] {
                    return impl_->stop.load() || !impl_->paused.load() || impl_->seek_to.load() >= 0;
                });
                if (impl_->stop.load() || !generation_live()) {
                    break;
                }
            }

            const int seek_ms = impl_->seek_to.exchange(-1);
            if (seek_ms >= 0) {
                if (decoder.seekable()) {
                    std::string seek_error;
                    if (decoder.seek(seek_ms, seek_error)) {
                        buffer.clear();
                        pending.clear();
                        decode_pos_ms = seek_ms;
                        decoder_eof = false;
                        if (generation_live() && impl_->output_open.load() && mod->outMod->Flush) {
                            mod->outMod->Flush(seek_ms);
                        }
                    }
                }
            }

            if (!pending.empty()) {
                size_t off = 0;
                while (off < pending.size() && !impl_->stop.load()) {
                    const size_t wrote = buffer.write(pending.data() + off, pending.size() - off);
                    if (wrote == 0) {
                        break;
                    }
                    off += wrote;
                }
                if (off > 0 && off < pending.size()) {
                    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(off));
                } else if (off == pending.size()) {
                    pending.clear();
                }
            }

            const bool bisect = decode_only_bisect();
            bool output_blocked = false;

            if (impl_->output_open.load() && generation_live()) {
                const int channels = std::max(1, impl_->channels.load());
                const int block = channels * 2;
                size_t available = buffer.size();
                available -= available % static_cast<size_t>(block);

                // Match in_opus during play: full 576-frame blocks only.
                // At EOF, flush a short remainder so VOD/tests can finish.
                //
                // Winamp wires dsp_isactive/dsp_dosamples to its built-in EQ:
                //   0 = nothing, 1 = DSP plug-in (may expand up to 2x),
                //   2 = EQ/replaygain only (same length). Only ×2 CanWrite
                // for a real DSP plug-in — EQ must still run via dsp_dosamples.
                int dsp_active = 0;
                if (!bisect && mod->dsp_isactive) {
                    dsp_active = mod->dsp_isactive();
                }
                const int dsp_factor = (dsp_active == 1) ? 2 : 1;
                const int full_bytes = k_vis_frames * block;
                int can_write = 0;
                if (!bisect) {
                    std::lock_guard<std::mutex> lock(impl_->out_mu);
                    if (generation_live() && impl_->output_open.load()) {
                        can_write = mod->outMod->CanWrite();
                    }
                }

                int frames = 0;
                if (static_cast<int>(available) >= full_bytes) {
                    if (bisect || can_write >= full_bytes * dsp_factor) {
                        frames = k_vis_frames;
                    } else {
                        output_blocked = true;
                        Sleep(20);
                    }
                } else if (decoder_eof && available >= static_cast<size_t>(block)) {
                    const int rem = static_cast<int>(available / static_cast<size_t>(block));
                    if (bisect || can_write >= rem * block * dsp_factor) {
                        frames = rem;
                    } else {
                        output_blocked = true;
                        Sleep(20);
                    }
                } else if (!decoder_eof && available > 0) {
                    // Waiting to accumulate a full 576-frame block — yield.
                    Sleep(5);
                }

                if (frames > 0) {
                    const int chunk_bytes = frames * block;
                    const size_t channel_count = static_cast<size_t>(channels);
                    if (sample_buffer.size() < static_cast<size_t>(frames) * channel_count * 2u) {
                        sample_buffer.resize(static_cast<size_t>(frames) * channel_count * 2u);
                    }
                    buffer.read(reinterpret_cast<uint8_t*>(sample_buffer.data()), static_cast<size_t>(chunk_bytes));

                    // in_opus: vis on the raw PCM block before DSP, and only
                    // for a full 576-frame chunk (Winamp reads that many).
                    if (!bisect && vis_enabled() && frames == k_vis_frames) {
                        if (mod->SAAddPCMData) {
                            mod->SAAddPCMData(
                                sample_buffer.data(), channels, 16, decode_pos_ms);
                        }
                        if (!sa_only_bisect() && mod->VSAAddPCMData) {
                            mod->VSAAddPCMData(
                                sample_buffer.data(), channels, 16, decode_pos_ms);
                        }
                    }

                    int samples = frames;
                    // Always invoke dsp_dosamples when Winamp provided it.
                    // Built-in EQ is applied inside that call (see Winamp In.cpp
                    // eq_dosamples); skipping when isactive()==0 would also skip
                    // the first EQ enable until the next check, and some builds
                    // only settle filter state on the dosamples path.
                    if (!bisect && mod->dsp_dosamples) {
                        samples = mod->dsp_dosamples(
                            sample_buffer.data(),
                            samples,
                            16,
                            channels,
                            impl_->sample_rate.load());
                        if (samples < 0) {
                            samples = 0;
                        }
                        const size_t max_shaped = sample_buffer.size() / channel_count;
                        if (static_cast<size_t>(samples) > max_shaped) {
                            samples = static_cast<int>(max_shaped);
                        }
                    }
                    int shaped_bytes = samples * block;
                    if (shaped_bytes > 8192) {
                        shaped_bytes = 8192 - (8192 % block);
                    }
                    if (shaped_bytes > 0) {
                        if (bisect) {
                            static DWORD last_bisect_log = 0;
                            static size_t discarded = 0;
                            discarded += static_cast<size_t>(shaped_bytes);
                            const DWORD now_bisect = GetTickCount();
                            if (now_bisect - last_bisect_log > 2000) {
                                LOG_INFO(
                                    "decode-only bisect: discarded %u bytes so far",
                                    static_cast<unsigned>(discarded));
                                last_bisect_log = now_bisect;
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(impl_->out_mu);
                            if (generation_live() && impl_->output_open.load()) {
                                mod->outMod->Write(
                                    reinterpret_cast<char*>(sample_buffer.data()), shaped_bytes);
                            }
                            const int rate = std::max(1, impl_->sample_rate.load());
                            decode_pos_ms += MulDiv(shaped_bytes / block, 1000, rate);
                        }
                    }

                    const DWORD now = GetTickCount();
                    if (now - last_info > 500) {
                        const int kbps = decoder.bitrate_kbps();
                        if (kbps > 0) {
                            impl_->bitrate_kbps.store(kbps);
                        }
                        set_winamp_info(
                            mod, kbps, impl_->sample_rate.load(), channels);
                        last_info = now;
                    }
                }
            }

            // Decode when the ring has room. If output is backed up, only
            // top up when below a quarter full so stop does not face a huge ring.
            const size_t room_threshold = buffer.capacity() / 2;
            const bool ring_has_room = pending.empty() && !decoder_eof && buffer.size() < room_threshold;
            const bool should_decode =
                ring_has_room && (bisect || !output_blocked || buffer.size() < (room_threshold / 2));
            if (should_decode) {
                std::string read_error;
                const int rc = decoder.read(pending, read_error);
                if (rc == 1) {
                    // Output was opened on the Play() thread.
                } else if (rc == 0) {
                    decoder_eof = true;
                    pending.clear();
                } else if (rc == -2 || impl_->stop.load()) {
                    break;
                } else {
                    fail(read_error.empty() ? "Decode failed" : read_error);
                    break;
                }
            }

            if (decoder_eof && pending.empty() && buffer.size() < static_cast<size_t>(std::max(1, impl_->channels.load()) * 2)) {
                bool resume_for_seek = false;
                for (int spins = 0; spins < 1500 && !impl_->stop.load() && generation_live(); ++spins) {
                    if (impl_->seek_to.load() >= 0) {
                        resume_for_seek = true;
                        break;
                    }
                    if (impl_->paused.load()) {
                        std::unique_lock<std::mutex> lock(impl_->state_mu);
                        impl_->cv.wait_for(lock, std::chrono::milliseconds(20), [&] {
                            return impl_->stop.load() || !impl_->paused.load() || impl_->seek_to.load() >= 0;
                        });
                        continue;
                    }
                    int playing = 0;
                    if (generation_live() && impl_->output_open.load() && mod->outMod->IsPlaying) {
                        playing = mod->outMod->IsPlaying();
                    }
                    if (!playing) {
                        break;
                    }
                    // Some output drivers need CanWrite while draining.
                    if (generation_live() && impl_->output_open.load() && mod->outMod->CanWrite) {
                        mod->outMod->CanWrite();
                    }
                    Sleep(20);
                }
                if (resume_for_seek || impl_->seek_to.load() >= 0) {
                    continue;
                }
                if (!impl_->stop.load() && generation_live() && mod->hMainWindow) {
                    PostMessage(mod->hMainWindow, WM_WA_MPEG_EOF, 0, 0);
                    posted_eof = true;
                }
                break;
            }

            if (!decoder_eof && buffer.size() >= buffer.capacity() / 2) {
                Sleep(5);
            } else if (!impl_->output_open.load()) {
                Sleep(5);
            }
        }

        if (!posted_eof && !impl_->stop.load() && generation_live() && !decoder_eof && mod->hMainWindow) {
            PostMessage(mod->hMainWindow, WM_WA_MPEG_EOF, 0, 0);
        }
        LOG_INFO("playback thread stopped gen=%u", static_cast<unsigned>(play_generation));
    } catch (const std::exception& ex) {
        LOG_INFO("playback exception: %s", ex.what());
    } catch (...) {
        LOG_INFO("playback exception");
    }
}
