#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class CameraMic
{
public:
  using OnAudio = std::function<void(const std::vector<float>& frames16k)>;

  CameraMic();
  ~CameraMic();

  CameraMic(const CameraMic&) = delete;
  CameraMic& operator=(const CameraMic&) = delete;

  bool open(const std::string& rtspUrl, OnAudio onAudio);
  // Reads and decodes audio until a block is emitted; false on end, error, timeout or closed mic.
  bool readBlock();
  void close();
  const std::string& lastError() const;

private:
  static int recoverCb(void* opaque);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
