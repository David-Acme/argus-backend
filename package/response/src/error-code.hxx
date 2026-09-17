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
  Cancelled,
  DeadlineExceeded
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
  case ErrorCode::Cancelled: return "CANCELLED";
  case ErrorCode::DeadlineExceeded: return "DEADLINE_EXCEEDED";
  }
  throw std::invalid_argument("Unknown response error code");
}
