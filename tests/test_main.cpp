#include "audio_buffer.h"
#include "ffmpeg_decoder.h"
#include "ffmpeg_loader.h"
#include "playback.h"
#include "playlist_file.h"
#include "settings.h"
#include "url_util.h"

#include "in2.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;

void check(bool condition, const char* text, const char* file, int line) {
    if (!condition) {
        std::cerr << "FAIL " << file << ":" << line << " " << text << "\n";
        ++g_failed;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

std::wstring exe_directory() {
    wchar_t path[32768];
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (n == 0) {
        return L".";
    }
    for (DWORD i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i - 1] = L'\0';
            break;
        }
    }
    return path;
}

std::wstring full_path(const std::wstring& path) {
    wchar_t buffer[32768];
    const DWORD n = GetFullPathNameW(path.c_str(), 32768, buffer, nullptr);
    if (n == 0 || n >= 32768) {
        return path;
    }
    return buffer;
}

std::wstring ffmpeg_directory() {
    wchar_t env[32768];
    const DWORD n = GetEnvironmentVariableW(L"IN_HLS_FFMPEG_DIR", env, 32768);
    if (n > 0 && n < 32768) {
        return env;
    }
    return full_path(exe_directory() + L"\\..\\..\\..\\third_party\\ffmpeg\\bin");
}

std::string wide_to_acp(const std::wstring& text) {
    const int bytes = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

void test_audio_buffer() {
    AudioBuffer buffer(8);
    const uint8_t first[] = {1, 2, 3, 4, 5, 6};
    CHECK(buffer.write(first, 6) == 6);
    uint8_t out[8] = {};
    CHECK(buffer.read(out, 4) == 4);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);
    const uint8_t second[] = {7, 8, 9, 10, 11, 12, 13};
    CHECK(buffer.write(second, 7) == 6);
    CHECK(buffer.write(second, 1) == 0);
    CHECK(buffer.size() == 8);
    CHECK(buffer.read(out, 8) == 8);
    const uint8_t expect[] = {5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(std::memcmp(out, expect, 8) == 0);
    CHECK(buffer.size() == 0);
    buffer.write(first, 3);
    buffer.clear();
    CHECK(buffer.size() == 0);
    CHECK(buffer.read(out, 1) == 0);

    AudioBuffer shared(4096);
    std::vector<uint8_t> source(100000);
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<uint8_t>(i * 17u + 3u);
    }
    std::vector<uint8_t> received(source.size());
    std::thread producer([&] {
        size_t off = 0;
        while (off < source.size()) {
            const size_t n = std::min<size_t>(1 + (off % 97), source.size() - off);
            const size_t wrote = shared.write(source.data() + off, n);
            off += wrote;
            if (wrote == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
    size_t got = 0;
    while (got < received.size()) {
        const size_t n = shared.read(received.data() + got, 50);
        got += n;
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    producer.join();
    CHECK(received == source);
}

void test_urls() {
    const char* signed_url =
        "https://cdn.example/live/master.m3u8?token=abc%2Bdef&Signature=aaa%3D%3D";
    CHECK(is_hls_url(signed_url));
    CHECK(to_ffmpeg_input(signed_url) == signed_url);
    CHECK(is_hls_url("http://example.com/a.M3U8"));
    CHECK(is_hls_url("http://example.com/a.m3u8/"));
    CHECK(is_hls_url("https://example.com/path/playlist.m3u8?x=1#frag"));
    CHECK(!is_hls_url("https://example.com/file.mp3?list=master.m3u8"));
    CHECK(!is_hls_url("C:\\music\\playlist.m3u8"));
    CHECK(!is_remote_url("C:\\music\\playlist.m3u8"));
    CHECK(is_remote_url("file://server/share/a.m3u8"));
    CHECK(to_ffmpeg_input("C:\\music\\playlist.m3u8") == "C:\\music\\playlist.m3u8");
    CHECK(claims_hls_name("http://example.com/a.m3u9?x=1"));
    CHECK(claims_hls_name("C:\\music\\playlist.m3u9"));
    CHECK(!claims_hls_name("C:\\music\\playlist.m3u8"));
    CHECK(canonical_media_path("http://example.com/a.m3u9?token=abc%2Bdef") ==
          "http://example.com/a.m3u8?token=abc%2Bdef");
    CHECK(canonical_media_path("C:\\music\\playlist.m3u9") == "C:\\music\\playlist.m3u8");
    CHECK(canonical_media_path(signed_url) == signed_url);
}

void test_playlist_file() {
    CHECK(is_external_playlist_path(L"C:\\playlists\\bbc.m3u"));
    CHECK(is_external_playlist_path(L"C:\\radio\\stations.pls"));
    CHECK(!is_external_playlist_path(L"http://example.com/live.m3u8"));
    CHECK(!is_external_playlist_path(L"http://example.com/live.m3u9"));
    CHECK(!is_external_playlist_path(L"C:\\radio\\live.m3u8"));
    CHECK(!is_external_playlist_path(L"C:\\radio\\live.m3u9"));

    const char* text =
        "#EXTM3U\n"
        "#EXTINF:-1,BBC - Radio 1\n"
        "http://example.com/bbc_radio_one.m3u8\n"
        "#EXTINF:-1,BBC - Radio 2\n"
        "http://example.com/bbc_radio_two.m3u8?x=1\n"
        "# comment\n"
        "http://example.com/no_title.m3u8\n";
    std::vector<PlaylistEntry> entries;
    CHECK(parse_m3u_bytes(text, std::strlen(text), entries));
    CHECK(entries.size() == 3);
    CHECK(entries[0].title == L"BBC - Radio 1");
    CHECK(entries[0].url == L"http://example.com/bbc_radio_one.m3u9");
    CHECK(entries[1].title == L"BBC - Radio 2");
    CHECK(entries[1].url == L"http://example.com/bbc_radio_two.m3u9?x=1");
    CHECK(entries[2].title.empty());
    CHECK(entries[2].url == L"http://example.com/no_title.m3u9");

    const auto temp = std::filesystem::temp_directory_path() / "in_hls_bbc_test.m3u";
    {
        std::ofstream out(temp, std::ios::binary);
        out << text;
    }
    entries.clear();
    CHECK(load_playlist_file(temp.wstring().c_str(), entries));
    CHECK(entries.size() == 3);
    CHECK(entries[0].title == L"BBC - Radio 1");

    std::wstring rewritten;
    CHECK(materialize_rewritten_playlist(temp.wstring().c_str(), rewritten));
    CHECK(!rewritten.empty());
    entries.clear();
    CHECK(load_playlist_file(rewritten.c_str(), entries));
    CHECK(entries.size() == 3);
    CHECK(entries[0].url == L"http://example.com/bbc_radio_one.m3u9");
    CHECK(entries[0].title == L"BBC - Radio 1");
    std::filesystem::remove(temp);
    std::filesystem::remove(rewritten);
}

struct FakeOutput {
    std::mutex mu;
    std::vector<char> pcm;
    int rate = 0;
    int channels = 0;
    int bits = 0;
    int paused = 0;
    std::atomic<int> playing{0};
    std::vector<int> flushes;
    bool opened = false;
    bool ever_opened = false;
    std::atomic<int> write_limit{1 << 20};
    std::atomic<int> vis_calls{0};
    std::atomic<unsigned> vis_checksum{0};
};

FakeOutput g_fake;

int __cdecl fake_open(int samplerate, int numchannels, int bitspersamp, int, int) {
    std::lock_guard<std::mutex> lock(g_fake.mu);
    g_fake.rate = samplerate;
    g_fake.channels = numchannels;
    g_fake.bits = bitspersamp;
    g_fake.opened = true;
    g_fake.ever_opened = true;
    g_fake.pcm.clear();
    return 200;
}
void __cdecl fake_close() {
    std::lock_guard<std::mutex> lock(g_fake.mu);
    g_fake.opened = false;
}
int __cdecl fake_write(char* buf, int len) {
    std::lock_guard<std::mutex> lock(g_fake.mu);
    g_fake.pcm.insert(g_fake.pcm.end(), buf, buf + len);
    return 0;
}
int __cdecl fake_can_write() { return g_fake.write_limit.load(); }
int __cdecl fake_is_playing() { return g_fake.playing.load(); }
int __cdecl fake_pause(int pause) {
    const int previous = g_fake.paused;
    g_fake.paused = pause;
    return previous;
}
void __cdecl fake_set_volume(int) {}
void __cdecl fake_set_pan(int) {}
void __cdecl fake_flush(int t) {
    std::lock_guard<std::mutex> lock(g_fake.mu);
    g_fake.flushes.push_back(t);
}
int __cdecl fake_get_output_time() { return 0; }
int __cdecl fake_get_written_time() { return 0; }

void __cdecl fake_vis(void* data, int channels, int bits, int) {
    const int bytes = 576 * channels * (bits / 8);
    unsigned sum = 0;
    const auto* bytes_in = static_cast<const unsigned char*>(data);
    for (int i = 0; i < bytes; ++i) {
        sum += bytes_in[i];
    }
    g_fake.vis_checksum.fetch_add(sum);
    g_fake.vis_calls.fetch_add(1);
}

void reset_fake() {
    std::lock_guard<std::mutex> lock(g_fake.mu);
    g_fake.pcm.clear();
    g_fake.flushes.clear();
    g_fake.rate = 0;
    g_fake.channels = 0;
    g_fake.bits = 0;
    g_fake.paused = 0;
    g_fake.opened = false;
    g_fake.ever_opened = false;
    g_fake.playing.store(0);
    g_fake.write_limit.store(1 << 20);
    g_fake.vis_calls.store(0);
    g_fake.vis_checksum.store(0);
}

void bind_output(In_Module& module, Out_Module& output) {
    std::memset(&output, 0, sizeof(output));
    output.version = OUT_VER;
    output.Open = fake_open;
    output.Close = fake_close;
    output.Write = fake_write;
    output.CanWrite = fake_can_write;
    output.IsPlaying = fake_is_playing;
    output.Pause = fake_pause;
    output.SetVolume = fake_set_volume;
    output.SetPan = fake_set_pan;
    output.Flush = fake_flush;
    output.GetOutputTime = fake_get_output_time;
    output.GetWrittenTime = fake_get_written_time;
    module.outMod = &output;
}

void write_tone_wav(const std::wstring& path, int rate, int channels, int frames) {
    std::vector<int16_t> samples(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (int i = 0; i < frames; ++i) {
        const double wave = std::sin(2.0 * 3.141592653589793 * 440.0 * i / rate);
        const auto value = static_cast<int16_t>(wave * 12000.0);
        for (int c = 0; c < channels; ++c) {
            samples[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)] = value;
        }
    }
    std::ofstream file(path, std::ios::binary);
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riff_size = 36 + data_bytes;
    const uint16_t audio_format = 1;
    const uint16_t channel_count = static_cast<uint16_t>(channels);
    const uint32_t sample_rate = static_cast<uint32_t>(rate);
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channel_count * bits / 8;
    const uint16_t align = channel_count * bits / 8;
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riff_size), 4);
    file.write("WAVEfmt ", 8);
    const uint32_t fmt_size = 16;
    file.write(reinterpret_cast<const char*>(&fmt_size), 4);
    file.write(reinterpret_cast<const char*>(&audio_format), 2);
    file.write(reinterpret_cast<const char*>(&channel_count), 2);
    file.write(reinterpret_cast<const char*>(&sample_rate), 4);
    file.write(reinterpret_cast<const char*>(&byte_rate), 4);
    file.write(reinterpret_cast<const char*>(&align), 2);
    file.write(reinterpret_cast<const char*>(&bits), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&data_bytes), 4);
    file.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(data_bytes));
}

bool pcm_has_energy(const std::vector<uint8_t>& pcm) {
    for (uint8_t byte : pcm) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

bool wait_until_stopped(Playback& playback, int timeout_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (playback.running()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > timeout_ms) {
            return false;
        }
        Sleep(20);
    }
    return true;
}

}  // namespace

int main() {
    test_audio_buffer();
    test_urls();
    test_playlist_file();
    CHECK(sizeof(void*) == 4);

    const std::wstring ffmpeg_dir = ffmpeg_directory();
    set_ffmpeg_search_directory(ffmpeg_dir);
    std::string error;
    if (!load_ffmpeg_dlls(ffmpeg_dir, error)) {
        std::cerr << "FAIL could not load FFmpeg: " << error << "\n";
        return 1;
    }
    ffmpeg_network_startup();

    const std::filesystem::path work = std::filesystem::path(exe_directory()) / L"testdata";
    std::filesystem::create_directories(work);
    const std::wstring wav_path = (work / L"tone.wav").wstring();
    write_tone_wav(wav_path, 44100, 2, 44100 * 2);
    const std::string wav_acp = wide_to_acp(wav_path);

    {
        FfmpegDecoder decoder;
        DecoderSettings settings;
        settings.timeout_ms = 5000;
        CHECK(decoder.open(to_ffmpeg_input(wav_acp.c_str()), settings, error));
        CHECK(decoder.sample_rate() == 0 || decoder.sample_rate() == 44100);
        std::vector<uint8_t> all;
        for (;;) {
            std::vector<uint8_t> block;
            const int rc = decoder.read(block, error);
            if (rc == 1) {
                all.insert(all.end(), block.begin(), block.end());
                continue;
            }
            CHECK(rc == 0);
            break;
        }
        CHECK(decoder.sample_rate() == 44100);
        CHECK(decoder.channels() == 2);
        CHECK(decoder.duration_ms() > 1500 && decoder.duration_ms() < 2500);
        CHECK(decoder.seekable());
        CHECK(pcm_has_energy(all));
        CHECK(all.size() > 300000);
        CHECK(decoder.seek(1000, error));
        std::vector<uint8_t> after;
        CHECK(decoder.read(after, error) == 1);
        CHECK(!after.empty());
        decoder.close();
        CHECK(decoder.open(to_ffmpeg_input(wav_acp.c_str()), settings, error));
        decoder.close();
    }

    {
        FfmpegDecoder decoder;
        std::atomic<bool> stop{false};
        decoder.set_stop_flag(&stop);
        DecoderSettings settings;
        settings.timeout_ms = 30000;
        settings.reconnect = false;
        const auto start = std::chrono::steady_clock::now();
        std::thread stopper([&] {
            Sleep(300);
            stop.store(true);
            decoder.request_stop();
        });
        const bool opened = decoder.open("http://203.0.113.5/does-not-exist.m3u8", settings, error);
        stopper.join();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        CHECK(!opened);
        CHECK(elapsed.count() < 8000);
        if (elapsed.count() >= 8000) {
            std::cerr << "interrupt took " << elapsed.count() << " ms\n";
        }
        decoder.close();
    }

    Settings playback_settings;
    playback_settings.timeout_ms = 10000;
    playback_settings.buffer_ms = 500;
    playback_settings.reconnect = true;
    playback_settings.log = false;
    set_current_settings(playback_settings);

    In_Module module{};
    Out_Module output{};
    bind_output(module, output);
    {
        reset_fake();
        g_fake.playing.store(1);
        Playback playback(&module);
        CHECK(playback.play(wav_acp.c_str()) == 0);
        Sleep(250);
        playback.pause();
        CHECK(playback.is_paused() == 1);
        playback.unpause();
        CHECK(playback.is_paused() == 0);
        for (int i = 0; i < 40 && playback.length_ms() <= 0; ++i) {
            Sleep(50);
        }
        playback.set_output_time_ms(1000);
        Sleep(300);
        {
            std::lock_guard<std::mutex> lock(g_fake.mu);
            CHECK(!g_fake.flushes.empty());
            if (!g_fake.flushes.empty()) {
                CHECK(g_fake.flushes.back() == 1000);
            }
        }
        g_fake.playing.store(0);
        playback.stop();
        CHECK(!playback.running());
        CHECK(wait_until_stopped(playback, 2000));
    }

    {
        reset_fake();
        // Must fit a full 576-frame stereo block (vis needs that many samples).
        g_fake.write_limit.store(576 * 2 * 2 * 2);
        g_fake.playing.store(1);
        module.SAAddPCMData = fake_vis;
        module.VSAAddPCMData = fake_vis;
        Playback playback(&module);
        CHECK(playback.play(wav_acp.c_str()) == 0);
        for (int i = 0; i < 100 && g_fake.vis_calls.load() == 0; ++i) {
            Sleep(20);
        }
        playback.stop();
        CHECK(wait_until_stopped(playback, 2000));
        CHECK(g_fake.vis_calls.load() > 0);
        CHECK(g_fake.vis_checksum.load() != 0);
        module.SAAddPCMData = nullptr;
        module.VSAAddPCMData = nullptr;
        g_fake.write_limit.store(1 << 20);
    }

    const bool skip_mux = GetEnvironmentVariableW(L"IN_HLS_SKIP_HLS_MUX", nullptr, 0) > 0 ||
                          GetFileAttributesW((ffmpeg_dir + L"\\ffmpeg.exe").c_str()) == INVALID_FILE_ATTRIBUTES;
    if (skip_mux) {
        std::cout << "SKIP local HLS mux (ffmpeg.exe not used for this run)\n";
    } else {
        const std::filesystem::path hls_dir = work / L"hls";
        std::filesystem::remove_all(hls_dir);
        std::filesystem::create_directories(hls_dir);
        std::wstring command = L"\"" + ffmpeg_dir +
            L"\\ffmpeg.exe\" -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 "
            L"-c:a aac -b:a 96k -f hls -hls_time 2 -hls_playlist_type vod "
            L"-hls_segment_filename seg%03d.ts playlist.m3u8";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            hls_dir.c_str(),
            &startup,
            &process);
        CHECK(created);
        if (created) {
            WaitForSingleObject(process.hProcess, 60000);
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CHECK(code == 0);
            if (code == 0) {
                const std::string playlist = wide_to_acp((hls_dir / L"playlist.m3u8").wstring());
                reset_fake();
                Playback playback(&module);
                CHECK(playback.play(playlist.c_str()) == 0);
                CHECK(wait_until_stopped(playback, 20000));
                playback.stop();
                std::lock_guard<std::mutex> lock(g_fake.mu);
                if (g_fake.pcm.size() < 10000) {
                    std::cerr << "HLS playback error: " << playback.last_error() << "\n";
                }
                CHECK(g_fake.ever_opened);
                CHECK(g_fake.rate == 44100);
                CHECK(g_fake.pcm.size() > 10000);
                CHECK(playback.length_ms() > 500);
            }
        }
    }

    const char* supplied = std::getenv("IN_HLS_TEST_URL");
    if (supplied && supplied[0]) {
        FfmpegDecoder decoder;
        DecoderSettings settings;
        settings.timeout_ms = 15000;
        const bool opened = decoder.open(to_ffmpeg_input(supplied), settings, error);
        if (!opened) {
            std::cerr << "supplied URL failed: " << error << "\n";
        }
        CHECK(opened);
        if (opened) {
            std::vector<uint8_t> block;
            size_t total = 0;
            const auto started = std::chrono::steady_clock::now();
            for (;;) {
                const int rc = decoder.read(block, error);
                if (rc != 1) {
                    std::cerr << "supplied URL read rc=" << rc << " " << error << "\n";
                    break;
                }
                total += block.size();
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started);
                if (elapsed.count() >= 8 || total > 48000 * 2 * 2 * 8) {
                    break;
                }
            }
            std::cout << "supplied URL rate=" << decoder.sample_rate()
                      << " channels=" << decoder.channels()
                      << " duration_ms=" << decoder.duration_ms()
                      << " seekable=" << decoder.seekable()
                      << " bytes=" << total << "\n";
            CHECK(decoder.sample_rate() > 0);
            CHECK(decoder.channels() > 0);
            CHECK(total > 10000);
        }
        decoder.close();
    } else {
        std::cout << "SKIP IN_HLS_TEST_URL is not set\n";
    }

    ffmpeg_network_shutdown();
    if (g_failed) {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
