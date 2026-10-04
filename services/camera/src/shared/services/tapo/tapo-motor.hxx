#pragma once

#include <shared/services/tapo/tapo-transport.hxx>

namespace tapo_motor
{
inline constexpr int kLockedRotor = -64304;

TapoResult outcomeOf(TapoResult result);
}
