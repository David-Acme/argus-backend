#pragma once

#include <cstdint>
#include <string>

class SimHash
{
public:
  SimHash() = delete;
  ~SimHash() = delete;

  static uint64_t hash(const std::string& text);
  static int distance(uint64_t a, uint64_t b);
};
