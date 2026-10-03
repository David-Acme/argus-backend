#include "pocket-choice-states.hxx"

#include <feature/synthesis/services/tts-service.hxx>

#include <algorithm>
#include <ranges>

namespace
{
constexpr double kBytesPerMegabyte = 1'000'000.0;
constexpr const char* kProvisionCommand = "services/tts/scripts/provision.sh";
constexpr const char* kNonCommercialOptIn = "ARGUS_TTS_POCKET_NONCOMMERCIAL_VOICES=1 ";

std::string spanishVariant()
{
  return "es-" + TtsService::configuredPocketVariant(TtsLang::ES);
}

PocketComponent variantComponent(std::string variant)
{
  return {.kind = PocketComponentKind::Variant, .variant = std::move(variant), .voice = {}};
}

PocketComponent voiceComponent(std::string variant, std::string voice)
{
  return {.kind = PocketComponentKind::Voice, .variant = std::move(variant), .voice = std::move(voice)};
}

bool nonCommercial(const PocketComponent& component, const PocketCatalog& catalog)
{
  if (component.kind != PocketComponentKind::Voice)
    return false;
  const auto* voice = catalog.voice(component.variant, component.voice);
  return voice != nullptr && voice->nonCommercial();
}

double sizeOf(const PocketComponent& component, const PocketCatalog& catalog)
{
  if (component.kind == PocketComponentKind::Variant) {
    const auto* variant = catalog.variant(component.variant);
    return variant == nullptr ? 0.0 : static_cast<double>(variant->bytes) / kBytesPerMegabyte;
  }
  const auto* voice = catalog.voice(component.variant, component.voice);
  return voice == nullptr ? 0.0 : static_cast<double>(voice->bytes) / kBytesPerMegabyte;
}

bool installable(const PocketComponent& component, const PocketProvisioningView& view)
{
  if (component.kind == PocketComponentKind::Variant)
    return view.catalog.variant(component.variant) != nullptr && view.host.canExport;
  if (view.catalog.voice(component.variant, component.voice) == nullptr || !view.host.canDownload)
    return false;
  return !nonCommercial(component, view.catalog) || view.nonCommercialAllowed;
}

bool present(const PocketComponent& component, const PocketProvisioningView& view)
{
  return view.installed && view.installed(component);
}

std::optional<PocketInstallJob> jobOf(const PocketComponent& component, const PocketProvisioningView& view)
{
  return view.job ? view.job(component) : std::nullopt;
}

ChoiceState stateOf(const std::string& choice, const PocketComponent& component, const PocketProvisioningView& view)
{
  ChoiceState state{.choice = choice,
                    .availability = ChoiceAvailability::Installed,
                    .sizeMb = sizeOf(component, view.catalog),
                    .hostCommand = {}};
  if (present(component, view))
    return state;
  state.hostCommand = pocketHostCommand(component, nonCommercial(component, view.catalog));
  const auto job = jobOf(component, view);
  const bool canInstall = installable(component, view);
  if (job == PocketInstallJob::Running)
    state.availability = ChoiceAvailability::Installing;
  else if (!canInstall)
    state.availability = ChoiceAvailability::HostOnly;
  else if (job == PocketInstallJob::Failed)
    state.availability = ChoiceAvailability::Failed;
  else
    state.availability = ChoiceAvailability::Installable;
  return state;
}

std::vector<std::string> spanishDirectories()
{
  if (TtsService::configuredPocketVariant(TtsLang::ES) == "quality")
    return {"es-quality", "es-fast"};
  return {"es-fast"};
}

ChoiceState pocketEngineState(const std::string& choice, TtsLang lang, const PocketProvisioningView& view)
{
  if (choice != "pocket")
    return {.choice = choice, .availability = ChoiceAvailability::Installed, .sizeMb = 0, .hostCommand = {}};
  if (lang == TtsLang::EN)
    return stateOf(choice, variantComponent("en"), view);
  const auto configured = variantComponent(spanishVariant());
  auto state = stateOf(choice, configured, view);
  const auto directories = spanishDirectories();
  if (std::ranges::any_of(directories, [&view](const std::string& variant) {
        return present(variantComponent(variant), view);
      }))
    state = {.choice = choice, .availability = ChoiceAvailability::Installed, .sizeMb = state.sizeMb, .hostCommand = {}};
  return state;
}

void addMissing(std::vector<PocketComponent>& components, PocketComponent component, const PocketProvisioningView& view)
{
  if (present(component, view) || jobOf(component, view) == PocketInstallJob::Running || !installable(component, view))
    return;
  if (std::ranges::find(components, component) == components.end())
    components.push_back(std::move(component));
}

bool touches(const std::vector<std::string>& keys, std::initializer_list<std::string_view> watched)
{
  return std::ranges::any_of(keys, [watched](const std::string& key) {
    return std::ranges::find(watched, std::string_view(key)) != watched.end();
  });
}
}

std::string PocketComponent::id() const
{
  return kind == PocketComponentKind::Variant ? "variant " + variant : "voice " + variant + ":" + voice;
}

std::vector<std::string> PocketComponent::arguments() const
{
  if (kind == PocketComponentKind::Variant)
    return {"--variant", variant};
  return {"--voice", variant + ":" + voice};
}

std::string pocketHostCommand(const PocketComponent& component, bool nonCommercial)
{
  std::string command = nonCommercial ? kNonCommercialOptIn : "";
  command += kProvisionCommand;
  for (const auto& argument : component.arguments())
    command += " " + argument;
  return command;
}

bool pocketComponentPresent(const std::filesystem::path& pocketDir, const PocketComponent& component)
{
  std::error_code error;
  if (component.kind == PocketComponentKind::Variant)
    return std::filesystem::is_regular_file(pocketDir / component.variant / "bundle.json", error);
  return std::filesystem::is_regular_file(pocketDir / component.variant / "voices" / (component.voice + ".safetensors"),
                                          error);
}

std::vector<ChoiceState> pocketChoiceStates(const SettingSpec& spec, const PocketProvisioningView& view)
{
  std::vector<ChoiceState> states;
  states.reserve(spec.choices.size());
  for (const auto& choice : spec.choices) {
    if (spec.key == "tts.engine_es")
      states.push_back(pocketEngineState(choice, TtsLang::ES, view));
    else if (spec.key == "tts.engine_en")
      states.push_back(pocketEngineState(choice, TtsLang::EN, view));
    else if (spec.key == "tts.pocket_variant_es")
      states.push_back(stateOf(choice, variantComponent("es-" + choice), view));
    else if (spec.key == "tts.pocket_voice_es")
      states.push_back(stateOf(choice, voiceComponent(spanishVariant(), choice), view));
    else if (spec.key == "tts.pocket_voice_en")
      states.push_back(stateOf(choice, voiceComponent("en", choice), view));
  }
  return states;
}

std::vector<PocketComponent> pocketComponentsToInstall(const std::vector<std::string>& changedKeys,
                                                       const PocketProvisioningView& view)
{
  std::vector<PocketComponent> components;
  if (touches(changedKeys, {"tts.engine_es", "tts.pocket_variant_es", "tts.pocket_voice_es"}) &&
      TtsService::configuredEngine(TtsLang::ES) == SpeechEngineKind::Pocket) {
    addMissing(components, variantComponent(spanishVariant()), view);
    addMissing(components, voiceComponent(spanishVariant(), TtsService::configuredPocketVoice(TtsLang::ES)), view);
  }
  if (touches(changedKeys, {"tts.engine_en", "tts.pocket_voice_en"}) &&
      TtsService::configuredEngine(TtsLang::EN) == SpeechEngineKind::Pocket) {
    addMissing(components, variantComponent("en"), view);
    addMissing(components, voiceComponent("en", TtsService::configuredPocketVoice(TtsLang::EN)), view);
  }
  return components;
}
