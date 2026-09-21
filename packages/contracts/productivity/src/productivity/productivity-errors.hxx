#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

// The productivity boundary's refusals: the projects, tasks, calendar events
// and shares the domain owns, and the two access checks that guard them. The
// service answers with these instead of a message string, so the code, the
// status and the text cannot drift apart.
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
// The user exists; the share is what is missing for them.
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
} // namespace ProductivityErrors
