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
inline constexpr ErrorDefinition ReminderNotFound{
    .code = ErrorCode::NotFound,
    .status = 404,
    .message = "Reminder not found"};
inline constexpr ErrorDefinition ReminderAlreadyDone{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "The reminder is already done; it cannot be snoozed"};
inline constexpr ErrorDefinition NotASeries{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "It does not repeat; edit it instead of one of its occurrences"};
inline constexpr ErrorDefinition NotAnOccurrence{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "occurrence is not an occurrence of this series"};
inline constexpr ErrorDefinition OccurrenceAlreadyPassed{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "That occurrence has already been delivered"};
inline constexpr ErrorDefinition OccurrenceOutsideItsSlot{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "A reminder's occurrence moves only between the series' previous and next occurrences"};
inline constexpr ErrorDefinition ReminderOccurrenceOnlyMoves{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "A reminder's occurrence can only be skipped or moved"};
inline constexpr ErrorDefinition TooManyOccurrenceExceptions{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "The series already has too many changed occurrences"};
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
inline constexpr ErrorDefinition AssigneeNotInProject{
    .code = ErrorCode::ValidationError,
    .status = 422,
    .message = "The assignee is neither the project's owner nor one of its members"};
inline constexpr ErrorDefinition IdempotencyKeyReused{
    .code = ErrorCode::Conflict,
    .status = 409,
    .message = "That Idempotency-Key already created something else"};
inline constexpr ErrorDefinition ChangeNotRecorded{
    .code = ErrorCode::InternalError,
    .status = 500,
    .message = "The change could not be recorded"};
}
