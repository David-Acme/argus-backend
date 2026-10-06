#pragma once

#include <argus/productivity/v1/reminder.grpc.pb.h>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class ReminderRpcOutcome : uint8_t
{
  Success,
  NotFound,
  Invalid,
  Refused,
  Unavailable
};

struct ReminderClientConfig
{
  std::string target;
  std::string credential;
};

struct ReminderCall
{
  argus::client::CallerIdentity identity;
};

struct ReminderCreateCall
{
  argus::client::CallerIdentity identity;
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
  std::optional<std::string> recurrenceRule;
  std::string idempotencyKey;
};

struct ReminderUpdateCall
{
  argus::client::CallerIdentity identity;
  int64_t id{0};
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<int64_t> scheduledAt;
  std::optional<bool> isCompleted;
};

struct ReminderRefCall
{
  argus::client::CallerIdentity identity;
  int64_t id{0};
};

struct ReminderListCall
{
  argus::client::CallerIdentity identity;
  bool includeCompleted{true};
  int32_t limit{0};
};

struct ReminderResult
{
  ReminderRpcOutcome outcome{ReminderRpcOutcome::Unavailable};
  std::string message;
  std::optional<argus::productivity::v1::ReminderRow> reminder;
};

struct ReminderListResult
{
  ReminderRpcOutcome outcome{ReminderRpcOutcome::Unavailable};
  std::string message;
  std::vector<argus::productivity::v1::ReminderRow> reminders;
};

class ProductivityReminderClient
{
public:
  explicit ProductivityReminderClient(ReminderClientConfig config);

  ProductivityReminderClient(const ProductivityReminderClient&) = delete;
  ProductivityReminderClient& operator=(const ProductivityReminderClient&) = delete;
  virtual ~ProductivityReminderClient() = default;

  [[nodiscard]] virtual ReminderResult create(const ReminderCreateCall& call) const;
  [[nodiscard]] virtual ReminderResult update(const ReminderUpdateCall& call) const;
  [[nodiscard]] virtual ReminderRpcOutcome remove(const ReminderRefCall& call) const;
  [[nodiscard]] virtual ReminderResult get(const ReminderRefCall& call) const;
  [[nodiscard]] virtual ReminderListResult list(const ReminderListCall& call) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::productivity::v1::ReminderService::StubInterface> stub_;
  std::string credential_;
};
