#pragma once

#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <vector>

struct MemorySaveInput
{
  MemoryScope scope;
  int64_t refId;
  MemoryType type;
  std::string content;
  int priority;
  MemorySource source;
  std::optional<int64_t> sourceTurnId;
  std::string lang;
};

struct MemoryEntry
{
  int64_t id;
  std::string scope;
  int64_t refId;
  MemoryType type;
  std::string content;
  int priority;
  std::string source;
  int64_t createdAt;
  int64_t updatedAt;
  int64_t hitCount;
};

class MemoryStore
{
public:
  MemoryStore() = delete;
  ~MemoryStore() = delete;

  static int64_t save(const MemorySaveInput& input);
  static int64_t saveDedup(const MemorySaveInput& input);
  static bool remove(int64_t id);
  static std::vector<MemoryEntry> list(const std::string& scope,
                                       int64_t refId, int limit);
  static size_t count();
};
