#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace ProximityErrors
{
inline constexpr ErrorDefinition LocationOff{.code = ErrorCode::Conflict,
                                             .status = 409,
                                             .message = "Locating you at home is off in your privacy choices"};
inline constexpr ErrorDefinition ReceiverNotFound{.code = ErrorCode::NotFound,
                                                  .status = 404,
                                                  .message = "Receiver not found"};
inline constexpr ErrorDefinition SensorNotFound{.code = ErrorCode::NotFound,
                                                .status = 404,
                                                .message = "Sensor not found"};
inline constexpr ErrorDefinition LocationUnknown{.code = ErrorCode::ServiceUnavailable,
                                                 .status = 503,
                                                 .message = "Your privacy choices could not be read; try again"};
}
