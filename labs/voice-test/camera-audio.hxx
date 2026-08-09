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
  // Reads and decodes audio until a block is emitted. Returns false on
  // end-of-stream, error or timeout, or when the mic is not open.
  bool readBlock();
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
