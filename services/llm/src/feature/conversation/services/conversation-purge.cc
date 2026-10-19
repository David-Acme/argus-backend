#include <feature/conversation/services/conversation-purge.hxx>

#include <drogon/drogon.h>

#include <thread>
#include <utility>

namespace
{
constexpr int64_t kSecondsPerDay = 86400;
}

double secondsUntilDaily(std::time_t now, DailyClock at)
{
  std::tm local{};
  localtime_r(&now, &local);
  const int64_t nowOfDay = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  const int64_t target = at.hour * 3600 + at.minute * 60;
  int64_t delta = target - nowOfDay;
  if (delta <= 0)
    delta += kSecondsPerDay;
  return static_cast<double>(delta);
}

ConversationPurgeTask::ConversationPurgeTask(std::shared_ptr<ConversationService> service,
                                             std::function<int64_t()> retentionDays, DailyClock at)
    : service_(std::move(service)), retentionDays_(std::move(retentionDays)), at_(at)
{
}

void ConversationPurgeTask::start()
{
  arm();
}

void ConversationPurgeTask::stop()
{
  if (timer_.has_value()) {
    drogon::app().getLoop()->invalidateTimer(*timer_);
    timer_.reset();
  }
}

void ConversationPurgeTask::arm()
{
  const double delay = secondsUntilDaily(std::time(nullptr), at_);
  timer_ = drogon::app().getLoop()->runAfter(delay, [this]() {
    std::thread([service = service_, days = retentionDays_()]() {
      const int removed = service->purgeExpired(std::time(nullptr), days);
      LOG_INFO << "Conversation purge: removed " << removed << " turns older than " << days << " days";
    }).detach();
    arm();
  });
}
