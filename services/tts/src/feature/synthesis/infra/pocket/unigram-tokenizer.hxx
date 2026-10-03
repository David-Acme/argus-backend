#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class UnigramTokenizer
{
public:
  [[nodiscard]] static UnigramTokenizer load(const std::filesystem::path& path);

  [[nodiscard]] std::vector<std::int64_t> encode(std::string_view text) const;
  [[nodiscard]] std::size_t vocabularySize() const { return vocabularySize_; }

private:
  struct Piece
  {
    std::int64_t id{0};
    double score{0};
  };

  struct PieceHash
  {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept
    {
      return std::hash<std::string_view>{}(text);
    }
  };

  void encodeWord(std::string_view word, std::vector<std::int64_t>& ids) const;

  std::unordered_map<std::string, Piece, PieceHash, std::equal_to<>> pieces_;
  std::array<std::int64_t, 256> byteIds_{};
  std::size_t maxPieceBytes_{0};
  std::size_t vocabularySize_{0};
  std::int64_t unknownId_{0};
  double unknownScore_{0};
};
