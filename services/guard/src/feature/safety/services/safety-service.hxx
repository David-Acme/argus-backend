#pragma once

#include <feature/guard/services/disarm-gate.hxx>
#include <feature/safety/infra/safety-ports.hxx>
#include <feature/safety/repositories/safety-alert/safety-alert-repository.hxx>
#include <feature/safety/repositories/safety-setting/safety-setting-repository.hxx>
#include <feature/safety/repositories/user-pin/user-pin-repository.hxx>
#include <feature/safety/services/pin-attempts.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct SafetyStatus
{
  bool duressEnabled{false};
  bool hasPin{false};
};

struct PinSetInput
{
  int64_t userId{0};
  std::string userName;
  std::string disarmPin;
  std::string duressPin;
  std::optional<std::string> currentPin;
};

struct PinRemoveInput
{
  int64_t userId{0};
  std::string userName;
  std::optional<std::string> currentPin;
};

struct PanicInput
{
  int64_t userId{0};
  std::string userName;
  std::optional<int64_t> environmentId;
};

struct PanicResult
{
  int64_t alertId{0};
  bool sent{false};
  bool repeated{false};
};

struct SafetyToggleInput
{
  bool duressEnabled{false};
  int64_t actorUserId{0};
};

struct SafetyRuntime;

class SafetyService : public DisarmGate
{
public:
  using Clock = std::function<int64_t()>;

  struct Dependencies
  {
    const SafetyAlertSink* sink{nullptr};
    const SafetyActorNotifier* actor{nullptr};
    Clock clock;
  };

  struct Config
  {
    int pinIterations{600000};
    int64_t panicRepeatWindowS{60};
    int panicLimit{3};
    int64_t panicLimitWindowS{3600};
    int64_t retryBaseS{2};
    int64_t retryMaxS{60};
    int64_t resumeWindowS{86400};
    double sweepIntervalS{5.0};
    int64_t retentionS{30LL * 86400};
    PinAttempts::Limits attempts{};
  };

  SafetyService(Dependencies dependencies, Config config);
  ~SafetyService() override;

  SafetyService(const SafetyService&) = delete;
  SafetyService& operator=(const SafetyService&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  [[nodiscard]] drogon::Task<SafetyStatus> status(int64_t userId) const;
  [[nodiscard]] drogon::Task<SafetyStatus> setPin(const PinSetInput& input) const;
  [[nodiscard]] drogon::Task<SafetyStatus> removePin(const PinRemoveInput& input) const;
  [[nodiscard]] drogon::Task<SafetySetting> setting() const;
  [[nodiscard]] drogon::Task<SafetySetting> toggle(const SafetyToggleInput& input) const;
  [[nodiscard]] drogon::Task<PanicResult> panic(const PanicInput& input) const;

  [[nodiscard]] drogon::Task<DisarmVerdict>
  authorize(const DisarmRequest& request) const override;
  void duress(const DisarmRequest& request) const override;

  [[nodiscard]] drogon::Task<bool> deliver(const SafetyAlertNotice& notice) const;
  [[nodiscard]] drogon::Task<size_t> sweepPending() const;
  [[nodiscard]] drogon::Task<int64_t> purgeExpired() const;
  [[nodiscard]] drogon::Task<void> forgetUser(int64_t userId) const;

private:
  struct PinCheck
  {
    const DisarmRequest& request;
    const UserPinRow& pin;
  };

  [[nodiscard]] drogon::Task<DisarmVerdict> verifyPin(const PinCheck& check) const;
  [[nodiscard]] drogon::Task<void> requireCurrentPin(const DisarmRequest& request) const;
  [[nodiscard]] int64_t now() const;
  [[nodiscard]] int64_t retryDelay(int failures) const;
  void recordOutcome(int64_t alertId, bool sent) const;

  Dependencies dependencies_;
  Config config_;
  UserPinRepository pinRepository_;
  SafetySettingRepository settingRepository_;
  SafetyAlertRepository alertRepository_;
  std::shared_ptr<PinAttempts> attempts_;
  std::shared_ptr<SafetyRuntime> runtime_;
};
