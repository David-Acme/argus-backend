#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

namespace ModuleErrors
{
inline constexpr ErrorDefinition UnknownModule{.code = ErrorCode::NotFound, .status = 404, .message = "Unknown module"};
inline constexpr ErrorDefinition ComingSoon{
    .code = ErrorCode::ModuleComingSoon, .status = 409, .message = "This module is not available yet"};
inline constexpr ErrorDefinition HardwareInsufficient{.code = ErrorCode::ModuleHardwareInsufficient,
                                                      .status = 409,
                                                      .message = "This server cannot run the module"};
inline constexpr ErrorDefinition JobRunning{
    .code = ErrorCode::ModuleJobRunning, .status = 409, .message = "The module already has a job in progress"};
inline constexpr ErrorDefinition RequiredBy{
    .code = ErrorCode::ModuleRequiredBy, .status = 409, .message = "Another enabled module requires this one"};
inline constexpr ErrorDefinition CoreModule{
    .code = ErrorCode::ModuleCore, .status = 409, .message = "The core module cannot be disabled"};
inline constexpr ErrorDefinition AlreadyEnabled{
    .code = ErrorCode::Conflict, .status = 409, .message = "The module is already enabled"};
inline constexpr ErrorDefinition NoJob{.code = ErrorCode::NotFound, .status = 404, .message = "The module has no job"};
inline constexpr ErrorDefinition JobBusy{
    .code = ErrorCode::Conflict, .status = 409, .message = "The job cannot change in its current step"};
inline constexpr ErrorDefinition PinRequired{
    .code = ErrorCode::PinRequired, .status = 403, .message = "A PIN is required to delete the module's data"};
inline constexpr ErrorDefinition PinInvalid{.code = ErrorCode::PinInvalid, .status = 403, .message = "The PIN is not correct"};
inline constexpr ErrorDefinition PinLocked{
    .code = ErrorCode::PinLocked, .status = 429, .message = "Too many wrong PINs; try again later"};
inline constexpr ErrorDefinition PinUnverifiable{.code = ErrorCode::ServiceUnavailable,
                                                 .status = 503,
                                                 .message = "The PIN cannot be checked right now"};
inline constexpr ErrorDefinition Unavailable{
    .code = ErrorCode::ServiceUnavailable, .status = 503, .message = "Modules are not available"};
inline constexpr ErrorDefinition NotSettled{.code = ErrorCode::ServiceUnavailable,
                                            .status = 503,
                                            .message = "Modules are still being read from their services"};
inline constexpr ErrorDefinition RolesHeld{.code = ErrorCode::ModuleRolesHeld,
                                           .status = 409,
                                           .message = "People hold roles of this module; give each a new role to uninstall it"};
inline constexpr ErrorDefinition ReassignRefused{
    .code = ErrorCode::Conflict, .status = 409, .message = "A role could not be reassigned"};
inline constexpr ErrorDefinition RolesUnverifiable{.code = ErrorCode::ServiceUnavailable,
                                                   .status = 503,
                                                   .message = "Who holds the roles of this module cannot be checked right now"};
inline constexpr ErrorDefinition OwnerRequest{
    .code = ErrorCode::Conflict, .status = 409, .message = "The Owner turns modules on directly"};
inline constexpr ErrorDefinition RequestUnavailable{.code = ErrorCode::ServiceUnavailable,
                                                    .status = 503,
                                                    .message = "The request cannot reach the Owner right now"};
}
