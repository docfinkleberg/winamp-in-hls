#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// Fixed-capacity byte queue. write() copies what fits and returns that count.
// Safe for one producer and one consumer, or any threads, via an internal mutex.
class AudioBuffer {
public:
    explicit AudioBuffer(size_t capacity_bytes);

    size_t write(const uint8_t* src, size_t bytes);
    size_t read(uint8_t* dst, size_t bytes);
    size_t size() const;
    size_t capacity() const;
    void clear();

private:
    mutable std::mutex mu_;
    std::vector<uint8_t> data_;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    size_t size_ = 0;
};
