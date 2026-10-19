#include <feature/conversation/services/conversation-transcript.hxx>

bool transcribable(const TranscriptExchange& exchange)
{
  const auto blank = [](const std::string& text) { return text.find_first_not_of(" \t\r\n") == std::string::npos; };
  return exchange.userId > 0 && !blank(exchange.userText) && !blank(exchange.reply);
}
