#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RecallInput
{
  int64_t userId;
  std::string text;
  std::string lang;
  std::vector<int64_t> personIds;
};

struct RecalledMemory
{
  int64_t id;
  std::string content;
  float score;
};

struct RecallContext
{
  std::string prependText;
  std::string profileText;
  std::vector<int64_t> usedIds;
};

class MemoryRecall
{
public:
  MemoryRecall() = delete;
  ~MemoryRecall() = delete;

  static RecallContext recall(const RecallInput& input);
  static std::string profileText(int64_t userId,
                                 const std::vector<int64_t>& personIds);
};
