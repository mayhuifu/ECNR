#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ecnr {

// Single-producer / single-consumer int16 ring buffer for aligning the render
// (far-end reference) stream with the capture (mic) stream. Not thread-safe;
// the AEC chain is single-threaded by design (one frame loop, one core).
class RingBuffer {
 public:
  explicit RingBuffer(size_t capacity_samples);

  size_t Capacity() const { return data_.size(); }
  size_t Size() const { return size_; }
  size_t Available() const { return data_.size() - size_; }
  bool Empty() const { return size_ == 0; }

  // Returns number of samples actually written (may be less than `count` if
  // the buffer fills up; older samples are NOT overwritten).
  size_t Write(const int16_t* src, size_t count);

  // Returns number of samples actually read (may be less than `count` if
  // the buffer doesn't have enough samples).
  size_t Read(int16_t* dst, size_t count);

  void Clear();

 private:
  std::vector<int16_t> data_;
  size_t head_ = 0;  // write index
  size_t tail_ = 0;  // read index
  size_t size_ = 0;
};

}  // namespace ecnr
