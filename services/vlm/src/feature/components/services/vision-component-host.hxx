#pragma once

#include <settings/component-host.hxx>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>

struct VisionComponentHostInput
{
  std::filesystem::path modelsDir;
  ComponentFetch fetch;
  std::function<bool()> loaded;
  std::function<void()> load;
  std::function<void()> unload;
};

class VisionComponentHost final : public ComponentHost
{
public:
  static constexpr std::string_view kComponent = "vision";

  explicit VisionComponentHost(VisionComponentHostInput input);
  ~VisionComponentHost() override;
  VisionComponentHost(const VisionComponentHost&) = delete;
  VisionComponentHost& operator=(const VisionComponentHost&) = delete;
  VisionComponentHost(VisionComponentHost&&) = delete;
  VisionComponentHost& operator=(VisionComponentHost&&) = delete;

  [[nodiscard]] ComponentStatus status(const ComponentSpec& spec) const override;
  ComponentStatus install(const ComponentSpec& spec) override;
  ComponentStatus cancel(const ComponentSpec& spec) override;
  ComponentStatus remove(const ComponentSpec& spec) override;

private:
  enum class LoadState : std::uint8_t
  {
    Idle,
    Loading,
    Tried,
    Removing
  };

  [[nodiscard]] bool readyOrLoad();

  VisionComponentHostInput input_;
  std::mutex mutex_;
  LoadState state_{LoadState::Idle};
  std::jthread loader_;
  DiskComponentHost disk_;
};
