#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace ProductivityErrors
{
inline constexpr ErrorDefinition ProjectNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Project not found"};
inline constexpr ErrorDefinition TaskNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Task not found"};
inline constexpr ErrorDefinition CalendarEventNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Calendar event not found"};
inline constexpr ErrorDefinition ShareNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Share not found"};
inline constexpr ErrorDefinition UserNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "User not found"};
inline constexpr ErrorDefinition CannotSeeProjects{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "That user cannot see projects"};
inline constexpr ErrorDefinition CannotSeeCalendarEvents{
    .code = ErrorCode::Forbidden,
    .status = 403,
    .message = "That user cannot see calendar events"};
inline constexpr ErrorDefinition OwnerAlreadyHasAccess{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "The owner already has access"};
inline constexpr ErrorDefinition IdempotencyKeyReused{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "That Idempotency-Key already created something else"};
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
}
