#include "stt-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>

ListenerConfig SttConfig::resolveListener()
{
  return ListenerConfig::resolve(7030);
}

SttRpcConfig SttConfig::resolveRpc()
{
  SttRpcConfig config;
  config.address = ConfigService::getString("rpc.address");
  config.credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(config.credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  config.settingsCredentials = settingsCallers(config.credentials);
  withoutSettingsCaller(config.credentials);
  return config;
}

std::filesystem::path SttConfig::resolveComponentsRoot()
{
  const std::string root = ConfigService::getString("components.models_dir");
  return root.empty() ? std::filesystem::path("models") : std::filesystem::path(root);
}

SttSpotConfig SttConfig::resolveSpot()
{
  SttSpotConfig config;
  const auto text = [](const std::string& key, const std::string& fallback) {
    const std::string value = ConfigService::getString(key);
    return value.empty() ? fallback : value;
  };
  config.modelDir = text("kws.model_dir", "models/kws");
  config.encoder = text("kws.encoder", "encoder.onnx");
  config.decoder = text("kws.decoder", "decoder.onnx");
  config.joiner = text("kws.joiner", "joiner.onnx");
  config.tokens = text("kws.tokens", "tokens.txt");
  config.keywordsFile = text("kws.keywords_file", "keywords.txt");
  if (const double threshold = ConfigService::getDouble("kws.threshold"); threshold > 0.0)
    config.threshold = static_cast<float>(std::min(threshold, 1.0));
  if (const double score = ConfigService::getDouble("kws.score"); score > 0.0)
    config.score = static_cast<float>(score);
  if (const int paths = ConfigService::getInt("kws.max_active_paths"); paths > 0)
    config.maxActivePaths = paths;
  if (const int blanks = ConfigService::getInt("kws.trailing_blanks"); blanks > 0)
    config.trailingBlanks = blanks;
  if (const int threads = ConfigService::getInt("kws.threads"); threads > 0)
    config.threads = threads;
  return config;
}
