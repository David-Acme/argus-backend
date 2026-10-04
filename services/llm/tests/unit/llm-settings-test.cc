#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-registry.hxx>
#include <feature/llm/services/sampling-config.hxx>
#include <feature/settings/llm-settings.hxx>
#include <llama.h>
#include <llm/llm-service.hxx>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("an absent sampling key runs the catalog's fallback")
{
  const auto path = std::filesystem::temp_directory_path() / "llm-settings-empty.toml";
  {
    std::ofstream config(path, std::ios::trunc);
    config << "[llm]\n";
  }
  ConfigService::load(path.string());
  const LlmSampling sampling = resolveSampling();
  for (const auto& spec : llmSettingsCatalog()) {
    if (spec.group != "sampling")
      continue;
    CHECK_MESSAGE(spec.apply == SettingApply::Live, spec.key);
    const double fallback = std::stod(spec.fallback);
    if (spec.key == "llm.temperature")
      CHECK(sampling.temperature == doctest::Approx(fallback));
    else if (spec.key == "llm.max_tokens")
      CHECK(sampling.maxTokens == static_cast<int>(fallback));
    else if (spec.key == "llm.top_k")
      CHECK(sampling.topK == static_cast<int>(fallback));
    else if (spec.key == "llm.top_p")
      CHECK(sampling.topP == doctest::Approx(fallback));
    else if (spec.key == "llm.min_p")
      CHECK(sampling.minP == doctest::Approx(fallback));
    else if (spec.key == "llm.penalty_last_n")
      CHECK(sampling.penaltyLastN == static_cast<int>(fallback));
    else if (spec.key == "llm.penalty_repeat")
      CHECK(sampling.penaltyRepeat == doctest::Approx(fallback));
    else if (spec.key == "llm.penalty_freq")
      CHECK(sampling.penaltyFreq == doctest::Approx(fallback));
    else if (spec.key == "llm.penalty_present")
      CHECK(sampling.penaltyPresent == doctest::Approx(fallback));
    else
      CHECK(spec.key == "llm.seed");
  }
  std::filesystem::remove(path);
}

TEST_CASE("resolveSampling keeps a hand-edited value inside its sane range")
{
  const auto path = std::filesystem::temp_directory_path() / "llm-settings-wild.toml";
  {
    std::ofstream config(path, std::ios::trunc);
    config << "[llm]\nmax_tokens = 100000\ntemperature = -1.0\ntop_k = 0\ntop_p = 5.0\n"
           << "min_p = 0.9\npenalty_repeat = 0.2\npenalty_freq = -1.0\nseed = -4\n";
  }
  ConfigService::load(path.string());
  const LlmSampling sampling = resolveSampling();
  CHECK(sampling.maxTokens == 4096);
  CHECK(sampling.temperature == doctest::Approx(0.0F));
  CHECK(sampling.topK == 1);
  CHECK(sampling.topP == doctest::Approx(1.0F));
  CHECK(sampling.minP == doctest::Approx(0.5F));
  CHECK(sampling.penaltyRepeat == doctest::Approx(1.0F));
  CHECK(sampling.penaltyFreq == doctest::Approx(0.0F));
  CHECK(sampling.seed == LlmSampling{}.seed);
  std::filesystem::remove(path);
}

namespace
{
int generatedTokens(LlmService& engine)
{
  ChatRequest request;
  request.messages = {{.role = "user", .content = "Cuenta del uno al cien en palabras, separados por comas."}};
  request.temperature = 0.0F;
  int tokens = 0;
  engine.chatStream(request, [&tokens](const std::string& token, bool done) {
    if (!done && !token.empty())
      ++tokens;
  });
  return tokens;
}
}

TEST_CASE("a sampling change through the registry applies to the next generation")
{
  const auto path = std::filesystem::temp_directory_path() / "llm-settings-live.toml";
  {
    std::ofstream config(path, std::ios::trunc);
    config << "[llm]\nmodel_path = \"" << ARGUS_TEST_LLM_MODELS_DIR
           << "/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf\"\ncontext_size = 4096\n"
           << "max_tokens = 16\ntemperature = 0.85\nseed = 42\n";
  }
  ConfigService::load(path.string());
  llama_backend_init();
  {
    LlmService engine;
    engine.init();
    REQUIRE_MESSAGE(engine.isLoaded(), "run scripts/setup.sh first");
    SettingsRegistry registry(llmSettingsCatalog());
    registry.onChange([&engine](const std::vector<std::string>&) { engine.refreshSampling(); });

    CHECK(engine.defaultMaxTokens() == 16);
    const int before = generatedTokens(engine);
    CHECK(before == 16);

    const auto result = registry.update({{.key = "llm.max_tokens", .value = "64"},
                                         {.key = "llm.temperature", .value = "0.2"},
                                         {.key = "llm.min_p", .value = "0.05"}});
    REQUIRE(result.rejected.empty());
    CHECK(engine.defaultMaxTokens() == 64);
    CHECK(engine.defaultTemperature() == doctest::Approx(0.2F));
    CHECK(engine.sampling().minP == doctest::Approx(0.05F));

    const int after = generatedTokens(engine);
    MESSAGE("tokens before " << before << ", after " << after);
    CHECK(after > 16);
    CHECK(after <= 64);
    engine.shutdown();
  }
  llama_backend_free();
  std::filesystem::remove(path);
}
