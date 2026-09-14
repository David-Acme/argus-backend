#pragma once

#include <shared/enums.hxx>

#include <string>

// One autonomous effect the guard is considering, before any policy check.
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

// Single policy enforcement point: every autonomous effect is authorized here
// and nowhere else. Pure and deterministic; persistence stays in the service.
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
