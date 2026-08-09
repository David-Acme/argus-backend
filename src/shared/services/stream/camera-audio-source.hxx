#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct CameraAudioInput
{
  int64_t cameraId{1};
  int targetRate{16000};
  size_t ringCapacity{16000 * 30};
};

// Reads the camera microphone through the go2rtc source that already holds
// the RTSP session (the same connection that serves the video). Opening a
// second RTSP session for audio is not possible on the C225: it drops the
// oldest RTSP session whenever the 8800 talk channel is opened.
class CameraAudioSource
{
public:
  explicit CameraAudioSource(const CameraAudioInput& input);
  ~CameraAudioSource();

  CameraAudioSource(const CameraAudioSource&) = delete;
  CameraAudioSource& operator=(const CameraAudioSource&) = delete;

  bool open();
  // Pulls up to one decoded block of 16 kHz floats. Returns false on
  // end-of-stream, timeout or error (call open() again to reconnect).
  bool read(std::vector<float>& out);
  void close();
  bool isOpen() const;
  const std::string& lastError() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
