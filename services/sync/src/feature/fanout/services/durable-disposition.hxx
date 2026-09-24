#pragma once

#include <cstdint>

enum class DurableDisposition : uint8_t
{
  Ack = 0,
  Nak,
  Term
};
