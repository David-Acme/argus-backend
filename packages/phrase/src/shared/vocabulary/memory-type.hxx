#pragma once

#include <cstdint>
#include <string>

enum class MemoryType : uint8_t
{
  Persona = 0,
  Episodic,
  Instruction,
  System
};

inline std::string memoryTypeToString(MemoryType t)
{
  switch (t) {
    case MemoryType::Episodic:
      return "episodic";
    case MemoryType::Instruction:
      return "instruction";
    case MemoryType::System:
      return "system";
    default:
      return "persona";
  }
}

inline MemoryType memoryTypeFromString(const std::string& s)
{
  if (s == "episodic")
    return MemoryType::Episodic;
  if (s == "instruction")
    return MemoryType::Instruction;
  if (s == "system")
    return MemoryType::System;
  return MemoryType::Persona;
}
