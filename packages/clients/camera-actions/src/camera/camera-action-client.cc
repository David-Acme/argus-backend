#include "camera-action-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <algorithm>
#include <utility>

namespace
{
constexpr int kCallTimeoutMs = 60000;

using CameraCommandOutcome = argus::camera::v1::CommandOutcome;

CameraCommandOutcome outcomeForStatus(grpc::StatusCode code)
{
  switch (code) {
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return CameraCommandOutcome::RETRYABLE_FAILED;
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::FAILED_PRECONDITION:
    case grpc::StatusCode::NOT_FOUND:
    case grpc::StatusCode::PERMISSION_DENIED:
    case grpc::StatusCode::UNAUTHENTICATED:
    case grpc::StatusCode::ALREADY_EXISTS:
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return CameraCommandOutcome::REJECTED;
    default:
      return CameraCommandOutcome::INDETERMINATE;
  }
}

CameraCommandOutcome ackOutcome(const argus::camera::v1::ActionAck& ack)
{
  if (ack.outcome() != CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED)
    return ack.outcome();
  if (ack.duplicate())
    return CameraCommandOutcome::DUPLICATE_SUCCEEDED;
  if (ack.accepted())
    return CameraCommandOutcome::SUCCEEDED;
  if (ack.detail() == "in_flight")
    return CameraCommandOutcome::IN_FLIGHT;
  if (ack.detail() == "indeterminate")
    return CameraCommandOutcome::INDETERMINATE;
  if (ack.detail() == "command_id_conflict")
    return CameraCommandOutcome::CONFLICT;
  return CameraCommandOutcome::REJECTED;
}

CameraCommandResult fromAck(const grpc::Status& status,
                            const argus::camera::v1::ActionAck& ack)
{
  CameraCommandResult result;
  result.status = status;
  result.detail = ack.detail();
  result.duplicate = ack.duplicate();
  result.outcome =
      status.ok() ? ackOutcome(ack) : outcomeForStatus(status.error_code());
  return result;
}

CameraCommandResult
fromListen(const grpc::Status& status,
           const argus::camera::v1::ListenResponse& response)
{
  CameraCommandResult result;
  result.status = status;
  result.captured = response.captured();
  result.speechDetected = response.speech_detected();
  result.endpointed = response.endpointed();
  result.duplicate = response.duplicate();
  result.text = response.text();
  if (status.ok()) {
    result.outcome =
        response.outcome() != CameraCommandOutcome::COMMAND_OUTCOME_UNSPECIFIED
            ? response.outcome()
            : (response.duplicate()
                   ? CameraCommandOutcome::DUPLICATE_SUCCEEDED
                   : (response.captured() ? CameraCommandOutcome::SUCCEEDED
                                          : CameraCommandOutcome::REJECTED));
  }
  else {
    result.outcome = outcomeForStatus(status.error_code());
  }
  return result;
}
}

CameraActionClient::CameraActionClient(CameraActionClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::camera::v1::CameraActionService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

CameraCommandResult
CameraActionClient::announce(const CameraAnnounceInput& input) const
{
  if (input.cameraId <= 0)
    return {.status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                   "camera id is required")};

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::camera::v1::AnnounceRequest request;
  request.set_camera_id(input.cameraId);
  request.set_text(input.text);
  request.set_lang(input.lang);
  request.set_command_id(input.commandId);
  request.set_encounter_id(input.encounterId);
  request.set_expires_at(input.expiresAt);

  argus::camera::v1::ActionAck response;
  const grpc::Status status = stub_->Announce(&context, request, &response);
  return fromAck(status, response);
}

CameraCommandResult
CameraActionClient::alarm(const CameraAlarmInput& input) const
{
  if (input.cameraId <= 0)
    return {.status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                   "camera id is required")};

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::camera::v1::AlarmRequest request;
  request.set_camera_id(input.cameraId);
  request.set_seconds(input.seconds);
  request.set_command_id(input.commandId);
  request.set_encounter_id(input.encounterId);
  request.set_expires_at(input.expiresAt);

  argus::camera::v1::ActionAck response;
  const grpc::Status status = stub_->Alarm(&context, request, &response);
  return fromAck(status, response);
}

CameraCommandResult
CameraActionClient::setSiren(const CameraSirenInput& input) const
{
  if (input.cameraId <= 0)
    return {.status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                   "camera id is required")};

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::camera::v1::SirenRequest request;
  request.set_camera_id(input.cameraId);
  request.set_enabled(input.enabled);
  request.set_command_id(input.commandId);
  request.set_encounter_id(input.encounterId);
  request.set_expires_at(input.expiresAt);
  request.set_lease_seconds(input.leaseSeconds);

  argus::camera::v1::ActionAck response;
  const grpc::Status status = stub_->SetSiren(&context, request, &response);
  return fromAck(status, response);
}

std::optional<CameraCrop>
CameraActionClient::personCrop(const CameraPersonCropInput& input) const
{
  if (input.cameraId <= 0)
    return std::nullopt;

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::camera::v1::PersonCropRequest request;
  request.set_camera_id(input.cameraId);
  request.set_track_id(input.trackId);
  request.set_first_seen_ms(input.firstSeenMs);

  argus::camera::v1::PersonCropResponse response;
  if (const grpc::Status status =
          stub_->GetPersonCrop(&context, request, &response);
      !status.ok())
    return std::nullopt;
  if (response.jpeg().empty())
    return std::nullopt;
  return CameraCrop{.jpeg = response.jpeg(),
                    .capturedAt = response.captured_at()};
}

CameraCommandResult
CameraActionClient::listen(const CameraListenInput& input) const
{
  if (input.cameraId <= 0 || input.commandId.empty())
    return {.status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                   "camera id and command id are required")};

  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);

  argus::camera::v1::ListenRequest request;
  request.set_camera_id(input.cameraId);
  request.set_seconds(input.seconds);
  request.set_lang(input.lang);
  request.set_command_id(input.commandId);
  request.set_encounter_id(input.encounterId);
  request.set_expires_at(input.expiresAt);

  argus::camera::v1::ListenResponse response;
  const grpc::Status status = stub_->Listen(&context, request, &response);
  return fromListen(status, response);
}

bool CameraActionClient::relearn(int64_t cameraId) const
{
  if (cameraId <= 0)
    return false;
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);
  argus::camera::v1::RelearnRequest request;
  request.set_camera_id(cameraId);
  argus::camera::v1::RelearnResponse response;
  const grpc::Status status = stub_->Relearn(&context, request, &response);
  return status.ok() && response.accepted();
}

std::optional<CameraLookResult> CameraActionClient::look(const CameraLookInput& input) const
{
  constexpr int64_t kLookMarginMs = 5000;
  if (input.cameraId <= 0)
    return std::nullopt;
  grpc::ClientContext context;
  argus::client::setDeadline(context, static_cast<int>(std::max<int64_t>(input.budgetMs, 1000) + kLookMarginMs));
  argus::client::addCallerCredential(context, credential_);
  argus::camera::v1::LookRequest request;
  request.set_camera_id(input.cameraId);
  request.set_track_id(input.trackId);
  request.set_box_x(input.boxX);
  request.set_box_y(input.boxY);
  request.set_box_w(input.boxW);
  request.set_box_h(input.boxH);
  request.set_reason(input.reason);
  request.set_budget_ms(input.budgetMs);
  request.set_follow(input.follow);
  argus::camera::v1::LookResponse response;
  if (const grpc::Status status = stub_->Look(&context, request, &response); !status.ok())
    return std::nullopt;
  CameraLookResult result{.confirmed = response.confirmed(),
                          .cls = response.class_name(),
                          .confidence = response.confidence(),
                          .cropIds = {},
                          .moved = response.moved(),
                          .detail = response.detail()};
  result.cropIds.assign(response.crop_ids().begin(), response.crop_ids().end());
  return result;
}
