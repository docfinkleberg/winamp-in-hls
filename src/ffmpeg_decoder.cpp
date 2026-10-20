#include "ffmpeg_decoder.h"

#include "log.h"
#include "url_util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

struct FfmpegDecoderState;

namespace {

std::mutex g_log_mu;
FfmpegDecoderState* g_log_target = nullptr;
void (*g_previous_av_log)(void*, int, const char*, va_list) = nullptr;

std::string av_error(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

std::string metadata_string(const AVDictionary* dict, const char* key) {
    if (!dict || !key) {
        return {};
    }
    const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
    if (!entry || !entry->value || !entry->value[0]) {
        return {};
    }
    return entry->value;
}

std::string extract_opening_url(const char* line) {
    if (!line || !line[0]) {
        return {};
    }
    const char* keys[] = {
        "Opening '",
        "Opening \"",
        "opening '",
        "opening \"",
        "HLS request for url '",
        "HLS request for url \"",
        "hls request for url '",
    };
    const char* start = nullptr;
    char quote = '\0';
    for (const char* key : keys) {
        start = std::strstr(line, key);
        if (start) {
            start += std::strlen(key);
            quote = key[std::strlen(key) - 1];
            break;
        }
    }
    if (!start || !start[0]) {
        // Fallback: first http(s) URL on the line.
        const char* http = std::strstr(line, "http://");
        const char* https = std::strstr(line, "https://");
        start = http;
        if (https && (!http || https < http)) {
            start = https;
        }
        if (!start) {
            return {};
        }
        const char* end = start;
        while (*end && !std::isspace(static_cast<unsigned char>(*end)) && *end != '\'' && *end != '"') {
            ++end;
        }
        return std::string(start, end);
    }
    const char* end = start;
    while (*end && *end != quote) {
        ++end;
    }
    std::string url(start, end);
    if (url.find("http://") == 0 || url.find("https://") == 0) {
        return url;
    }
    return {};
}

void note_opened_url(FfmpegDecoderState* impl, const std::string& url);
void dispatch_ffmpeg_log_line(const char* line);

void av_log_capture(void* ptr, int level, const char* fmt, va_list vl) {
    if (g_previous_av_log) {
        va_list copy;
        va_copy(copy, vl);
        g_previous_av_log(ptr, level, fmt, copy);
        va_end(copy);
    }
    // HLS segment opens are often logged at INFO/VERBOSE/DEBUG depending on build.
    if (level > AV_LOG_DEBUG || !fmt) {
        return;
    }
    char line[1024] = {};
    vsnprintf(line, sizeof(line), fmt, vl);
    dispatch_ffmpeg_log_line(line);
}

}  // namespace

struct FfmpegDecoderState {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVChannelLayout out_layout{};
    bool out_layout_set = false;
    int stream_index = -1;
    int sample_rate = 0;
    int channels = 0;
    int swr_format = -1;
    std::atomic<int> bitrate_kbps{0};
    int64_t duration_ms = -1;
    bool seekable = false;
    bool eof = false;
    bool draining = false;
    // Rolling estimate when codecpar/format bit_rate is missing (common on HLS).
    int64_t estimate_bytes = 0;
    int64_t estimate_duration_us = 0;
    std::string title;
    std::string source_url;
    std::string format_name;
    std::string codec_name;
    std::string last_segment_url;
    std::atomic<uint64_t> packets_read{0};
    std::atomic<uint64_t> segments_seen{0};
    mutable std::mutex status_mu;
    std::string last_error;
    std::atomic<bool> local_stop{false};
    std::atomic<bool>* external_stop = nullptr;
    DecoderLogFn log_hook = nullptr;
    void* log_hook_user = nullptr;

    bool stopping() const {
        if (local_stop.load(std::memory_order_acquire)) {
            return true;
        }
        return external_stop && external_stop->load(std::memory_order_acquire);
    }
};

namespace {

void note_opened_url(FfmpegDecoderState* impl, const std::string& url) {
    if (!impl || url.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl->status_mu);
    if (url == impl->source_url) {
        return;  // master/media playlist open — not a media segment
    }
    if (url == impl->last_segment_url) {
        return;
    }
    // Prefer MPEG-TS / fragment-looking URLs over replays of the .m3u8 playlist.
    const bool playlist_like = url.find(".m3u8") != std::string::npos || url.find(".m3u9") != std::string::npos;
    if (playlist_like && !impl->last_segment_url.empty()) {
        return;
    }
    impl->last_segment_url = url;
    impl->segments_seen.fetch_add(1, std::memory_order_relaxed);
}

void dispatch_ffmpeg_log_line(const char* line) {
    if (!line || !line[0] || line[0] == '\n') {
        return;
    }
    DecoderLogFn hook = nullptr;
    void* hook_user = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_log_mu);
        if (!g_log_target) {
            return;
        }
        const std::string url = extract_opening_url(line);
        if (!url.empty()) {
            note_opened_url(g_log_target, url);
        }
        hook = g_log_target->log_hook;
        hook_user = g_log_target->log_hook_user;
    }
    if (hook) {
        hook(line, hook_user);
    }
}

void install_log_capture(FfmpegDecoderState* impl) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    if (!g_previous_av_log) {
        g_previous_av_log = av_log_default_callback;
        av_log_set_callback(av_log_capture);
    }
    g_log_target = impl;
}

void uninstall_log_capture(FfmpegDecoderState* impl) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    if (g_log_target == impl) {
        g_log_target = nullptr;
    }
}

}  // namespace

namespace {

int interrupt_callback(void* opaque) {
    auto* impl = static_cast<FfmpegDecoderState*>(opaque);
    return impl && impl->stopping() ? 1 : 0;
}

bool ensure_resampler(FfmpegDecoderState* impl, const AVFrame* frame, std::string& error) {
    const int rate = frame->sample_rate > 0 ? frame->sample_rate : impl->codec->sample_rate;
    AVChannelLayout input{};
    if (av_channel_layout_copy(&input, &frame->ch_layout) < 0 || input.nb_channels <= 0) {
        av_channel_layout_uninit(&input);
        const int guessed = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : impl->codec->ch_layout.nb_channels;
        av_channel_layout_default(&input, guessed > 0 ? guessed : 2);
    }
    const int out_channels = input.nb_channels > 1 ? 2 : 1;
    const bool same = impl->swr && impl->sample_rate == rate && impl->channels == out_channels &&
                      frame->format == impl->swr_format;
    if (same) {
        av_channel_layout_uninit(&input);
        return true;
    }
    if (impl->swr) {
        swr_free(&impl->swr);
    }
    if (impl->out_layout_set) {
        av_channel_layout_uninit(&impl->out_layout);
        impl->out_layout_set = false;
    }
    av_channel_layout_default(&impl->out_layout, out_channels);
    impl->out_layout_set = true;
    const int created = swr_alloc_set_opts2(
        &impl->swr,
        &impl->out_layout,
        AV_SAMPLE_FMT_S16,
        rate,
        &input,
        static_cast<AVSampleFormat>(frame->format),
        rate,
        0,
        nullptr);
    av_channel_layout_uninit(&input);
    if (created < 0 || !impl->swr || swr_init(impl->swr) < 0) {
        error = "Could not initialise the sample converter";
        return false;
    }
    impl->sample_rate = rate;
    impl->channels = out_channels;
    impl->swr_format = frame->format;
    return true;
}

bool convert_planar_float(FfmpegDecoderState* impl, const AVFrame* frame, std::vector<uint8_t>& pcm) {
    // Same idea as in_ffsox samples_copy_f32p: no swresample for the common
    // HE-AAC path (planar float at the stream rate).
    const int channels = impl->channels;
    const int frames = frame->nb_samples;
    pcm.resize(static_cast<size_t>(frames) * static_cast<size_t>(channels) * 2u);
    auto* out = reinterpret_cast<int16_t*>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            const float* plane = reinterpret_cast<const float*>(frame->extended_data[ch]);
            float sample = plane[i];
            if (sample > 1.0f) {
                sample = 1.0f;
            } else if (sample < -1.0f) {
                sample = -1.0f;
            }
            out[i * channels + ch] = static_cast<int16_t>(sample * 32767.0f);
        }
    }
    return true;
}

bool convert_frame(FfmpegDecoderState* impl, const AVFrame* frame, std::vector<uint8_t>& pcm, std::string& error) {
    if (frame->nb_samples <= 0 || frame->nb_samples > 65536) {
        error = "Unexpected audio frame size";
        return false;
    }

    int raw_channels = frame->ch_layout.nb_channels;
    if (raw_channels <= 0 && impl->codec) {
        raw_channels = impl->codec->ch_layout.nb_channels;
    }
    const int out_channels = raw_channels > 1 ? 2 : (raw_channels > 0 ? 1 : 0);
    if (out_channels < 1) {
        error = "Unexpected channel count";
        return false;
    }
    const int rate = frame->sample_rate > 0 ? frame->sample_rate : (impl->codec ? impl->codec->sample_rate : 0);
    if (rate > 0) {
        impl->sample_rate = rate;
    }
    impl->channels = out_channels;

    // Fast path used by in_ffsox for decoded AAC frames (no channel remix).
    if (frame->format == AV_SAMPLE_FMT_FLTP && frame->extended_data &&
        frame->ch_layout.nb_channels == out_channels) {
        return convert_planar_float(impl, frame, pcm);
    }

    if (!ensure_resampler(impl, frame, error)) {
        return false;
    }
    int out_samples = swr_get_out_samples(impl->swr, frame->nb_samples);
    if (out_samples < frame->nb_samples) {
        out_samples = frame->nb_samples;
    }
    out_samples += 32;

    uint8_t* planes[AV_NUM_DATA_POINTERS] = {};
    int linesize = 0;
    const int alloc = av_samples_alloc(
        planes,
        &linesize,
        out_channels,
        out_samples,
        AV_SAMPLE_FMT_S16,
        0);
    if (alloc < 0 || !planes[0]) {
        error = "Could not allocate a conversion buffer: " + av_error(alloc);
        return false;
    }

    const uint8_t* input_planes[AV_NUM_DATA_POINTERS] = {};
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
        input_planes[i] = frame->extended_data ? frame->extended_data[i] : nullptr;
    }
    const int converted = swr_convert(
        impl->swr,
        planes,
        out_samples,
        input_planes,
        frame->nb_samples);
    if (converted < 0) {
        error = "Sample conversion failed: " + av_error(converted);
        av_freep(&planes[0]);
        return false;
    }

    const int produced = std::min(converted, out_samples);
    const int bytes = produced > 0
                          ? av_samples_get_buffer_size(nullptr, out_channels, produced, AV_SAMPLE_FMT_S16, 1)
                          : 0;
    if (bytes > 0) {
        pcm.resize(static_cast<size_t>(bytes));
        std::memcpy(pcm.data(), planes[0], static_cast<size_t>(bytes));
    } else {
        pcm.clear();
    }
    av_freep(&planes[0]);
    return true;
}

}  // namespace

FfmpegDecoder::FfmpegDecoder() : impl_(new FfmpegDecoderState) {}

FfmpegDecoder::~FfmpegDecoder() {
    close();
    delete impl_;
    impl_ = nullptr;
}

void FfmpegDecoder::set_stop_flag(std::atomic<bool>* flag) {
    impl_->external_stop = flag;
}

void FfmpegDecoder::request_stop() {
    impl_->local_stop.store(true, std::memory_order_release);
}

bool FfmpegDecoder::open(const std::string& url, const DecoderSettings& settings, std::string& error) {
    close();
    impl_->local_stop.store(false, std::memory_order_release);
    if (url.empty()) {
        error = "Empty address";
        return false;
    }

    impl_->packet = av_packet_alloc();
    impl_->frame = av_frame_alloc();
    impl_->fmt = avformat_alloc_context();
    if (!impl_->packet || !impl_->frame || !impl_->fmt) {
        error = "Out of memory";
        close();
        return false;
    }
    impl_->fmt->interrupt_callback.callback = interrupt_callback;
    impl_->fmt->interrupt_callback.opaque = impl_;
    impl_->fmt->probesize = 1 << 20;
    impl_->fmt->max_analyze_duration = 3LL * AV_TIME_BASE;

    AVDictionary* options = nullptr;
    const std::string timeout_us = std::to_string(static_cast<long long>(settings.timeout_ms) * 1000LL);
    av_dict_set(&options, "rw_timeout", timeout_us.c_str(), 0);
    av_dict_set(&options, "timeout", timeout_us.c_str(), 0);
    av_dict_set(&options, "connect_timeout", timeout_us.c_str(), 0);
    av_dict_set(&options, "user_agent", settings.user_agent.c_str(), 0);
    av_dict_set(&options, "multiple_requests", "1", 0);
    av_dict_set(&options, "allowed_extensions", "ALL", 0);
    if (settings.reconnect) {
        av_dict_set(&options, "reconnect", "1", 0);
        av_dict_set(&options, "reconnect_streamed", "1", 0);
        av_dict_set(&options, "reconnect_on_network_error", "1", 0);
        av_dict_set(&options, "reconnect_delay_max", "2", 0);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->status_mu);
        impl_->source_url = url;
        impl_->format_name.clear();
        impl_->codec_name.clear();
        impl_->last_segment_url.clear();
        impl_->packets_read.store(0);
        impl_->segments_seen.store(0);
    }
    install_log_capture(impl_);
    // HLS/HTTP "Opening '...'" lines are often VERBOSE/DEBUG.
    av_log_set_level(AV_LOG_VERBOSE);

    LOG_INFO("open %s", url.c_str());
    const int opened = avformat_open_input(&impl_->fmt, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (opened < 0) {
        impl_->fmt = nullptr;
        if (impl_->stopping()) {
            error = "Stopped";
            close();
            return false;
        }
        error = "Could not open the stream: " + av_error(opened);
        LOG_INFO("%s", error.c_str());
        close();
        return false;
    }

    const int info = avformat_find_stream_info(impl_->fmt, nullptr);
    if (info < 0) {
        if (impl_->stopping()) {
            error = "Stopped";
        } else {
            error = "Could not read stream information: " + av_error(info);
        }
        LOG_INFO("%s", error.c_str());
        close();
        return false;
    }

    const int index = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        error = "The playlist does not contain an audio stream";
        LOG_INFO("%s", error.c_str());
        close();
        return false;
    }
    impl_->stream_index = index;
    AVStream* stream = impl_->fmt->streams[index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        error = "No decoder is available for this audio codec";
        LOG_INFO("%s", error.c_str());
        close();
        return false;
    }
    impl_->codec = avcodec_alloc_context3(decoder);
    if (!impl_->codec || avcodec_parameters_to_context(impl_->codec, stream->codecpar) < 0) {
        error = "Could not create the decoder";
        close();
        return false;
    }
    impl_->codec->pkt_timebase = stream->time_base;
    const int codec_ok = avcodec_open2(impl_->codec, decoder, nullptr);
    if (codec_ok < 0) {
        error = "Could not open the decoder: " + av_error(codec_ok);
        LOG_INFO("%s", error.c_str());
        close();
        return false;
    }

    impl_->duration_ms = -1;
    if (impl_->fmt->duration != AV_NOPTS_VALUE && impl_->fmt->duration > 0) {
        impl_->duration_ms = av_rescale(impl_->fmt->duration, 1000, AV_TIME_BASE);
    } else if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        const int64_t us = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
        if (us > 0) {
            impl_->duration_ms = av_rescale(us, 1000, AV_TIME_BASE);
        }
    }
    impl_->seekable = impl_->duration_ms > 0;
    impl_->estimate_bytes = 0;
    impl_->estimate_duration_us = 0;
    if (stream->codecpar->bit_rate > 0) {
        impl_->bitrate_kbps.store(static_cast<int>(stream->codecpar->bit_rate / 1000));
    } else if (impl_->fmt->bit_rate > 0) {
        impl_->bitrate_kbps.store(static_cast<int>(impl_->fmt->bit_rate / 1000));
    } else {
        // HLS often exposes the variant bandwidth as metadata (bits/sec).
        std::string variant = metadata_string(stream->metadata, "variant_bitrate");
        if (variant.empty()) {
            variant = metadata_string(impl_->fmt->metadata, "variant_bitrate");
        }
        if (!variant.empty()) {
            const long long bits = std::strtoll(variant.c_str(), nullptr, 10);
            if (bits > 0) {
                impl_->bitrate_kbps.store(static_cast<int>(bits / 1000));
            }
        }
    }
    // Needed before the first decoded frame so Play() can open the output
    // on Winamp's thread, as in_opus does.
    impl_->sample_rate = impl_->codec->sample_rate > 0 ? impl_->codec->sample_rate : stream->codecpar->sample_rate;
    {
        const int raw_channels = impl_->codec->ch_layout.nb_channels > 0
                                     ? impl_->codec->ch_layout.nb_channels
                                     : stream->codecpar->ch_layout.nb_channels;
        impl_->channels = raw_channels > 1 ? 2 : (raw_channels > 0 ? 1 : 0);
    }

    std::string title = metadata_string(stream->metadata, "title");
    if (title.empty()) {
        title = metadata_string(impl_->fmt->metadata, "title");
    }
    if (title.empty()) {
        title = metadata_string(impl_->fmt->metadata, "icy-name");
    }
    if (!title.empty()) {
        std::string acp = utf8_to_acp(title);
        impl_->title = acp.empty() ? title : acp;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->status_mu);
        if (impl_->fmt->iformat && impl_->fmt->iformat->name) {
            impl_->format_name = impl_->fmt->iformat->name;
        }
        if (decoder->long_name && decoder->long_name[0]) {
            impl_->codec_name = decoder->long_name;
        } else if (decoder->name) {
            impl_->codec_name = decoder->name;
        }
    }

    LOG_INFO(
        "ready rate=%d channels=%d duration_ms=%lld seekable=%d codec=%s",
        impl_->codec->sample_rate,
        impl_->codec->ch_layout.nb_channels,
        static_cast<long long>(impl_->duration_ms),
        impl_->seekable ? 1 : 0,
        decoder->name ? decoder->name : "?");
    return true;
}

void FfmpegDecoder::set_log_hook(DecoderLogFn fn, void* user) {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_log_mu);
    impl_->log_hook = fn;
    impl_->log_hook_user = user;
}

void FfmpegDecoder::close() {
    if (!impl_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_log_mu);
        impl_->log_hook = nullptr;
        impl_->log_hook_user = nullptr;
    }
    uninstall_log_capture(impl_);
    if (impl_->swr) {
        swr_free(&impl_->swr);
    }
    if (impl_->out_layout_set) {
        av_channel_layout_uninit(&impl_->out_layout);
        impl_->out_layout_set = false;
    }
    if (impl_->frame) {
        av_frame_free(&impl_->frame);
    }
    if (impl_->packet) {
        av_packet_free(&impl_->packet);
    }
    if (impl_->codec) {
        avcodec_free_context(&impl_->codec);
    }
    if (impl_->fmt) {
        avformat_close_input(&impl_->fmt);
    }
    impl_->stream_index = -1;
    impl_->sample_rate = 0;
    impl_->channels = 0;
    impl_->bitrate_kbps.store(0);
    impl_->duration_ms = -1;
    impl_->seekable = false;
    impl_->eof = false;
    impl_->draining = false;
    impl_->estimate_bytes = 0;
    impl_->estimate_duration_us = 0;
    impl_->swr_format = -1;
    impl_->title.clear();
    {
        std::lock_guard<std::mutex> lock(impl_->status_mu);
        impl_->source_url.clear();
        impl_->format_name.clear();
        impl_->codec_name.clear();
        impl_->last_segment_url.clear();
    }
    impl_->packets_read.store(0);
    impl_->segments_seen.store(0);
}

int FfmpegDecoder::read(std::vector<uint8_t>& pcm, std::string& error) {
    pcm.clear();
    if (!impl_->fmt || !impl_->codec) {
        error = "Decoder is not open";
        return -1;
    }
    while (!impl_->eof) {
        if (impl_->stopping()) {
            error = "Stopped";
            return -2;
        }
        const int received = avcodec_receive_frame(impl_->codec, impl_->frame);
        if (received == 0) {
            const bool converted = convert_frame(impl_, impl_->frame, pcm, error);
            av_frame_unref(impl_->frame);
            if (!converted) {
                return -1;
            }
            if (!pcm.empty()) {
                return 1;
            }
            continue;
        }
        if (received == AVERROR_EOF) {
            impl_->eof = true;
            return 0;
        }
        if (received != AVERROR(EAGAIN)) {
            error = "Decode failed: " + av_error(received);
            return impl_->stopping() ? -2 : -1;
        }
        if (impl_->draining) {
            impl_->eof = true;
            return 0;
        }

        const int got = av_read_frame(impl_->fmt, impl_->packet);
        if (got == AVERROR_EOF) {
            avcodec_send_packet(impl_->codec, nullptr);
            impl_->draining = true;
            continue;
        }
        if (got < 0) {
            if (impl_->stopping() || got == AVERROR_EXIT) {
                error = "Stopped";
                return -2;
            }
            error = "Read failed: " + av_error(got);
            LOG_INFO("%s", error.c_str());
            return -1;
        }
        if (impl_->packet->stream_index != impl_->stream_index) {
            av_packet_unref(impl_->packet);
            continue;
        }
        impl_->packets_read.fetch_add(1, std::memory_order_relaxed);
        // Rolling content bitrate from compressed packet sizes. Always update
        // (not only when the initial variant bit_rate is missing) so VBR / HLS
        // flux shows in SetInfo and the info box, similar to VLC.
        if (impl_->packet->size > 0) {
            impl_->estimate_bytes += impl_->packet->size;
            int64_t packet_us = 0;
            if (impl_->packet->duration > 0) {
                packet_us = av_rescale_q(
                    impl_->packet->duration,
                    impl_->fmt->streams[impl_->stream_index]->time_base,
                    AV_TIME_BASE_Q);
            }
            if (packet_us <= 0 && impl_->codec->sample_rate > 0) {
                // Fallback: assume ~20 ms AAC frames when duration is missing.
                packet_us = 20 * 1000;
            }
            if (packet_us > 0) {
                impl_->estimate_duration_us += packet_us;
            }
            // ~1s media window keeps the displayed kbps responsive without noise.
            if (impl_->estimate_duration_us >= 1000000) {
                const int64_t bps =
                    (impl_->estimate_bytes * 8 * AV_TIME_BASE) / impl_->estimate_duration_us;
                if (bps > 0) {
                    impl_->bitrate_kbps.store(static_cast<int>((bps + 500) / 1000));
                }
                impl_->estimate_bytes = 0;
                impl_->estimate_duration_us = 0;
            }
        }
        const int sent = avcodec_send_packet(impl_->codec, impl_->packet);
        av_packet_unref(impl_->packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            if (impl_->stopping()) {
                error = "Stopped";
                return -2;
            }
            error = "Could not send a packet to the decoder: " + av_error(sent);
            return -1;
        }
    }
    return 0;
}

bool FfmpegDecoder::seek(int64_t position_ms, std::string& error) {
    if (!impl_->fmt || !impl_->seekable || position_ms < 0) {
        error = "This stream cannot be seeked";
        return false;
    }
    const int64_t target = av_rescale(position_ms, AV_TIME_BASE, 1000);
    int result = avformat_seek_file(impl_->fmt, -1, INT64_MIN, target, INT64_MAX, 0);
    if (result < 0) {
        result = av_seek_frame(impl_->fmt, -1, target, AVSEEK_FLAG_BACKWARD);
    }
    if (result < 0) {
        error = "Seek failed: " + av_error(result);
        LOG_INFO("%s", error.c_str());
        return false;
    }
    if (impl_->codec) {
        avcodec_flush_buffers(impl_->codec);
    }
    if (impl_->swr) {
        swr_close(impl_->swr);
        if (swr_init(impl_->swr) < 0) {
            error = "Could not reset the sample converter after seeking";
            return false;
        }
    }
    impl_->eof = false;
    impl_->draining = false;
    LOG_INFO("seek ms=%lld", static_cast<long long>(position_ms));
    return true;
}

int FfmpegDecoder::sample_rate() const { return impl_->sample_rate; }
int FfmpegDecoder::channels() const { return impl_->channels; }
int FfmpegDecoder::bitrate_kbps() const { return impl_->bitrate_kbps.load(); }
int64_t FfmpegDecoder::duration_ms() const { return impl_->duration_ms; }
bool FfmpegDecoder::seekable() const { return impl_->seekable; }
const std::string& FfmpegDecoder::title() const { return impl_->title; }

DecoderStatus FfmpegDecoder::status() const {
    DecoderStatus out;
    if (!impl_ || !impl_->fmt) {
        return out;
    }
    out.open = true;
    out.live = !impl_->seekable || impl_->duration_ms < 0;
    out.sample_rate = impl_->sample_rate;
    out.channels = impl_->channels;
    out.bitrate_kbps = impl_->bitrate_kbps.load();
    out.duration_ms = impl_->duration_ms;
    out.packets_read = impl_->packets_read.load();
    out.segments_seen = impl_->segments_seen.load();
    out.title = impl_->title;
    std::lock_guard<std::mutex> lock(impl_->status_mu);
    out.url = impl_->source_url;
    out.format = impl_->format_name;
    out.codec = impl_->codec_name;
    out.last_segment_url = impl_->last_segment_url;
    return out;
}
