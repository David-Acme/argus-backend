#pragma once

#include <cstdint>
#include <shared/enums.hxx>
#include <string>
#include <vector>

struct ToolCall
{
  MemoryType type;
  int priority;
  std::string content;
};

class ToolParser
{
public:
  ToolParser(std::string start, std::string end);

  std::string feed(const std::string& chunk, std::vector<ToolCall>& out);
  void flush(std::vector<ToolCall>& out);

private:
  std::string start_;
  std::string end_;
  std::string buffer_;
  bool inBlock_ = false;
};
