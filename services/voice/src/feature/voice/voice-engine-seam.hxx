#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <chrono>
#include <identity/identity-client.hxx>
#include <identity/voiceprint-client.hxx>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <llm/llm-service.hxx>
#include <llm/llm-remote.hxx>
#include <shared/services/vad/vad-service.hxx>
#include <stt/stt-remote.hxx>
#include <tts/tts-remote.hxx>
#include <stop_token>
#include <string>
#include <vector>

struct VoiceTranscribeInput
{
  const std::vector<float>& samples;
  int32_t sampleRate{0};
  std::string language;
};

struct VoiceSttStreamInput
{
  int32_t sampleRate{0};
  std::string language;
};

class IVoiceSttStream
{
public:
  virtual ~IVoiceSttStream() = default;

  virtual void push(std::span<const float> samples) = 0;
  virtual void flush() = 0;
  [[nodiscard]] virtual std::string finish() = 0;
};

class IVoiceStt
{
public:
  virtual ~IVoiceStt() = default;

  virtual std::string transcribe(const VoiceTranscribeInput& input) = 0;
  [[nodiscard]] virtual std::unique_ptr<IVoiceSttStream> openStream(const VoiceSttStreamInput&)
  {
    return nullptr;
  }
};

class IVoiceTts
{
public:
  virtual ~IVoiceTts() = default;

  virtual float defaultSpeed(std::stop_token cancellation = {}) const = 0;
  virtual int sampleRate(std::stop_token cancellation = {}) const = 0;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk)
  {
    synthesizeStream({.request = req, .onChunk = std::move(onChunk), .cancellation = {}});
  }
  virtual void synthesizeStream(TtsRemoteStreamInput input) = 0;
};

class IVoiceLlm
{
public:
  virtual ~IVoiceLlm() = default;

  virtual void chatStream(LlmStreamInput input) = 0;
};

struct VoiceNameWrite
{
  int64_t userId{0};
  std::string role;
  std::string name;
};

class IVoiceIdentity
{
public:
  virtual ~IVoiceIdentity() = default;

  virtual void updateUserName(const VoiceNameWrite& write) = 0;
};

struct VoiceSpeakerInput
{
  const std::vector<float>& samples;
  int32_t sampleRate{0};
  int64_t userId{0};
  const std::string& deviceHash;
  const std::string& callKey;
};

struct VoiceSpeaker
{
  int64_t userId{0};
  std::string name;
  float score{0.0F};
};

class IVoiceSpeaker
{
public:
  virtual ~IVoiceSpeaker() = default;

  [[nodiscard]] virtual std::optional<VoiceSpeaker> identify(const VoiceSpeakerInput& input) = 0;
  virtual void closeCall(const std::string& callKey) = 0;
};

class GrpcVoiceSpeaker final : public IVoiceSpeaker
{
public:
  [[nodiscard]] std::optional<VoiceSpeaker> identify(const VoiceSpeakerInput& input) override;
  void closeCall(const std::string& callKey) override;

private:
  std::shared_ptr<const VoiceprintClient> clientFor(const std::string& target);

  mutable std::mutex mutex_;
  std::string cachedTarget_;
  std::string cachedSecret_;
  std::shared_ptr<const VoiceprintClient> client_;
};

class GrpcVoiceIdentity final : public IVoiceIdentity
{
public:
  void updateUserName(const VoiceNameWrite& write) override;

private:
  std::shared_ptr<const IdentityClient> clientFor(const std::string& target);

  mutable std::mutex mutex_;
  std::string cachedTarget_;
  std::string cachedSecret_;
  std::shared_ptr<const IdentityClient> client_;
};

class RemoteVoiceTts final : public IVoiceTts
{
public:
  using IVoiceTts::synthesizeStream;

  float defaultSpeed(std::stop_token cancellation = {}) const override;
  int sampleRate(std::stop_token cancellation = {}) const override;

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    client_.synthesizeStream(std::move(input));
  }

private:
  struct Capabilities
  {
    float speed{1.0F};
    int sampleRate{0};
    std::chrono::steady_clock::time_point at{};
  };

  Capabilities capabilities(const std::stop_token& cancellation) const;

  TtsClient client_;
  mutable std::mutex mutex_;
  mutable std::optional<Capabilities> cached_;
};

class RemoteVoiceStt final : public IVoiceStt
{
public:
  std::string transcribe(const VoiceTranscribeInput& input) override;
  [[nodiscard]] std::unique_ptr<IVoiceSttStream> openStream(const VoiceSttStreamInput& input) override;

  static bool supportsLanguage(const std::string& lang);

private:
  SttClient client_;
};

class RemoteVoiceLlm final : public IVoiceLlm
{
public:
  void chatStream(LlmStreamInput input) override;

private:
  std::shared_ptr<const LlmClient>
  clientFor(const LlmRemoteConfig& config);

  mutable std::mutex mutex_;
  std::string cachedUrl_;
  int cachedTimeoutMs_{0};
  std::shared_ptr<const LlmClient> client_;
};

class IVoiceVad
{
public:
  virtual ~IVoiceVad() = default;

  [[nodiscard]] virtual std::unique_ptr<VadModel> createModel() const = 0;
};

class SileroVoiceVad final : public IVoiceVad
{
public:
  [[nodiscard]] std::unique_ptr<VadModel> createModel() const override
  {
    return makeSileroVadModel();
  }
};

IVoiceStt& voiceStt();
IVoiceTts& voiceTts();
IVoiceLlm& voiceLlm();
IVoiceIdentity& voiceIdentity();
IVoiceVad& voiceVad();
IVoiceSpeaker& voiceSpeaker();

struct VoiceEngineSeam
{
  IVoiceStt& stt = voiceStt();
  IVoiceTts& tts = voiceTts();
  IVoiceLlm& llm = voiceLlm();
  IVoiceIdentity& identity = voiceIdentity();
  IVoiceVad& vad = voiceVad();
  IVoiceSpeaker& speaker = voiceSpeaker();
};
