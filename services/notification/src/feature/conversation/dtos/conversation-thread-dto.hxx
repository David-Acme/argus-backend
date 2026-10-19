#pragma once

#include <cstdint>

struct ConversationThreadDto
{
  int64_t counterpartId{0};

  static ConversationThreadDto fromPath(int64_t counterpartId);
};
