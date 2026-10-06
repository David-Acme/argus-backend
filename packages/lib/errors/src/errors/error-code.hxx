#pragma once

#include <stdexcept>
#include <string_view>

enum class ErrorCode
{
  Error,
  BadRequest,
  Unauthorized,
  Forbidden,
  NotFound,
  MethodNotAllowed,
  Conflict,
  RemoteNotAllowed,
  ServiceUnavailable,
  TooManyRequests,
  InternalError,
  BadGateway,
  ValidationError,
  UserNotFound,
  TtsNotLoaded,
  SttNotLoaded,
  LlmNotLoaded,
  VlmNotLoaded,
  CameraUnreachable,
  Cancelled,
  DeadlineExceeded,
  SessionNotFound,
  AccountDisabled,
  RtcUnavailable,
  CallNotFound,
  CallTaken,
  CallExpired,
  PinRequired,
  PinInvalid,
  PinLocked,
  LivenessCheckFailed,
  LivenessUnavailable,
  FaceQualityInsufficient,
  ModuleDisabled,
  ModuleHardwareInsufficient,
  ModuleComingSoon,
  ModuleJobRunning,
  ModuleRequiredBy,
  ModuleCore,
  RoleInactive
};

constexpr std::string_view toString(ErrorCode code)
{
  switch (code) {
  case ErrorCode::Error: return "ERROR";
  case ErrorCode::BadRequest: return "BAD_REQUEST";
  case ErrorCode::Unauthorized: return "UNAUTHORIZED";
  case ErrorCode::Forbidden: return "FORBIDDEN";
  case ErrorCode::NotFound: return "NOT_FOUND";
  case ErrorCode::MethodNotAllowed: return "METHOD_NOT_ALLOWED";
  case ErrorCode::Conflict: return "CONFLICT";
  case ErrorCode::RemoteNotAllowed: return "REMOTE_NOT_ALLOWED";
  case ErrorCode::ServiceUnavailable: return "SERVICE_UNAVAILABLE";
  case ErrorCode::TooManyRequests: return "TOO_MANY_REQUESTS";
  case ErrorCode::InternalError: return "INTERNAL_ERROR";
  case ErrorCode::BadGateway: return "BAD_GATEWAY";
  case ErrorCode::ValidationError: return "VALIDATION_ERROR";
  case ErrorCode::UserNotFound: return "USER_NOT_FOUND";
  case ErrorCode::TtsNotLoaded: return "TTS_NOT_LOADED";
  case ErrorCode::SttNotLoaded: return "STT_NOT_LOADED";
  case ErrorCode::LlmNotLoaded: return "LLM_NOT_LOADED";
  case ErrorCode::VlmNotLoaded: return "VLM_NOT_LOADED";
  case ErrorCode::CameraUnreachable: return "CAMERA_UNREACHABLE";
  case ErrorCode::Cancelled: return "CANCELLED";
  case ErrorCode::DeadlineExceeded: return "DEADLINE_EXCEEDED";
  case ErrorCode::SessionNotFound: return "SESSION_NOT_FOUND";
  case ErrorCode::AccountDisabled: return "ACCOUNT_DISABLED";
  case ErrorCode::RtcUnavailable: return "RTC_UNAVAILABLE";
  case ErrorCode::CallNotFound: return "CALL_NOT_FOUND";
  case ErrorCode::CallTaken: return "CALL_TAKEN";
  case ErrorCode::CallExpired: return "CALL_EXPIRED";
  case ErrorCode::PinRequired: return "PIN_REQUIRED";
  case ErrorCode::PinInvalid: return "PIN_INVALID";
  case ErrorCode::PinLocked: return "PIN_LOCKED";
  case ErrorCode::LivenessCheckFailed: return "LIVENESS_CHECK_FAILED";
  case ErrorCode::LivenessUnavailable: return "LIVENESS_UNAVAILABLE";
  case ErrorCode::FaceQualityInsufficient: return "FACE_QUALITY_INSUFFICIENT";
  case ErrorCode::ModuleDisabled: return "MODULE_DISABLED";
  case ErrorCode::ModuleHardwareInsufficient: return "MODULE_HARDWARE_INSUFFICIENT";
  case ErrorCode::ModuleComingSoon: return "MODULE_COMING_SOON";
  case ErrorCode::ModuleJobRunning: return "MODULE_JOB_RUNNING";
  case ErrorCode::ModuleRequiredBy: return "MODULE_REQUIRED_BY";
  case ErrorCode::ModuleCore: return "MODULE_CORE";
  case ErrorCode::RoleInactive: return "ROLE_INACTIVE";
  }
  throw std::invalid_argument("Unknown response error code");
}
