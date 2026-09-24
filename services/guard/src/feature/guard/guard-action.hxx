#pragma once

#include <feature/guard/vocabulary/guard-action-kind.hxx>
#include <feature/guard/vocabulary/guard-danger.hxx>

#include <string>

struct GuardActionRequest
{
  GuardActionKind kind{GuardActionKind::Notify};
  GuardDanger danger{GuardDanger::None};
  bool greetingEnabled{false};
  bool replyRequested{false};
};

struct GuardActionDecision
{
  bool authorized{false};
  std::string reason;
};

class GuardActionAuthorizer
{
public:
  struct Config
  {
    int notifyLevel{2};
    int announceLevel{3};
    int alarmLevel{4};
    bool armSiren{false};
    bool dialogueEnabled{false};
  };

  explicit GuardActionAuthorizer(Config config);

  GuardActionDecision authorize(const GuardActionRequest& request) const;

  bool armSiren() const { return config_.armSiren; }

private:
  Config config_;
};
