#include "voice-vector.hxx"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numeric>

namespace
{
constexpr float kNormFloor = 1e-6F;
constexpr size_t kFloatBytes = sizeof(float);
}

namespace voice_vector
{

std::vector<float> normalized(std::span<const float> values)
{
  std::vector<float> out(values.begin(), values.end());
  const float norm =
      std::sqrt(std::inner_product(out.begin(), out.end(), out.begin(), 0.0F));
  if (norm > kNormFloor)
    for (float& value : out)
      value /= norm;
  return out;
}

float cosine(std::span<const float> left, std::span<const float> right)
{
  if (left.empty() || left.size() != right.size())
    return 0.0F;
  const float dot =
      std::inner_product(left.begin(), left.end(), right.begin(), 0.0F);
  const float leftNorm = std::sqrt(
      std::inner_product(left.begin(), left.end(), left.begin(), 0.0F));
  const float rightNorm = std::sqrt(
      std::inner_product(right.begin(), right.end(), right.begin(), 0.0F));
  if (leftNorm <= kNormFloor || rightNorm <= kNormFloor)
    return 0.0F;
  return dot / (leftNorm * rightNorm);
}

std::vector<float> centroid(std::span<const std::vector<float>> embeddings)
{
  if (embeddings.empty())
    return {};
  std::vector<float> sum(embeddings.front().size(), 0.0F);
  for (const auto& embedding : embeddings) {
    const std::vector<float> unit = normalized(embedding);
    if (unit.size() != sum.size())
      return {};
    std::ranges::transform(sum, unit, sum.begin(), std::plus<>());
  }
  return normalized(sum);
}

std::vector<char> toBlob(std::span<const float> values)
{
  std::vector<char> blob;
  blob.reserve(values.size() * kFloatBytes);
  for (const float value : values) {
    const auto word = std::bit_cast<uint32_t>(value);
    for (unsigned shift = 0; shift < 32U; shift += 8U)
      blob.push_back(static_cast<char>((word >> shift) & 0xFFU));
  }
  return blob;
}

std::vector<float> fromBlob(std::string_view blob)
{
  std::vector<float> values;
  values.reserve(blob.size() / kFloatBytes);
  for (size_t offset = 0; offset + kFloatBytes <= blob.size();
       offset += kFloatBytes) {
    uint32_t word = 0;
    for (unsigned index = 0; index < kFloatBytes; ++index)
      word |= static_cast<uint32_t>(static_cast<uint8_t>(blob[offset + index]))
              << (8U * index);
    values.push_back(std::bit_cast<float>(word));
  }
  return values;
}

}
