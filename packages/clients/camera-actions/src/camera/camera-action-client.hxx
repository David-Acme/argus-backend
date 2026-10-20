#pragma once

#include <argus/camera/v1/actions.grpc.pb.h>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using CameraCommandOutcome = argus::camera::v1::CommandOutcome;

struct CameraAnnounceInput
{
  int64_t cameraId{0};
  std::string text;
  std::string lang{"es"};
  std::string commandId;
  int64_t encounterId{0};
  int64_t expiresAt{0};
};

struct CameraAlarmInput
{
  int64_t cameraId{0};
  int seconds{0};
  std::string commandId;
  int64_t encounterId{0};
  int64_t expiresAt{0};
};

struct CameraSirenInput
{
  int64_t cameraId{0};
  bool enabled{false};
  std::string commandId;
  int64_t encounterId{0};
  int64_t expiresAt{0};
  int leaseSeconds{0};
};

struct CameraCrop
{
  std::string jpeg;
  int64_t capturedAt{0};
};

struct CameraPersonCropInput
{
  int64_t cameraId{0};
  int64_t trackId{0};
  int64_t firstSeenMs{0};
};

struct CameraListenInput
{
  int64_t cameraId{0};
  int seconds{0};
  std::string lang{"es"};
  std::string commandId;
  int64_t encounterId{0};
  int64_t expiresAt{0};
};

struct CameraLookInput
{
  int64_t cameraId{0};
  int64_t trackId{0};
  double boxX{0.0};
  double boxY{0.0};
  double boxW{0.0};
  double boxH{0.0};
  std::string reason;
  int64_t budgetMs{8000};
  bool follow{false};
};

struct CameraLookResult
{
  bool confirmed{false};
  std::string cls;
  float confidence{0.0F};
  std::vector<int64_t> cropIds;
  bool moved{false};
  std::string detail;
};

struct CameraCommandResult
{
  grpc::Status status;
  CameraCommandOutcome outcome{
      CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED};
  std::string detail;
  bool duplicate{false};
  bool captured{false};
  bool speechDetected{false};
  bool endpointed{false};
  std::string text;

  [[nodiscard]] bool transportOk() const { return status.ok(); }

  [[nodiscard]] bool succeeded() const
  {
    return transportOk() &&
           (outcome == CameraCommandOutcome::SUCCEEDED ||
            outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED);
  }

  [[nodiscard]] bool inFlight() const
  {
    return transportOk() && outcome == CameraCommandOutcome::IN_FLIGHT;
  }

  [[nodiscard]] bool indeterminate() const
  {
    return outcome == CameraCommandOutcome::INDETERMINATE;
  }

  [[nodiscard]] bool conflict() const { return outcome == CameraCommandOutcome::CONFLICT; }

  [[nodiscard]] bool rejected() const
  {
    return outcome == CameraCommandOutcome::REJECTED ||
           outcome == CameraCommandOutcome::CONFLICT;
  }

  [[nodiscard]] bool retryable() const
  {
    if (!transportOk())
      return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
             status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
    return outcome == CameraCommandOutcome::RETRYABLE_FAILED;
  }
};

struct CameraActionClientConfig
{
  std::string target;
  std::string credential;
};

class CameraActionClient
{
public:
  explicit CameraActionClient(CameraActionClientConfig config);

  CameraActionClient(const CameraActionClient&) = delete;
  CameraActionClient& operator=(const CameraActionClient&) = delete;
  virtual ~CameraActionClient() = default;

  virtual CameraCommandResult announce(const CameraAnnounceInput& input) const;

  virtual CameraCommandResult alarm(const CameraAlarmInput& input) const;

  virtual CameraCommandResult setSiren(const CameraSirenInput& input) const;

  virtual std::optional<CameraCrop>
  personCrop(const CameraPersonCropInput& input) const;

  virtual CameraCommandResult listen(const CameraListenInput& input) const;

  virtual bool relearn(int64_t cameraId) const;

  virtual std::optional<CameraLookResult> look(const CameraLookInput& input) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::camera::v1::CameraActionService::StubInterface> stub_;
  std::string credential_;
};
