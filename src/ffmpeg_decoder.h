#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct DecoderSettings {
    int timeout_ms = 15000;
    bool reconnect = true;
    std::string user_agent = "WinampHLS/1.0";
};

struct DecoderStatus {
    bool open = false;
    bool live = false;
    std::string url;
    std::string title;
    std::string format;
    std::string codec;
    std::string last_segment_url;
    int sample_rate = 0;
    int channels = 0;
    int bitrate_kbps = 0;
    int64_t duration_ms = -1;
    uint64_t packets_read = 0;
    uint64_t segments_seen = 0;
};

struct FfmpegDecoderState;

class FfmpegDecoder {
public:
    FfmpegDecoder();
    ~FfmpegDecoder();

    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    void set_stop_flag(std::atomic<bool>* flag);
    void request_stop();

    bool open(const std::string& url, const DecoderSettings& settings, std::string& error);
    void close();

    // 1 = pcm holds one converted block, 0 = end of stream, -1 = error, -2 = stopped.
    int read(std::vector<uint8_t>& pcm, std::string& error);
    bool seek(int64_t position_ms, std::string& error);

    int sample_rate() const;
    int channels() const;
    int bitrate_kbps() const;
    int64_t duration_ms() const;
    bool seekable() const;
    const std::string& title() const;
    DecoderStatus status() const;

private:
    FfmpegDecoderState* impl_;
};
