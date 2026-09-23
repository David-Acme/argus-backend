#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <identity/identity-client.hxx>
#include <memory>
#include <mutex>
#include <llm/llm-service.hxx>
#include <llm/details/llm-remote.hxx>
#include <stt/stt-remote.hxx>
#include <tts/tts-remote.hxx>
#include <stop_token>
#include <string>
#include <vector>

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

  virtual void chatStream(const ChatRequest& req, TokenCallback onToken) = 0;
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

  float defaultSpeed(std::stop_token cancellation = {}) const override
  {
    return client_.defaultSpeed(cancellation);
  }

  int sampleRate(std::stop_token cancellation = {}) const override
  {
    return client_.sampleRate(cancellation);
  }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    client_.synthesizeStream(std::move(input));
  }

private:
  TtsClient client_;
};

class RemoteVoiceStt final : public IVoiceStt
{
public:
  std::string transcribe(const std::vector<float>& audioSamples,
                         int32_t sampleRate) override;

  bool setLanguage(const std::string& lang) override;

private:
  std::shared_ptr<const SttHttpClient>
  clientFor(const SttRemoteConfig& config);

  mutable std::mutex mutex_;
  std::string cachedUrl_;
  int cachedTimeoutMs_{0};
  std::shared_ptr<const SttHttpClient> client_;
  std::string lang_;
};

class RemoteVoiceLlm final : public IVoiceLlm
{
public:
  void chatStream(const ChatRequest& req, TokenCallback onToken) override;

private:
  std::shared_ptr<const LlmHttpClient>
  clientFor(const LlmRemoteConfig& config);

  mutable std::mutex mutex_;
  std::string cachedUrl_;
  int cachedTimeoutMs_{0};
  std::shared_ptr<const LlmHttpClient> client_;
};

IVoiceStt& voiceStt();
IVoiceTts& voiceTts();
IVoiceLlm& voiceLlm();
IVoiceIdentity& voiceIdentity();

struct VoiceEngineSeam
{
  IVoiceStt& stt = voiceStt();
  IVoiceTts& tts = voiceTts();
  IVoiceLlm& llm = voiceLlm();
  IVoiceIdentity& identity = voiceIdentity();
};
