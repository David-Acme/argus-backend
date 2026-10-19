#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace MessengerErrors
{
inline constexpr ErrorDefinition RecipientNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "No household member matches that name"};
inline constexpr ErrorDefinition MessageTooLong{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "The message is longer than messenger.max_message_chars"};
inline constexpr ErrorDefinition SelfRecipient{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "A message to yourself is not a relay"};
inline constexpr ErrorDefinition MessageRequired{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "The message is empty"};
inline constexpr ErrorDefinition RateLimited{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Relay limit reached for this hour (messenger.max_relays_per_hour)"};
inline constexpr ErrorDefinition VisitorLimited{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Visitor notices limit reached for this camera and member (messenger.max_visitor_notices_per_hour_per_camera)"};
inline constexpr ErrorDefinition ChannelUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "No channel can carry the message right now"};
}
