#pragma once

#include <settings/component-vocabulary.hxx>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <string>
#include <vector>

class ComponentHost
{
public:
  ComponentHost() = default;
  virtual ~ComponentHost() = default;
  ComponentHost(const ComponentHost&) = delete;
  ComponentHost& operator=(const ComponentHost&) = delete;
  ComponentHost(ComponentHost&&) = delete;
  ComponentHost& operator=(ComponentHost&&) = delete;

  [[nodiscard]] virtual ComponentStatus status(const ComponentSpec& spec) const = 0;
  virtual ComponentStatus install(const ComponentSpec& spec) = 0;
  virtual ComponentStatus cancel(const ComponentSpec& spec) = 0;
  virtual ComponentStatus remove(const ComponentSpec& spec) = 0;
};

struct ComponentFetchInput
{
  const ComponentFile& file;
  std::filesystem::path target;
  std::stop_token stop;
};

using ComponentFetch = std::function<std::string(const ComponentFetchInput&)>;

struct DiskComponentHostInput
{
  std::filesystem::path modelsDir;
  std::vector<std::string> owned;
  ComponentFetch fetch;
  std::function<bool(const std::string&)> ready;
};

class DiskComponentHost final : public ComponentHost
{
public:
  explicit DiskComponentHost(DiskComponentHostInput input);
  ~DiskComponentHost() override;
  DiskComponentHost(const DiskComponentHost&) = delete;
  DiskComponentHost& operator=(const DiskComponentHost&) = delete;
  DiskComponentHost(DiskComponentHost&&) = delete;
  DiskComponentHost& operator=(DiskComponentHost&&) = delete;

  [[nodiscard]] ComponentStatus status(const ComponentSpec& spec) const override;
  ComponentStatus install(const ComponentSpec& spec) override;
  ComponentStatus cancel(const ComponentSpec& spec) override;
  ComponentStatus remove(const ComponentSpec& spec) override;

  [[nodiscard]] static bool acceptable(const ComponentSpec& spec);

private:
  struct Activity
  {
    std::jthread worker;
    bool running{false};
    std::string failure;
  };

  void check(const ComponentSpec& spec) const;
  [[nodiscard]] ComponentStatus statusLocked(const ComponentSpec& spec) const;
  void run(const ComponentSpec& spec, const std::stop_token& stop);
  void stop(const std::string& id);

  DiskComponentHostInput input_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Activity>> activities_;
};
