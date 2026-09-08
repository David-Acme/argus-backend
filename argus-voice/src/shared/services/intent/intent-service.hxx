#pragma once

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

enum class ToolIntent
{
  Camera,
  MemorySave,
  None,
  Unknown,
};

std::string toolIntentToString(ToolIntent intent);
ToolIntent toolIntentFromString(const std::string& label);

struct IntentHit
{
  ToolIntent intent;
  float score;
};

namespace fasttext
{
class FastText;
}

class IntentService
{
public:
  IntentService();
  ~IntentService();

  IntentService(const IntentService&) = delete;
  IntentService& operator=(const IntentService&) = delete;

  void init();
  void shutdown();
  bool isLoaded() const;

  std::vector<IntentHit> match(const std::string& text) const;

  // Pure functions over a hit list; every caller uses the same ones.
  static std::string normalize(const std::string& text);
  static bool isMatchable(const std::string& text);
  static float score(const std::vector<IntentHit>& hits, ToolIntent intent);
  // Winning class over the runner-up by threshold + intent.margin.
  static bool fired(const std::vector<IntentHit>& hits, ToolIntent intent);
  static float margin(const std::vector<IntentHit>& hits);

private:
  mutable std::shared_mutex modelMutex_;
  std::unique_ptr<fasttext::FastText> model_;
  bool loaded_ = false;
};
