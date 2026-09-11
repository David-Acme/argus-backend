#pragma once

#include <cstdint>
#include <string>

// FNV-1a content checksum shared by the database migration tools.
class Fnv1a
{
public:
  void add(uint8_t value);
  void add(const void* data, size_t size);
  uint64_t value() const { return hash_; }
  std::string hex() const;

private:
  static constexpr uint64_t kOffset = 14695981039346656037ULL;
  static constexpr uint64_t kPrime = 1099511628211ULL;

  uint64_t hash_{kOffset};
};

inline void Fnv1a::add(uint8_t value)
{
  hash_ ^= value;
  hash_ *= kPrime;
}

inline void Fnv1a::add(const void* data, size_t size)
{
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i)
    add(bytes[i]);
}

inline std::string Fnv1a::hex() const
{
  static const char* digits = "0123456789abcdef";
  std::string out(16, '0');
  uint64_t hash = hash_;
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[hash & 0xF];
    hash >>= 4;
  }
  return out;
}
