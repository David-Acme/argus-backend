#include "notification-thread-read-dto.hxx"

#include <utility>
#include <validation/validation_dsl.hxx>

NotificationThreadReadDto NotificationThreadReadDto::fromPath(
    std::string threadKey)
{
  NotificationThreadReadDto dto{.threadKey = std::move(threadKey)};

  START_VALIDATION(NotificationThreadReadDto, dto)
  IS_NOT_EMPTY(threadKey)
  MAX_LENGTH(threadKey, kMaxThreadKeyLength)
  END_VALIDATION()

  return dto;
}
