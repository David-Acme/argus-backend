#include "tool-parser.hxx"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{

std::string_view nextToken(std::string_view& rest)
{
  size_t start = 0;
  while (start < rest.size() && std::isspace(static_cast<unsigned char>(rest[start])))
    ++start;
  rest.remove_prefix(start);
  size_t end = 0;
  while (end < rest.size() &&
         !std::isspace(static_cast<unsigned char>(rest[end])))
    ++end;
  std::string_view tok = rest.substr(0, end);
  rest.remove_prefix(end);
  return tok;
}

std::optional<ToolCall> parseBlock(const std::string& block)
{
  std::string_view rest = block;
  if (nextToken(rest) != "save")
    return std::nullopt;

  MemoryType type = MemoryType::Persona;
  int priority = 85;
  std::string content;

  while (!rest.empty()) {
    const std::string_view tok = nextToken(rest);
    if (tok.empty())
      break;
    if (tok.substr(0, 5) == "type=") {
      type = memoryTypeFromString(std::string(tok.substr(5)));
    }
    else if (tok.substr(0, 9) == "priority=") {
      priority = std::atoi(std::string(tok.substr(9)).c_str());
    }
    else if (tok.substr(0, 8) == "content=") {
      content = std::string(tok.substr(8));
      const size_t first = rest.find_first_not_of(" \t\r\n");
      if (first != std::string_view::npos)
        rest.remove_prefix(first);
      if (!rest.empty())
        content += " " + std::string(rest);
      break;
    }
  }

  if (content.empty())
    return std::nullopt;

  return ToolCall{
      .type = type,
      .priority = std::clamp(priority, 0, 100),
      .content = std::move(content),
  };
}

} // namespace

ToolParser::ToolParser(std::string start, std::string end)
    : start_(std::move(start)), end_(std::move(end))
{
}

std::string ToolParser::feed(const std::string& chunk, std::vector<ToolCall>& out)
{
  std::string cleaned;
  cleaned.reserve(chunk.size());

  for (size_t i = 0; i < chunk.size();) {
    if (!inBlock_) {
      const size_t hit = chunk.find(start_, i);
      if (hit == std::string::npos) {
        cleaned += chunk.substr(i);
        break;
      }
      cleaned += chunk.substr(i, hit - i);
      buffer_.clear();
      inBlock_ = true;
      i = hit + start_.size();
    }
    else {
      const size_t hit = chunk.find(end_, i);
      if (hit == std::string::npos) {
        buffer_ += chunk.substr(i);
        break;
      }
      buffer_ += chunk.substr(i, hit - i);
      if (const auto call = parseBlock(buffer_))
        out.push_back(*call);
      inBlock_ = false;
      i = hit + end_.size();
    }
  }

  return cleaned;
}

void ToolParser::flush(std::vector<ToolCall>&)
{
  inBlock_ = false;
  buffer_.clear();
}
