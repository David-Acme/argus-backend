#pragma once

#include <optional>
#include <stdexcept>

template <typename T>
const T& requireValue(const std::optional<T>& value)
{
  if (!value)
    throw std::runtime_error("test: expected a value");
  return *value;
}
