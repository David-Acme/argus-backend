#include "conversation-thread-dto.hxx"

#include <validation/validation_dsl.hxx>

ConversationThreadDto ConversationThreadDto::fromPath(int64_t counterpartId)
{
  ConversationThreadDto dto{.counterpartId = counterpartId};

  START_VALIDATION(ConversationThreadDto, dto)
  IS_NON_NEGATIVE(counterpartId)
  END_VALIDATION()

  return dto;
}
