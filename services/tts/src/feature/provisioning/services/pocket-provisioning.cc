#include "pocket-provisioning.hxx"

#include <config/config-service.hxx>

#include <drogon/drogon.h>

#include <utility>

PocketProvisioning::PocketProvisioning(PocketProvisioningConfig config)
    : config_(std::move(config)), host_(probeProvisioningHost(config_.paths))
{
  if (host_.script && host_.canDownload)
    installer_ = std::make_unique<PocketInstaller>(PocketInstallerConfig{
        .script = *host_.script, .configFile = config_.configFile, .nonCommercialAllowed = &nonCommercialAllowed});
  if (!host_.script)
    LOG_INFO << "Pocket components install on the host only (no provisioning script next to "
             << config_.paths.modelsDir.string() << ")";
  else
    LOG_INFO << "Pocket components install on demand (voices " << (host_.canDownload ? "yes" : "no")
             << ", models " << (host_.canExport ? "yes" : "no") << ")";
}

bool PocketProvisioning::nonCommercialAllowed()
{
  return ConfigService::getBool("tts.pocket_noncommercial_voices");
}

const ProvisioningHost& PocketProvisioning::host() const
{
  return host_;
}

PocketProvisioningView PocketProvisioning::view() const
{
  const auto pocketDir = config_.paths.pocketDir;
  return {.catalog = PocketCatalog::load(pocketDir / "catalog.txt"),
          .host = host_,
          .nonCommercialAllowed = nonCommercialAllowed(),
          .installed = [pocketDir](const PocketComponent& component) {
            return pocketComponentPresent(pocketDir, component);
          },
          .job = [this](const PocketComponent& component) -> std::optional<PocketInstallJob> {
            return installer_ ? installer_->job(component) : std::nullopt;
          }};
}

std::vector<ChoiceState> PocketProvisioning::choiceStates(const SettingSpec& spec) const
{
  return pocketChoiceStates(spec, view());
}

void PocketProvisioning::installFor(const std::vector<std::string>& changedKeys)
{
  if (!installer_)
    return;
  const auto components = pocketComponentsToInstall(changedKeys, view());
  if (!components.empty())
    installer_->request(components);
}

void PocketProvisioning::stop()
{
  if (installer_)
    installer_->stop();
}
