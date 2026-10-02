#pragma once

#include "guard-policy.hxx"

#include <config/guard-config.hxx>
#include <cstdint>
#include <string>
#include <vector>

#include <drogon/utils/coroutine.h>

class CameraActionClient;
class LlmClient;
class VlmClient;

struct GuardAssessmentInput
{
  int64_t cameraId{0};
  int64_t trackId{0};
  int64_t firstSeenMs{0};
  int64_t publishedAtMs{0};
  std::string rule;
  std::string profile;
  int visitCount{0};
  int checks{0};
  bool expectedGuest{false};
  GuardDanger danger{GuardDanger::None};
  bool personHasUser{false};
  std::string personName;
  std::string personRole;
  std::string personObservation;
  std::vector<std::string> knownTags;
  bool greeted{false};
  std::string greetingText;
  std::string personReply;
  bool replied{false};
  std::string replyText;
  int dialogueTurns{0};
};

enum class GuardDecisionKind
{
  Invalid,
  ToolCall,
  Final,
};

struct GuardToolCall
{
  std::string tool;
  std::string prompt;
  int seconds{0};
};

struct GuardFinalDecision
{
  std::string threat;
  bool veto{false};
  std::vector<std::string> tags;
  std::string summary;
  std::string announceText;
  std::string reason;
};

struct GuardDecision
{
  GuardDecisionKind kind{GuardDecisionKind::Invalid};
  GuardToolCall tool;
  GuardFinalDecision final;
};

struct GuardToolLog
{
  std::string kind;
  std::string status;
};

namespace guard_assessment
{

GuardDecision parseDecision(const std::string& text);

}

struct GuardAssessmentResult
{
  bool performed{false};
  bool valid{false};
  bool veto{false};
  std::string mode;
  std::string threat;
  std::string caption;
  std::string summary;
  std::vector<std::string> tags;
  std::string announceText;
  int toolRounds{0};
  std::vector<GuardToolLog> toolLogs;
};

class GuardAssessment
{
public:
  struct Dependencies
  {
    CameraActionClient* camera{nullptr};
    VlmClient* vlm{nullptr};
    LlmClient* llm{nullptr};
  };

  using Config = GuardAssessmentConfig;

  GuardAssessment(Dependencies dependencies, Config config);
  GuardAssessment(const GuardAssessment&) = delete;
  GuardAssessment& operator=(const GuardAssessment&) = delete;
  virtual ~GuardAssessment() = default;

  virtual drogon::Task<GuardAssessmentResult> assess(
      const GuardAssessmentInput& input) const;

private:
  Dependencies dependencies_;
  Config config_;
};
