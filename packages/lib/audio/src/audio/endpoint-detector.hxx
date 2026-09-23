#pragma once

#include <cstddef>
#include <cstdint>

struct EndpointConfig
{
  int sampleRate{16000};
  int frameMs{30};
  float speechFloorRatio{4.0F};
  float silenceFloorRatio{2.0F};
  float absoluteFloorRms{16.0F};
  float zcrMin{0.02F};
  float zcrMax{0.40F};
  int startFrames{3};
  int endSilenceMs{700};
  int maxUtteranceMs{10000};
  int minSpeechMs{300};
};

struct EndpointStatus
{
  bool speechStarted{false};
  bool endpointed{false};
};

class EndpointDetector
{
public:
  explicit EndpointDetector(EndpointConfig config);

  EndpointStatus process(const int16_t* samples, std::size_t count);

  bool speechDetected() const { return speechDetected_; }

  void reset();

private:
  void processFrame();

  EndpointConfig config_;
  int frameSamples_{0};
  int frameFill_{0};
  double frameEnergy_{0.0};
  int frameCrossings_{0};
  int16_t previousSample_{0};
  bool hasPrevious_{false};
  double noiseFloor_{0.0};
  bool speechDetected_{false};
  bool speaking_{false};
  int speechFrames_{0};
  int silenceFrames_{0};
  int speechMs_{0};
  int totalMs_{0};
};

namespace audio_endpoint
{

EndpointConfig cameraListenDefaults();

}
