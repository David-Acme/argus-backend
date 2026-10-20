#pragma once

#include <voice/voice-lang.hxx>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct VisitorRepliesConfig
{
  std::chrono::milliseconds accept{1800000};
  std::chrono::milliseconds hold{1800000};
  int maxChars{300};
};

struct VisitorThreadInput
{
  std::string conversationId;
  int64_t cameraId{0};
  int64_t personId{0};
  std::vector<int64_t> recipients;
  int64_t at{0};
};

struct VisitorReplyInput
{
  int64_t userId{0};
  std::string conversationId;
  std::string text;
  int64_t now{0};
};

enum class VisitorReplyOutcome : uint8_t
{
  Queued,
  Said,
  NotFound,
  Expired,
  TooLong
};

struct VisitorReplyResult
{
  VisitorReplyOutcome outcome{VisitorReplyOutcome::NotFound};
  int64_t replyUntil{0};
  int64_t cameraId{0};
};

struct PendingVisitorReply
{
  std::string text;
  int64_t personId{0};
  int64_t until{0};
  std::string conversationId;
};

struct VisitorReplyTake
{
  int64_t cameraId{0};
  int64_t personId{0};
  int64_t now{0};
};

[[nodiscard]] std::string visitorReplyLine(VoiceLang lang, std::string_view text);

[[nodiscard]] int64_t utf8Characters(std::string_view text);

class VisitorReplies
{
public:
  explicit VisitorReplies(VisitorRepliesConfig config) : config_(config) {}

  [[nodiscard]] int64_t open(const VisitorThreadInput& input);
  [[nodiscard]] VisitorReplyResult reply(const VisitorReplyInput& input);
  [[nodiscard]] std::optional<PendingVisitorReply> take(const VisitorReplyTake& input);
  [[nodiscard]] size_t pendingCount() const;

private:
  struct Thread
  {
    int64_t cameraId{0};
    int64_t personId{0};
    std::vector<int64_t> recipients;
    int64_t acceptUntil{0};
  };

  void purge(int64_t now);

  VisitorRepliesConfig config_;
  mutable std::mutex mutex_;
  std::map<std::string, Thread> threads_;
  std::map<int64_t, PendingVisitorReply> pending_;
};
