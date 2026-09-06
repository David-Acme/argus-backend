#pragma once

#include <cstdint>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <string>
#include <vector>

// Narrow engine seams behind the voice session. The legacy singletons back
// them today; Fase 4 engine services re-point these adapters to HTTP clients
// without touching the session again.
class IVoiceStt
{
public:
  virtual ~IVoiceStt() = default;

  virtual std::string transcribe(const std::vector<float>& audioSamples,
                                 int32_t sampleRate) = 0;
  virtual bool setLanguage(const std::string& lang) = 0;
};

class IVoiceTts
{
public:
  virtual ~IVoiceTts() = default;

  virtual float defaultSpeed() const = 0;
  virtual int sampleRate() const = 0;
  virtual void synthesizeStream(const TtsRequest& req,
                                TtsChunkCallback onChunk) = 0;
};

class IVoiceLlm
{
public:
  virtual ~IVoiceLlm() = default;

  virtual void chatStream(const ChatRequest& req, TokenCallback onToken) = 0;
};

// Adapters wrapping the legacy singletons (the current seam wiring).
class SingletonVoiceStt final : public IVoiceStt
{
public:
  std::string transcribe(const std::vector<float>& audioSamples,
                         int32_t sampleRate) override
  {
    return SttService::instance().transcribe(audioSamples, sampleRate);
  }

  bool setLanguage(const std::string& lang) override
  {
    return SttService::instance().setLanguage(lang);
  }
};

class SingletonVoiceTts final : public IVoiceTts
{
public:
  float defaultSpeed() const override
  {
    return TtsService::instance().defaultSpeed();
  }

  int sampleRate() const override
  {
    return TtsService::instance().sampleRate();
  }

  void synthesizeStream(const TtsRequest& req,
                        TtsChunkCallback onChunk) override
  {
    TtsService::instance().synthesizeStream(req, std::move(onChunk));
  }
};

class SingletonVoiceLlm final : public IVoiceLlm
{
public:
  void chatStream(const ChatRequest& req, TokenCallback onToken) override
  {
    LlmService::instance().chatStream(req, std::move(onToken));
  }
};

// Access to the shared AI services (initialized by the service registry).
IVoiceStt& voiceStt();
IVoiceTts& voiceTts();
IVoiceLlm& voiceLlm();

// The engine set backing one voice session; defaults to the legacy singletons.
struct VoiceEngineSeam
{
  IVoiceStt& stt = voiceStt();
  IVoiceTts& tts = voiceTts();
  IVoiceLlm& llm = voiceLlm();
};