#pragma once

#include <cstdint>
#include <shared/services/memory/memory-recall.hxx>
#include <shared/services/memory/tool-parser.hxx>
#include <string>

struct CaptureInput
{
  int64_t userId;
  std::string lang;
  std::string text;
};

class MemoryService
{
public:
  MemoryService() = delete;
  ~MemoryService() = delete;

  static void init();
  static void shutdown();
  static bool isLoaded();

  static int64_t captureExplicit(const CaptureInput& input);
  static int64_t captureToolCall(int64_t userId, const std::string& lang,
                                 const ToolCall& call);
  static RecallContext recall(const RecallInput& input);
  static void bumpHitCount(const std::vector<int64_t>& ids);
};
