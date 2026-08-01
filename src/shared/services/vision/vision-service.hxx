#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct mtmd_context;

struct VisionRequest
{
  std::vector<unsigned char> imageRgb;
  uint32_t width{0};
  uint32_t height{0};
  std::string prompt{"Describe esta imagen de seguridad."};
  int32_t maxTokens{512};
  float temperature{0.3f};
};

class VisionService
{
public:
  VisionService() = delete;
  ~VisionService() = delete;

  static void init();
  static void shutdown();

  static std::string describe(const VisionRequest& req);

  // Coroutine variant: runs inference off the event loop.
  static drogon::Task<std::string> describeAsync(const VisionRequest& req);

  static bool isLoaded();

private:
  static std::string buildVisionPrompt(const std::string& userPrompt);

  static std::unique_ptr<llama_model, void (*)(llama_model*)> model_;
  static std::unique_ptr<llama_context, void (*)(llama_context*)> context_;
  static std::unique_ptr<mtmd_context, void (*)(mtmd_context*)> mtmd_;
  static int64_t contextSize_;
  static bool loaded_;
  static bool hasEncoder_;
  static std::mutex mutex_;

  static constexpr const char* SYSTEM_PROMPT =
      R"SYSPROMPT(Eres Argus Vision, un sistema de análisis visual para vigilancia de seguridad.
Tu función es analizar imágenes de cámaras de seguridad y describir:
1. Personas detectadas (cantidad, ubicación, características visibles, actividad).
2. Vehículos (tipo, color, dirección, matrícula si es visible).
3. Objetos anómalos o abandonados.
4. Movimientos o comportamientos sospechosos.
5. Cambios en la escena respecto al estado normal.

NO tienes acceso a la nube — todo el procesamiento es local.
Sé conciso, objetivo y prioriza detalles relevantes para seguridad. Responde en el mismo idioma en que te hablen.)SYSPROMPT";
};
