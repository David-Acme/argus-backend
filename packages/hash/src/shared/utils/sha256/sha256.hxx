#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace argus::hash
{

// Self-contained SHA-256 so no service needs a crypto dependency for hashing.
class Sha256
{
public:
  Sha256() { reset(); }

  void reset()
  {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    total_ = 0;
    buffered_ = 0;
  }

  void update(const void* data, size_t size)
  {
    const auto* bytes = static_cast<const uint8_t*>(data);
    total_ += size;
    while (size > 0) {
      const size_t take = std::min(size, kBlockSize - buffered_);
      std::memcpy(buffer_.data() + buffered_, bytes, take);
      buffered_ += take;
      bytes += take;
      size -= take;
      if (buffered_ == kBlockSize) {
        transform(buffer_.data());
        buffered_ = 0;
      }
    }
  }

  void update(std::string_view data) { update(data.data(), data.size()); }

  std::array<uint8_t, 32> digest()
  {
    const uint64_t bitLength = total_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0x00;
    while (buffered_ != kBlockSize - 8)
      update(&zero, 1);
    std::array<uint8_t, 8> length{};
    for (int index = 0; index < 8; ++index)
      length[static_cast<size_t>(index)] =
          static_cast<uint8_t>(bitLength >> (56 - 8 * index));
    update(length.data(), length.size());

    std::array<uint8_t, 32> out{};
    for (size_t index = 0; index < state_.size(); ++index) {
      out[index * 4] = static_cast<uint8_t>(state_[index] >> 24);
      out[index * 4 + 1] = static_cast<uint8_t>(state_[index] >> 16);
      out[index * 4 + 2] = static_cast<uint8_t>(state_[index] >> 8);
      out[index * 4 + 3] = static_cast<uint8_t>(state_[index]);
    }
    return out;
  }

private:
  static constexpr size_t kBlockSize = 64;

  static uint32_t rotr(uint32_t value, uint32_t bits)
  {
    return (value >> bits) | (value << (32 - bits));
  }

  void transform(const uint8_t* block)
  {
    static constexpr std::array<uint32_t, 64> k =
        {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
         0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
         0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
         0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
         0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
         0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
         0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
         0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
         0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
         0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
         0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
         0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
         0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t w[64];
    for (int index = 0; index < 16; ++index)
      w[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                 (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                 (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                 static_cast<uint32_t>(block[index * 4 + 3]);
    for (int index = 16; index < 64; ++index) {
      const uint32_t s0 = rotr(w[index - 15], 7) ^ rotr(w[index - 15], 18) ^
                          (w[index - 15] >> 3);
      const uint32_t s1 = rotr(w[index - 2], 17) ^ rotr(w[index - 2], 19) ^
                          (w[index - 2] >> 10);
      w[index] = w[index - 16] + s0 + w[index - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (int index = 0; index < 64; ++index) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t temp1 = h + s1 + ch + k[index] + w[index];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<uint32_t, 8> state_{};
  std::array<uint8_t, kBlockSize> buffer_{};
  uint64_t total_{0};
  size_t buffered_{0};
};

inline std::string hex(const std::array<uint8_t, 32>& digest)
{
  static const char* digits = "0123456789abcdef";
  std::string out(64, '0');
  for (size_t index = 0; index < digest.size(); ++index) {
    out[index * 2] = digits[digest[index] >> 4];
    out[index * 2 + 1] = digits[digest[index] & 0x0f];
  }
  return out;
}

inline std::string sha256Hex(std::string_view data)
{
  Sha256 hasher;
  hasher.update(data);
  return hex(hasher.digest());
}

} // namespace argus::hash
