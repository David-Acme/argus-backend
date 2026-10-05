#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

template <typename Sample>
class BasicSampleRing
{
public:
  explicit BasicSampleRing(size_t capacity) : buffer_(capacity, Sample{}) {}

  void push(const Sample* data, size_t count)
  {
    const size_t capacity = buffer_.size();
    if (capacity == 0)
      return;
    if (count >= capacity) {
      std::copy(data + count - capacity, data + count, buffer_.begin());
      head_ = 0;
      size_ = capacity;
      return;
    }
    const size_t overflow = size_ + count > capacity ? size_ + count - capacity : 0;
    head_ = (head_ + overflow) % capacity;
    size_ -= overflow;
    const size_t tail = (head_ + size_) % capacity;
    const size_t first = std::min(count, capacity - tail);
    std::copy(data, data + first, buffer_.begin() + static_cast<std::ptrdiff_t>(tail));
    std::copy(data + first, data + count, buffer_.begin());
    size_ += count;
  }

  bool pop(Sample* dst, size_t count)
  {
    if (count > size_)
      return false;
    if (count == 0)
      return true;
    const size_t capacity = buffer_.size();
    const size_t first = std::min(count, capacity - head_);
    const auto from = buffer_.begin() + static_cast<std::ptrdiff_t>(head_);
    std::copy(from, from + static_cast<std::ptrdiff_t>(first), dst);
    std::copy(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count - first),
              dst + first);
    head_ = (head_ + count) % capacity;
    size_ -= count;
    return true;
  }

  void drainInto(std::vector<Sample>& out)
  {
    out.resize(size_);
    pop(out.data(), out.size());
  }

  [[nodiscard]] size_t size() const { return size_; }
  [[nodiscard]] size_t capacity() const { return buffer_.size(); }
  void clear() { head_ = 0; size_ = 0; }

private:
  std::vector<Sample> buffer_;
  size_t head_{0};
  size_t size_{0};
};

using SampleRing = BasicSampleRing<float>;
