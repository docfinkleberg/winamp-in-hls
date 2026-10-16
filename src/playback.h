#pragma once

#include "ffmpeg_decoder.h"
#include "in2.h"

#include <cstdint>
#include <string>

struct PlaybackInfoSnapshot {
    std::string file;
    std::string title;
    bool playing = false;
    bool paused = false;
    bool current = false;
    int position_ms = 0;
    DecoderStatus decoder;
};

class Playback {
public:
    explicit Playback(In_Module* module);
    ~Playback();

    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    void set_module(In_Module* module);
    int play(const char* filename);
    void pause();
    void unpause();
    int is_paused() const;
    void stop();
    int length_ms() const;
    int output_time_ms() const;
    void set_output_time_ms(int time_ms);
    int written_time_ms() const;
    void file_info(const char* file, char* title, int* length_ms) const;
    PlaybackInfoSnapshot info_snapshot(const char* file) const;
    void set_volume(int volume);
    void set_pan(int pan);
    bool running() const;
    std::string last_error() const;

private:
    struct Impl;
    Impl* impl_;
    void thread_main(std::string filename, uint32_t play_generation);
};
