#include "pocket-installer.hxx"

#include <feature/provisioning/infra/provision-process.hxx>

#include <drogon/drogon.h>

#include <csignal>
#include <utility>

PocketInstaller::PocketInstaller(PocketInstallerConfig config)
    : config_(std::move(config)), worker_([this](const std::stop_token& stop) { work(stop); })
{
}

PocketInstaller::~PocketInstaller()
{
  stop();
}

void PocketInstaller::request(const std::vector<PocketComponent>& components)
{
  {
    std::scoped_lock lock(mutex_);
    if (stopping_)
      return;
    for (const auto& component : components) {
      if (!isComponentToken(component.variant) ||
          (component.kind == PocketComponentKind::Voice && !isComponentToken(component.voice)))
        continue;
      const auto id = component.id();
      if (const auto found = jobs_.find(id); found != jobs_.end() && found->second == PocketInstallJob::Running)
        continue;
      jobs_[id] = PocketInstallJob::Running;
      queue_.push_back(component);
      LOG_INFO << "Pocket provisioning queued: " << id;
    }
  }
  wake_.notify_all();
}

std::optional<PocketInstallJob> PocketInstaller::job(const PocketComponent& component) const
{
  std::scoped_lock lock(mutex_);
  const auto found = jobs_.find(component.id());
  if (found == jobs_.end())
    return std::nullopt;
  return found->second;
}

void PocketInstaller::stop()
{
  {
    std::scoped_lock lock(mutex_);
    stopping_ = true;
    queue_.clear();
    if (child_ > 0)
      ::kill(-child_, SIGTERM);
  }
  worker_.request_stop();
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void PocketInstaller::work(const std::stop_token& stop)
{
  while (!stop.stop_requested()) {
    PocketComponent component;
    {
      std::unique_lock lock(mutex_);
      if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); }))
        return;
      component = std::move(queue_.front());
      queue_.pop_front();
    }
    install(component);
  }
}

void PocketInstaller::install(const PocketComponent& component)
{
  const auto id = component.id();
  std::vector<std::string> environment{"ARGUS_TTS_CONFIG=" + config_.configFile.string()};
  if (config_.nonCommercialAllowed && config_.nonCommercialAllowed())
    environment.emplace_back("ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1");
  LOG_INFO << "Pocket provisioning started: " << id;
  const int status = runProvisioning({.script = config_.script,
                                      .arguments = component.arguments(),
                                      .environment = std::move(environment),
                                      .onLine = [&id](std::string_view line) {
                                        LOG_INFO << "Pocket provisioning (" << id << "): " << line;
                                      },
                                      .onStart = [this](pid_t child) {
                                        std::scoped_lock lock(mutex_);
                                        child_ = child;
                                        if (stopping_)
                                          ::kill(-child, SIGTERM);
                                      }});
  std::scoped_lock lock(mutex_);
  child_ = 0;
  if (stopping_)
    return;
  if (status == 0) {
    jobs_.erase(id);
    LOG_INFO << "Pocket provisioning finished: " << id;
  }
  else {
    jobs_[id] = PocketInstallJob::Failed;
    LOG_WARN << "Pocket provisioning failed (" << status << "): " << id;
  }
}
