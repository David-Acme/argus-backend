#pragma once

#include <functional>
#include <string>
#include <vector>

class CameraMic
{
public:
  using OnAudio = std::function<void(const std::vector<float>& frames16k)>;

  CameraMic() = default;
  ~CameraMic();

  CameraMic(const CameraMic&) = delete;
  CameraMic& operator=(const CameraMic&) = delete;

  bool open(const std::string& rtspUrl, OnAudio onAudio);
  // Reads and decodes audio until a block is emitted. Returns false on
  // end-of-stream, error or timeout.
  bool readBlock();
  void close();

private:
  struct Impl;
  Impl* impl_{nullptr};
};
