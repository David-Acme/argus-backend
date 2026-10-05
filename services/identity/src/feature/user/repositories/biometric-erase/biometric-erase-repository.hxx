#pragma once

#include "biometric-erase-query.hxx"

#include <drogon/utils/coroutine.h>

class BiometricEraseRepository
{
public:
  [[nodiscard]] drogon::Task<ErasedBiometrics>
  erase(const BiometricEraseInput& input) const;
};
