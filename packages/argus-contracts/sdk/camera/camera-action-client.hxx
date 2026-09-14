#pragma once

#include <argus/camera/v1/actions.grpc.pb.h>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <optional>
#include <string>

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

// Exact observation crop request; trackId 0 keeps the legacy latest crop.
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

// One command result: the transport status is preserved separately from the
// command outcome, so a retryable transport failure is never mistaken for a
// rejected command or for a completed one.
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

  bool transportOk() const { return status.ok(); }

  bool succeeded() const
  {
    return transportOk() &&
           (outcome == CameraCommandOutcome::SUCCEEDED ||
            outcome == CameraCommandOutcome::DUPLICATE_SUCCEEDED);
  }

  bool inFlight() const
  {
    return transportOk() && outcome == CameraCommandOutcome::IN_FLIGHT;
  }

  bool indeterminate() const
  {
    return outcome == CameraCommandOutcome::INDETERMINATE;
  }

  bool conflict() const { return outcome == CameraCommandOutcome::CONFLICT; }

  bool rejected() const
  {
    return outcome == CameraCommandOutcome::REJECTED ||
           outcome == CameraCommandOutcome::CONFLICT;
  }

  // Only genuinely transient transport failures may be retried; an asserted
  // rejection, conflict or indeterminate outcome never is.
  bool retryable() const
  {
    if (!transportOk())
      return status.error_code() == grpc::StatusCode::UNAVAILABLE ||
             status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
    return outcome == CameraCommandOutcome::RETRYABLE_FAILED;
  }
};

// The caller-side capability credential for the guard -> camera edge.
struct CameraActionClientConfig
{
  std::string target;
  std::string credential;
};

// Thin SDK wrapper over argus.camera.v1.CameraActionService (rule 23).
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

  // Bound person crop for one observation; nullopt when unavailable or stale.
  virtual std::optional<CameraCrop>
  personCrop(const CameraPersonCropInput& input) const;

  // Bounded listen through the camera microphone, transcribed by argus-stt.
  virtual CameraCommandResult listen(const CameraListenInput& input) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::camera::v1::CameraActionService::StubInterface> stub_;
  std::string credential_;
};
