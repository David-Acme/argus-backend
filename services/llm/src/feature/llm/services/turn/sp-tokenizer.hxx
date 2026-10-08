#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace turn
{

enum class SpModel : unsigned char
{
  None,
  Bpe,
  Unigram
};

class SpTokenizer
{
public:
  [[nodiscard]] bool load(const std::filesystem::path& jsonPath);

  [[nodiscard]] bool loaded() const { return model_ != SpModel::None; }

  [[nodiscard]] SpModel model() const { return model_; }

  [[nodiscard]] std::vector<std::int32_t> encode(std::string_view text, bool addSpecialTokens = false) const;

  [[nodiscard]] std::int32_t clsId() const { return cls_; }
  [[nodiscard]] std::int32_t sepId() const { return sep_; }
  [[nodiscard]] std::int32_t maskId() const { return mask_; }
  [[nodiscard]] std::int32_t padId() const { return pad_; }
  [[nodiscard]] std::int32_t unkId() const { return unk_; }

  [[nodiscard]] const std::string& maskToken() const;

private:
  [[nodiscard]] std::string normalize(std::string_view text) const;
  [[nodiscard]] std::vector<std::string> preTokenize(const std::string& text) const;
  [[nodiscard]] std::vector<std::int32_t> bpe(const std::string& piece) const;
  [[nodiscard]] std::vector<std::int32_t> unigram(const std::string& piece) const;
  [[nodiscard]] std::vector<std::int32_t> bytesOf(const std::string& symbol) const;
  [[nodiscard]] std::vector<std::int32_t> fuse(const std::vector<std::int32_t>& ids) const;

  SpModel model_{SpModel::None};
  bool markerSpaces_{false};
  bool collapseWhitespace_{false};
  bool stripRight_{false};
  std::string marker_{"\xE2\x96\x81"};
  std::string maskToken_{"<mask>"};
  bool byteFallback_{false};
  bool ignoreMerges_{false};
  bool fuseUnk_{true};

  std::unordered_map<std::string, std::int32_t> vocab_;
  std::unordered_map<std::string, std::int32_t> merges_;
  std::unordered_map<std::string, float> scores_;
  std::int32_t unk_{0};
  std::int32_t cls_{0};
  std::int32_t sep_{0};
  std::int32_t mask_{0};
  std::int32_t pad_{0};
};

}
