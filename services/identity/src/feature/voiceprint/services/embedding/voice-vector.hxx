#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace voice_vector
{

[[nodiscard]] std::vector<float> normalized(std::span<const float> values);

[[nodiscard]] float cosine(std::span<const float> left,
                           std::span<const float> right);

[[nodiscard]] std::vector<float>
centroid(std::span<const std::vector<float>> embeddings);

[[nodiscard]] std::vector<char> toBlob(std::span<const float> values);

[[nodiscard]] std::vector<float> fromBlob(std::string_view blob);

}
