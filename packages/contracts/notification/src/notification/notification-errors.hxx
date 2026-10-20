#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace NotificationErrors
{
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
inline constexpr ErrorDefinition ResponseNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Response not found"};
inline constexpr ErrorDefinition ResponseClosed{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "The response already has another verdict"};
inline constexpr ErrorDefinition NotificationNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Notification not found"};
inline constexpr ErrorDefinition ConversationNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Conversation not found"};
inline constexpr ErrorDefinition MessageInvalid{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "The message is not valid"};
inline constexpr ErrorDefinition MessageToSelf{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "A message to yourself is not a message"};
inline constexpr ErrorDefinition RecipientNotFound{
    .code = ErrorCode::UserNotFound,
    .status = 404,
    .message = "No active household member by that id"};
inline constexpr ErrorDefinition MessageRateLimited{
    .code = ErrorCode::TooManyRequests,
    .status = 429,
    .message = "Message limit reached for this hour (conversation.max_messages_per_hour)"};
inline constexpr ErrorDefinition MessengerUnavailable{
    .code = ErrorCode::ServiceUnavailable,
    .status = 503,
    .message = "The household directory cannot be read right now"};
}
