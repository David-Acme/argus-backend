#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ProcessOutput
{
  std::vector<std::vector<int64_t>> textIds;
  std::vector<std::vector<std::vector<float>>> textMask;
};

class UnicodeProcessor
{
public:
  explicit UnicodeProcessor(const std::string& unicodeIndexerJsonPath);
  ~UnicodeProcessor() = default;

  ProcessOutput process(const std::vector<std::string>& textList,
                        const std::vector<std::string>& langList) const;

private:
  std::string preprocessText(const std::string& text,
                             const std::string& lang) const;
  static std::vector<uint16_t> textToUnicodeValues(const std::string& text);
  std::vector<std::vector<std::vector<float>>>
  textMask(const std::vector<int64_t>& textIdsLengths) const;

  std::vector<int64_t> indexer_;
};
