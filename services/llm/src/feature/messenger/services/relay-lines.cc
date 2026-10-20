#include "relay-lines.hxx"

namespace relay_lines
{

namespace
{

bool english(std::string_view lang)
{
  return lang.starts_with("en");
}

std::string joined(const std::vector<std::string>& words, std::string_view separator)
{
  std::string out;
  for (const auto& word : words) {
    if (!out.empty())
      out += separator;
    out += word;
  }
  return out;
}

}

std::string_view outcome(RelayOutcome value, std::string_view lang)
{
  const bool en = english(lang);
  switch (value) {
  case RelayOutcome::Delivered: return en ? "I told them." : "Se lo dije.";
  case RelayOutcome::Notified: return en ? "I left them the message." : "Le dejé el mensaje.";
  case RelayOutcome::NoAnswer:
    return en ? "They did not answer; I left the message." : "No contestó, le dejé el mensaje.";
  case RelayOutcome::NoRecipient:
    return en ? "I cannot find anyone with that name." : "No encuentro a nadie con ese nombre.";
  case RelayOutcome::Unavailable:
    return en ? "I cannot send it right now." : "Ahora no puedo enviarlo.";
  }
  return {};
}

std::string consentQuestion(const ConsentLine& line)
{
  if (english(line.lang))
    return "Shall I tell " + std::string(line.recipient) + ": “" + std::string(line.message) + "”?";
  return "¿Le digo a " + std::string(line.recipient) + ": «" + std::string(line.message) + "»?";
}

std::string_view consentExpired(std::string_view lang)
{
  return english(lang) ? "I did not send it; tell me the message again." : "No lo envié; dime otra vez el mensaje.";
}

std::string recipientQuestion(const std::vector<std::string>& candidates, std::string_view lang)
{
  if (candidates.empty())
    return english(lang) ? "Who should I tell?" : "¿A quién se lo digo?";
  if (english(lang))
    return "Should I tell " + joined(candidates, " or ") + "?";
  return "¿Se lo digo a " + joined(candidates, " o ") + "?";
}

std::string messageFromSender(const SenderLine& line)
{
  if (english(line.lang))
    return std::string(line.sender) + " says: " + std::string(line.message);
  return std::string(line.sender) + " te manda decir: " + std::string(line.message);
}

std::string_view visitorAsk(std::string_view lang)
{
  return english(lang) ? "How can I help you?" : "¿En qué le puedo ayudar?";
}

std::string_view visitorDone(std::string_view lang)
{
  return english(lang) ? "Thank you, I will pass it on." : "Gracias, se lo hago llegar.";
}

std::string visitorTitle(const VisitorTitleLine& line)
{
  if (english(line.lang))
    return "A visitor at " + std::string(line.camera);
  return "Un visitante en " + std::string(line.camera);
}

std::string visitorMessage(const VisitorLine& line)
{
  if (english(line.lang))
    return "A visitor at " + std::string(line.camera) + " says: " + std::string(line.need);
  return "Un visitante en " + std::string(line.camera) + " dice: " + std::string(line.need);
}

}
