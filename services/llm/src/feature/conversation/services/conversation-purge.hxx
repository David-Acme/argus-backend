#pragma once

#include <feature/conversation/services/conversation-service.hxx>

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>

struct DailyClock
{
  int hour{4};
  int minute{30};
};

[[nodiscard]] double secondsUntilDaily(std::time_t now, DailyClock at);

class ConversationPurgeTask
{
public:
  ConversationPurgeTask(std::shared_ptr<ConversationService> service, std::function<int64_t()> retentionDays,
                        DailyClock at);

  void start();

  void stop();

private:
  void arm();

  std::shared_ptr<ConversationService> service_;
  std::function<int64_t()> retentionDays_;
  DailyClock at_;
  std::optional<uint64_t> timer_;
};
