#include "tool-policy.hxx"

#include <algorithm>

namespace tool_policy
{

namespace
{
struct Sentences
{
  std::string_view moduleOff;
  std::string_view neverClaim;
};

constexpr std::string_view kMemoryPolicy =
    "Eres Argus. Usa memory.remember cuando el usuario te pide guardar un dato, "
    "memory.remind para un recordatorio con día u hora, memory.recall cuando "
    "pregunta por algo que te contó y memory.forget cuando pide olvidar algo. "
    "Nunca digas que guardaste u olvidaste algo sin haber usado esa herramienta. "
    "Si no hace falta ninguna, responde brevemente.";

constexpr std::string_view kClientActionPolicy =
    " Estás en una llamada y la app del usuario está abierta: si pide ver una "
    "cámara usa app.show_camera, si pide abrir una sección usa app.open, y si "
    "pide cambiar la vigilancia o dice que se va, que duerme o que vuelve, usa "
    "app.set_guard_mode. Nunca digas que hiciste algo en la app sin haber usado "
    "su herramienta. Confirma en una frase lo que hiciste.";

constexpr Sentences kSpanish{
    .moduleOff = "Si una herramienta responde que el módulo está apagado, díselo al usuario y ofrécele activarlo.",
    .neverClaim = "Nunca digas que agendaste, creaste, guardaste, activaste o cancelaste algo si la herramienta no lo "
                  "confirmó; si no pudiste, dilo."};

constexpr Sentences kEnglish{
    .moduleOff = "If a tool answers that the module is off, tell the user and offer to turn it on.",
    .neverClaim = "Never say you scheduled, created, saved, enabled or cancelled something unless the tool confirmed "
                  "it; if you could not, say so."};

const std::string& lineOf(const tools::ToolHandle& tool, bool english)
{
  const argus::mcp::ToolPolicy& policy = tool->spec.policy;
  if (english)
    return policy.english.empty() ? policy.spanish : policy.english;
  return policy.spanish.empty() ? policy.english : policy.spanish;
}

bool belongsToAModule(const tools::ToolHandle& tool)
{
  return !tool->spec.module.empty() && tool->spec.module != "core";
}
}

std::string generated(const PolicyInput& input)
{
  const bool english = input.lang == "en";
  std::vector<std::string> lines;
  for (const auto& tool : input.tools) {
    const std::string& line = lineOf(tool, english);
    if (!line.empty() && std::ranges::find(lines, line) == lines.end())
      lines.push_back(line);
  }
  if (lines.empty())
    return {};
  const Sentences& sentences = english ? kEnglish : kSpanish;
  std::string out;
  for (const auto& line : lines) {
    out += ' ';
    out += line;
  }
  if (std::ranges::any_of(input.tools, belongsToAModule)) {
    out += ' ';
    out += sentences.moduleOff;
  }
  out += ' ';
  out += sentences.neverClaim;
  return out;
}

std::string systemPrompt(const PromptInput& input)
{
  std::string out(kMemoryPolicy);
  out += generated({.tools = input.tools, .lang = input.lang});
  if (input.clientActions)
    out += kClientActionPolicy;
  return out;
}

}
