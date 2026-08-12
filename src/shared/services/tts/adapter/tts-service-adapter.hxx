#pragma once

#include <config/service.hxx>
#include <shared/services/tts/tts-service.hxx>

class TtsServiceAdapter : public IService
{
public:
  std::string name() const override { return "tts"; }
  std::string version() const override { return "1.0.0"; }
  bool initialize() override;
  bool isLoaded() const override;
  void shutdown() override;
  Json::Value health() const override;

  TtsService& service() { return ttsService_; }

private:
  TtsService ttsService_;
};
