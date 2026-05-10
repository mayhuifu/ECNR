#include "core/ring_buffer.h"

#include <algorithm>
#include <cstring>

namespace ecnr {

RingBuffer::RingBuffer(size_t capacity_samples) : data_(capacity_samples) {}

size_t RingBuffer::Write(const int16_t* src, size_t count) {
  const size_t to_write = std::min(count, Available());
  const size_t cap = data_.size();
  const size_t first_chunk = std::min(to_write, cap - head_);
  std::memcpy(data_.data() + head_, src, first_chunk * sizeof(int16_t));
  if (to_write > first_chunk) {
    std::memcpy(data_.data(), src + first_chunk,
                (to_write - first_chunk) * sizeof(int16_t));
  }
  head_ = (head_ + to_write) % cap;
  size_ += to_write;
  return to_write;
}

size_t RingBuffer::Read(int16_t* dst, size_t count) {
  const size_t to_read = std::min(count, size_);
  const size_t cap = data_.size();
  const size_t first_chunk = std::min(to_read, cap - tail_);
  std::memcpy(dst, data_.data() + tail_, first_chunk * sizeof(int16_t));
  if (to_read > first_chunk) {
    std::memcpy(dst + first_chunk, data_.data(),
                (to_read - first_chunk) * sizeof(int16_t));
  }
  tail_ = (tail_ + to_read) % cap;
  size_ -= to_read;
  return to_read;
}

void RingBuffer::Clear() {
  head_ = tail_ = size_ = 0;
}

}  // namespace ecnr
