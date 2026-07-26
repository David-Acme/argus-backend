#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;

struct ChatMessage
{
  std::string role;
  std::string content;
};

struct ChatRequest
{
  std::vector<ChatMessage> messages;
  int32_t maxTokens{512};
  float temperature{0.3f};
  bool resetContext{true};
};

using TokenCallback = std::function<void(const std::string& token, bool done)>;

class LlmService
{
public:
  LlmService() = delete;
  ~LlmService() = delete;

  static void init();
  static void shutdown();

  static std::string chat(const ChatRequest& req);
  static void chatStream(const ChatRequest& req, TokenCallback onToken);

  static bool isLoaded();

private:
  static std::string buildPrompt(const std::vector<ChatMessage>& messages);
  static std::string generate(const std::string& formattedPrompt,
                              float temperature, int32_t maxTokens,
                              bool resetContext);
  static void generateStream(const std::string& formattedPrompt,
                             float temperature, int32_t maxTokens,
                             bool resetContext,
                             TokenCallback onToken);

  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static int64_t contextSize_;
  static bool loaded_;

  static constexpr const char* SYSTEM_PROMPT = R"SYSPROMPT(Eres Argus, un asistente de seguridad inteligente para un sistema de vigilancia local. Tus funciones son:
1. Analizar eventos de seguridad (detecciones de movimiento, personas, vehículos, sonidos anómalos).
2. Responder preguntas sobre el estado del sistema: cámaras activas, eventos recientes, personas reconocidas.
3. Ayudar a configurar reglas de seguridad: zonas de exclusión, horarios de vigilancia, sensibilidad de detección.
4. Explicar alertas de seguridad en lenguaje claro y accionable.
5. NO tienes acceso a la nube — todo el procesamiento es local.

Sé conciso, preciso y prioriza la seguridad. Cuando no sepas algo, dilo directamente. Responde siempre en el mismo idioma en que te hablen.)SYSPROMPT";
};
