#include "audio_buffer.h"

#include <algorithm>
#include <cstring>

AudioBuffer::AudioBuffer(size_t capacity_bytes)
    : data_(capacity_bytes == 0 ? 1 : capacity_bytes) {}

size_t AudioBuffer::write(const uint8_t* src, size_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!src || bytes == 0 || data_.empty()) {
        return 0;
    }
    const size_t cap = data_.size();
    const size_t space = cap - size_;
    if (space == 0) {
        return 0;
    }
    if (bytes > space) {
        bytes = space;
    }
    const size_t first = std::min(bytes, cap - write_pos_);
    std::memcpy(data_.data() + write_pos_, src, first);
    if (bytes > first) {
        std::memcpy(data_.data(), src + first, bytes - first);
    }
    write_pos_ += bytes;
    if (write_pos_ >= cap) {
        write_pos_ -= cap;
    }
    size_ += bytes;
    return bytes;
}

size_t AudioBuffer::read(uint8_t* dst, size_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dst || bytes == 0 || size_ == 0) {
        return 0;
    }
    if (bytes > size_) {
        bytes = size_;
    }
    const size_t cap = data_.size();
    const size_t first = std::min(bytes, cap - read_pos_);
    std::memcpy(dst, data_.data() + read_pos_, first);
    if (bytes > first) {
        std::memcpy(dst + first, data_.data(), bytes - first);
    }
    read_pos_ += bytes;
    if (read_pos_ >= cap) {
        read_pos_ -= cap;
    }
    size_ -= bytes;
    return bytes;
}

size_t AudioBuffer::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return size_;
}

size_t AudioBuffer::capacity() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_.size();
}

void AudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    read_pos_ = 0;
    write_pos_ = 0;
    size_ = 0;
}
