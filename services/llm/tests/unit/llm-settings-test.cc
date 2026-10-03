#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-registry.hxx>
#include <feature/llm/services/sampling-config.hxx>
#include <feature/settings/llm-settings.hxx>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
std::filesystem::path scratchConfig()
{
  const auto path = std::filesystem::temp_directory_path() / "llm-settings-test.toml";
  std::ofstream config(path, std::ios::trunc);
  config << "[llm]\n"
         << "max_tokens = 256\n"
         << "temperature = 0.85\n"
         << "top_k = 20\n"
         << "top_p = 0.8\n"
         << "seed = 0\n"
         << "[memory]\n"
         << "recall_top_k = 4\n";
  return path;
}
}

TEST_CASE("the llm catalog builds a registry")
{
  CHECK_NOTHROW(SettingsRegistry{llmSettingsCatalog()});
}

TEST_CASE("the llm catalog exposes no infrastructure key")
{
  constexpr std::array<std::string_view, 8> forbidden{
      "path", "file", "target", "credential", "secret", "port", "address", "schema"};
  for (const auto& spec : llmSettingsCatalog())
    for (const auto fragment : forbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("the llm catalog names its basic keys and stable groups")
{
  const auto catalog = llmSettingsCatalog();
  const auto basic = std::ranges::count_if(catalog, [](const SettingSpec& spec) {
    return spec.level == SettingLevel::Basic;
  });
  CHECK(basic == 2);
  for (const auto& spec : catalog)
    CHECK((spec.group == "sampling" || spec.group == "memory" || spec.group == "engine"));
}

TEST_CASE("resolveSampling reads an applied temperature")
{
  const auto path = scratchConfig();
  ConfigService::load(path.string());
  CHECK(resolveSampling().temperature == doctest::Approx(0.85F));
  CHECK(resolveSampling().maxTokens == 256);

  SettingsRegistry registry(llmSettingsCatalog());
  const auto result = registry.update({{.key = "llm.temperature", .value = "1.2"},
                                       {.key = "llm.max_tokens", .value = "512"}});
  CHECK(result.rejected.empty());
  CHECK(resolveSampling().temperature == doctest::Approx(1.2F));
  CHECK(resolveSampling().maxTokens == 512);

  const auto refused = registry.update({{.key = "llm.temperature", .value = "3"}});
  CHECK(refused.rejected.size() == 1);
  CHECK(resolveSampling().temperature == doctest::Approx(1.2F));
  std::filesystem::remove(path);
}
