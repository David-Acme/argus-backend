#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

class SampleRing
{
public:
  explicit SampleRing(size_t capacity) : buffer_(capacity, 0.0F) {}

  void push(const float* data, size_t count)
  {
    if (count >= buffer_.size()) {
      std::copy(data + count - buffer_.size(), data + count, buffer_.begin());
      head_ = 0;
      size_ = buffer_.size();
      return;
    }
    for (size_t i = 0; i < count; ++i) {
      buffer_[(head_ + size_) % buffer_.size()] = data[i];
      if (size_ == buffer_.size())
        head_ = (head_ + 1) % buffer_.size();
      else
        ++size_;
    }
  }

  bool pop(float* dst, size_t count)
  {
    if (count > size_)
      return false;
    for (size_t i = 0; i < count; ++i)
      dst[i] = buffer_[(head_ + i) % buffer_.size()];
    head_ = (head_ + count) % buffer_.size();
    size_ -= count;
    return true;
  }

  size_t size() const { return size_; }
  size_t capacity() const { return buffer_.size(); }
  void clear() { head_ = 0; size_ = 0; }

private:
  std::vector<float> buffer_;
  size_t head_{0};
  size_t size_{0};
};
