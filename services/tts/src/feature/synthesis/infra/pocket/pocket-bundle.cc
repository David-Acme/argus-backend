#include "pocket-bundle.hxx"

#include <nlohmann/json.hpp>

#include <fstream>
#include <numeric>
#include <stdexcept>

namespace
{
std::vector<PocketStateSpec> readStates(const nlohmann::json& entries)
{
  if (!entries.is_array() || entries.empty())
    throw std::runtime_error("Pocket bundle state manifest is missing");
  std::vector<PocketStateSpec> states;
  states.reserve(entries.size());
  for (const auto& entry : entries) {
    const auto dtype = entry.at("dtype").get<std::string>();
    if (dtype != "float32" && dtype != "int64")
      throw std::runtime_error("Pocket bundle state dtype is not supported: " + dtype);
    PocketStateSpec spec{.module = entry.at("module").get<std::string>(),
                         .key = entry.at("key").get<std::string>(),
                         .inputName = entry.at("input_name").get<std::string>(),
                         .outputName = entry.at("output_name").get<std::string>(),
                         .shape = entry.at("shape").get<std::vector<std::int64_t>>(),
                         .dtype = dtype == "int64" ? PocketDtype::Int64 : PocketDtype::Float32,
                         .onesFill = entry.at("fill").get<std::string>() == "ones"};
    for (const auto dimension : spec.shape) {
      if (dimension <= 0)
        throw std::runtime_error("Pocket bundle state has an empty dimension: " + spec.module);
    }
    states.push_back(std::move(spec));
  }
  return states;
}
}

PocketBundle loadPocketBundle(const std::filesystem::path& directory)
{
  std::ifstream stream(directory / "bundle.json");
  if (!stream)
    throw std::runtime_error("Pocket bundle not found in " + directory.string());
  const auto json = nlohmann::json::parse(stream);
  if (json.at("schema_version").get<int>() != 3)
    throw std::runtime_error("Pocket bundle schema is not supported");
  PocketBundle bundle;
  bundle.directory = directory;
  bundle.tokenizerFile = json.at("tokenizer_file").get<std::string>();
  bundle.sampleRate = json.at("sample_rate").get<int>();
  bundle.samplesPerFrame = json.at("samples_per_frame").get<int>();
  bundle.latentDim = json.at("latent_dim").get<int>();
  bundle.conditioningDim = json.at("conditioning_dim").get<int>();
  bundle.mimiStepsPerLatent = json.at("mimi_steps_per_latent").get<int>();
  bundle.flowCapacity = json.at("flow_capacity").get<int>();
  bundle.mimiCapacity = json.at("mimi_capacity").get<int>();
  bundle.minFramesBeforeEos = json.at("min_frames_before_eos").get<int>();
  bundle.temperature = json.at("temperature").get<float>();
  bundle.eosThreshold = json.at("eos_threshold").get<float>();
  bundle.tokensPerSecond = json.at("tokens_per_second_estimate").get<double>();
  bundle.genSecondsPadding = json.at("gen_seconds_padding").get<double>();
  bundle.voiceCloning = json.value("voice_cloning", false);
  bundle.appendTerminalPunctuation = json.at("append_terminal_punctuation").get<bool>();
  bundle.capitalizeFirstLetter = json.at("capitalize_first_letter").get<bool>();
  bundle.removeSemicolons = json.at("remove_semicolons").get<bool>();
  bundle.padWithSpaces = json.at("pad_with_spaces_for_short_inputs").get<bool>();
  if (const auto& frames = json.at("model_recommended_frames_after_eos"); frames.is_number_integer())
    bundle.recommendedFramesAfterEos = frames.get<int>();
  const auto& replacements = json.at("replace_characters");
  bundle.replaceCharacters.reserve(replacements.size());
  for (const auto& [from, to] : replacements.items())
    bundle.replaceCharacters.emplace_back(from, to.get<std::string>());
  bundle.flowStates = readStates(json.at("flow_lm_state_manifest"));
  bundle.mimiStates = readStates(json.at("mimi_state_manifest"));
  if (bundle.sampleRate <= 0 || bundle.samplesPerFrame <= 0 || bundle.latentDim <= 0 ||
      bundle.conditioningDim <= 0 || bundle.flowCapacity <= 0 || bundle.mimiCapacity <= 0 ||
      bundle.mimiStepsPerLatent <= 0 || bundle.tokensPerSecond <= 0)
    throw std::runtime_error("Pocket bundle metadata is out of range");
  return bundle;
}

std::size_t stateElements(const PocketStateSpec& spec)
{
  return std::accumulate(spec.shape.begin(), spec.shape.end(), std::size_t{1},
                         [](std::size_t total, std::int64_t dimension) { return total * static_cast<std::size_t>(dimension); });
}
