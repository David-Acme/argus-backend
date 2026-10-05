#pragma once

#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <string_view>
#include <sync/auth-change-sink.hxx>

class NatsBus;

class AuthActionSink : public AuthChangeSink
{
public:
  struct Config
  {
    int retryMs{outbox::kRetryMs};
    std::string actionSubject;
    std::string streamName;
    std::string sessionSubject;
    std::string sessionStreamName;
  };

  AuthActionSink(std::shared_ptr<NatsBus> bus, const Config& config);
  ~AuthActionSink() override = default;
  AuthActionSink(const AuthActionSink&) = delete;
  AuthActionSink& operator=(const AuthActionSink&) = delete;
  AuthActionSink(AuthActionSink&&) = delete;
  AuthActionSink& operator=(AuthActionSink&&) = delete;

  [[nodiscard]] drogon::Task<void>
  publishAction(const AuthActionPublishInput& input) const override;

  [[nodiscard]] drogon::Task<void>
  publishSessionChange(const AuthSessionChangeInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = outbox::kMaxPayloadBytes;
  static constexpr std::string_view kActionIdPrefix = "auth-action:";
  static constexpr std::string_view kSessionIdPrefix = "auth-session:";

  [[nodiscard]] static outbox::OutboxRepository repository();

private:
  const std::string actionSubject_;
  const std::string sessionSubject_;
  outbox::TransactionalOutbox outbox_;
};
