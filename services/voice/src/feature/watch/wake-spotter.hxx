#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>

struct WakeHit
{
  std::string keyword;
};

class IWakeStream
{
public:
  virtual ~IWakeStream() = default;

  [[nodiscard]] virtual bool push(std::span<const float> samples) = 0;
  [[nodiscard]] virtual bool alive() const = 0;
};

class IWakeSpotter
{
public:
  virtual ~IWakeSpotter() = default;

  [[nodiscard]] virtual std::unique_ptr<IWakeStream> open(std::function<void(const WakeHit&)> onHit) = 0;
};

class SttWakeSpotter final : public IWakeSpotter
{
public:
  [[nodiscard]] std::unique_ptr<IWakeStream> open(std::function<void(const WakeHit&)> onHit) override;
};
