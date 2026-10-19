#pragma once

#include <optional>
#include <stdexcept>
#include <utility>

template <typename T>
T valueOf(std::optional<T> value)
{
  if (!value)
    throw std::runtime_error("test: expected a value");
  return *std::move(value);
}
