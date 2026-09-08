#pragma once

#include <cstdint>
#include <string>
#include <vector>

class UnicodeProcessor
{
public:
  explicit UnicodeProcessor(const std::string& unicodeIndexerJsonPath);
  ~UnicodeProcessor() = default;

  void process(const std::vector<std::string>& textList,
               const std::vector<std::string>& langList,
               std::vector<std::vector<int64_t>>& textIds,
               std::vector<std::vector<std::vector<float>>>& textMask) const;

private:
  std::string preprocessText(const std::string& text,
                             const std::string& lang) const;
  static std::vector<uint16_t> textToUnicodeValues(const std::string& text);
  std::vector<std::vector<std::vector<float>>>
  textMask(const std::vector<int64_t>& textIdsLengths) const;

  std::vector<int64_t> indexer_;
};
