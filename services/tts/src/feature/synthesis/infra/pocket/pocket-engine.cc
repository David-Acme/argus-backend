#include "pocket-engine.hxx"

#include "pocket-prompt.hxx"
#include "unigram-tokenizer.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
constexpr std::size_t kMaxDecodeFrames = 8;
constexpr std::chrono::milliseconds kQueuePoll{10};
constexpr double kFadeSeconds = 0.005;
constexpr double kMaxReferenceSeconds = 10.0;
constexpr double kPauseSeconds = 0.08;
constexpr double kPauseFadeSeconds = 0.02;
constexpr double kPauseFrameSeconds = 0.02;
constexpr double kPauseFloorDb = 35.0;
constexpr std::int64_t kCloneGenerationRoom = 64;

struct SessionFile
{
  Ort::Env& env;
  std::filesystem::path path;
  const Ort::SessionOptions& options;
};

std::unique_ptr<Ort::Session> openSession(const SessionFile& file)
{
  if (!std::filesystem::is_regular_file(file.path))
    throw std::runtime_error("Pocket graph missing: " + file.path.filename().string());
  return std::make_unique<Ort::Session>(file.env, file.path.c_str(), file.options);
}

class StateBank
{
public:
  explicit StateBank(const std::vector<PocketStateSpec>& specs)
  {
    slots_.reserve(specs.size());
    for (const auto& spec : specs) {
      Slot slot{.spec = &spec, .inPlace = spec.key == "cache", .floats = {}, .ints = {}, .front = 0};
      const auto elements = stateElements(spec);
      const std::size_t copies = slot.inPlace ? 1 : 2;
      for (std::size_t copy = 0; copy < copies; ++copy) {
        if (spec.dtype == PocketDtype::Int64)
          slot.ints.at(copy).assign(elements, 0);
        else
          slot.floats.at(copy).assign(elements, spec.onesFill ? 1.0F : 0.0F);
      }
      slots_.push_back(std::move(slot));
    }
  }

  void resetSmall()
  {
    for (auto& slot : slots_) {
      if (slot.inPlace)
        continue;
      slot.front = 0;
      for (auto& buffer : slot.floats)
        std::ranges::fill(buffer, slot.spec->onesFill ? 1.0F : 0.0F);
      for (auto& buffer : slot.ints)
        std::ranges::fill(buffer, 0);
    }
  }

  struct VoiceCopy
  {
    const PocketVoice& voice;
    std::int64_t length{0};
  };

  void loadVoice(const VoiceCopy& copy)
  {
    resetSmall();
    std::size_t cacheIndex = 0;
    for (auto& slot : slots_) {
      if (slot.spec->key == "offset") {
        std::ranges::fill(slot.ints.at(slot.front), copy.length);
        continue;
      }
      if (!slot.inPlace || copy.length == 0)
        continue;
      if (cacheIndex >= copy.voice.caches.size())
        throw std::runtime_error("Pocket voice does not cover every attention layer");
      const auto& source = copy.voice.caches.at(cacheIndex++);
      const auto& shape = slot.spec->shape;
      const auto rowFloats = static_cast<std::size_t>(shape.at(3)) * static_cast<std::size_t>(shape.at(4));
      const auto capacity = static_cast<std::size_t>(shape.at(2));
      const auto rows = static_cast<std::size_t>(copy.length);
      if (rows > capacity || source.size() != 2 * rows * rowFloats)
        throw std::runtime_error("Pocket voice does not fit the attention cache");
      auto& target = slot.floats.at(0);
      for (std::size_t half = 0; half < 2; ++half) {
        const auto from = source.begin() + static_cast<std::ptrdiff_t>(half * rows * rowFloats);
        const auto to = target.begin() + static_cast<std::ptrdiff_t>(half * capacity * rowFloats);
        std::ranges::copy(from, from + static_cast<std::ptrdiff_t>(rows * rowFloats), to);
      }
    }
  }

  [[nodiscard]] PocketVoice extractVoice(std::int64_t length) const
  {
    PocketVoice voice{.length = length, .caches = {}};
    for (const auto& slot : slots_) {
      if (!slot.inPlace)
        continue;
      const auto& shape = slot.spec->shape;
      const auto rowFloats = static_cast<std::size_t>(shape.at(3)) * static_cast<std::size_t>(shape.at(4));
      const auto capacity = static_cast<std::size_t>(shape.at(2));
      const auto rows = static_cast<std::size_t>(length);
      std::vector<float> values(2 * rows * rowFloats);
      const auto& source = slot.floats.at(0);
      for (std::size_t half = 0; half < 2; ++half) {
        const auto from = source.begin() + static_cast<std::ptrdiff_t>(half * capacity * rowFloats);
        std::ranges::copy(from, from + static_cast<std::ptrdiff_t>(rows * rowFloats),
                          values.begin() + static_cast<std::ptrdiff_t>(half * rows * rowFloats));
      }
      voice.caches.push_back(std::move(values));
    }
    return voice;
  }

  void bind(Ort::IoBinding& binding, const Ort::MemoryInfo& memory)
  {
    for (auto& slot : slots_) {
      const auto& shape = slot.spec->shape;
      const auto back = slot.inPlace ? slot.front : 1 - slot.front;
      if (slot.spec->dtype == PocketDtype::Int64) {
        auto& input = slot.ints.at(slot.front);
        auto& output = slot.ints.at(back);
        binding.BindInput(slot.spec->inputName.c_str(),
                          Ort::Value::CreateTensor<std::int64_t>(memory, input.data(), input.size(), shape.data(), shape.size()));
        binding.BindOutput(slot.spec->outputName.c_str(),
                           Ort::Value::CreateTensor<std::int64_t>(memory, output.data(), output.size(), shape.data(), shape.size()));
        continue;
      }
      auto& input = slot.floats.at(slot.front);
      auto& output = slot.floats.at(back);
      binding.BindInput(slot.spec->inputName.c_str(),
                        Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size()));
      binding.BindOutput(slot.spec->outputName.c_str(),
                         Ort::Value::CreateTensor<float>(memory, output.data(), output.size(), shape.data(), shape.size()));
    }
  }

  void advance()
  {
    for (auto& slot : slots_) {
      if (!slot.inPlace)
        slot.front = 1 - slot.front;
    }
  }

private:
  struct Slot
  {
    const PocketStateSpec* spec{nullptr};
    bool inPlace{false};
    std::array<std::vector<float>, 2> floats;
    std::array<std::vector<std::int64_t>, 2> ints;
    std::size_t front{0};
  };

  std::vector<Slot> slots_;
};

std::vector<float> endOnPause(std::span<const float> samples, int sampleRate)
{
  const auto rate = static_cast<double>(sampleRate);
  const auto frame = std::max<std::size_t>(1, static_cast<std::size_t>(kPauseFrameSeconds * rate));
  std::vector<float> audio(samples.begin(), samples.end());
  const auto frames = audio.size() / frame;
  if (frames == 0)
    return audio;
  std::vector<double> decibels(frames);
  for (std::size_t index = 0; index < frames; ++index) {
    double energy = 0;
    for (std::size_t offset = 0; offset < frame; ++offset) {
      const auto value = static_cast<double>(audio[(index * frame) + offset]);
      energy += value * value;
    }
    decibels[index] = 20.0 * std::log10(std::sqrt(energy / static_cast<double>(frame)) + 1e-12);
  }
  const auto loudest = *std::ranges::max_element(decibels);
  std::size_t last = 0;
  for (std::size_t index = 0; index < frames; ++index) {
    if (decibels[index] > loudest - kPauseFloorDb)
      last = index;
  }
  const auto end = (last + 1) * frame;
  audio.resize(end);
  const auto fade = std::min(static_cast<std::size_t>(kPauseFadeSeconds * rate), end);
  for (std::size_t index = 0; index < fade; ++index) {
    const auto gain = fade > 1 ? 1.0 - (static_cast<double>(index) / static_cast<double>(fade - 1)) : 0.0;
    audio[end - fade + index] *= static_cast<float>(gain);
  }
  audio.resize(end + static_cast<std::size_t>(kPauseSeconds * rate), 0.0F);
  return audio;
}
}

class LatentQueue
{
public:
  void push(std::span<const float> latent)
  {
    {
      std::scoped_lock lock(mutex_);
      latents_.insert(latents_.end(), latent.begin(), latent.end());
    }
    ready_.notify_all();
  }

  void finish()
  {
    {
      std::scoped_lock lock(mutex_);
      done_ = true;
    }
    ready_.notify_all();
  }

  struct TakeInput
  {
    std::vector<float>& batch;
    std::size_t maxValues{0};
    const std::function<bool()>& stopped;
  };

  bool take(const TakeInput& input)
  {
    std::unique_lock lock(mutex_);
    while (latents_.empty() && !done_) {
      if (input.stopped())
        return false;
      ready_.wait_for(lock, kQueuePoll);
    }
    if (latents_.empty())
      return false;
    const auto count = std::min(latents_.size(), input.maxValues);
    input.batch.assign(latents_.begin(), latents_.begin() + static_cast<std::ptrdiff_t>(count));
    latents_.erase(latents_.begin(), latents_.begin() + static_cast<std::ptrdiff_t>(count));
    return true;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<float> latents_;
  bool done_{false};
};

struct PocketEngine::Impl
{
  explicit Impl(const PocketEngineConfig& config)
      : bundle(loadPocketBundle(config.directory)),
        tokenizer(UnigramTokenizer::load(config.directory / bundle.tokenizerFile)),
        flowStates(bundle.flowStates),
        mimiStates(bundle.mimiStates)
  {
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    const int decoderThreads = std::max(1, config.threads / 2);
    const int generatorThreads = std::max(1, config.threads - decoderThreads);
    options.SetIntraOpNumThreads(generatorThreads);
    options.SetInterOpNumThreads(1);
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
    decoderOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    decoderOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    decoderOptions.SetIntraOpNumThreads(decoderThreads);
    decoderOptions.SetInterOpNumThreads(1);
    decoderOptions.AddConfigEntry("session.intra_op.allow_spinning", "0");
    lightOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    lightOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    lightOptions.SetIntraOpNumThreads(1);
    lightOptions.SetInterOpNumThreads(1);
    const std::string suffix = config.precision == "fp32" ? "" : "_int8";
    const auto open = [&](const std::string& name) {
      return openSession({.env = config.env, .path = config.directory / name, .options = options});
    };
    const auto openLight = [&](const std::string& name) {
      return openSession({.env = config.env, .path = config.directory / name, .options = lightOptions});
    };
    textConditioner = openLight("text_conditioner.onnx");
    flowMain = open("flow_lm_main" + suffix + ".onnx");
    flowStep = openLight("flow_lm_flow" + suffix + ".onnx");
    mimiDecoder = openSession(
        {.env = config.env, .path = config.directory / ("mimi_decoder" + suffix + ".onnx"), .options = decoderOptions});
    if (bundle.voiceCloning && std::filesystem::is_regular_file(config.directory / "voice_encoder.onnx"))
      voiceEncoder = open("voice_encoder.onnx");
    if (flowMain->GetInputCount() != bundle.flowStates.size() + 2 ||
        mimiDecoder->GetInputCount() != bundle.mimiStates.size() + 1)
      throw std::runtime_error("Pocket graphs do not match their bundle manifest");
    flowBinding = std::make_unique<Ort::IoBinding>(*flowMain);
    mimiBinding = std::make_unique<Ort::IoBinding>(*mimiDecoder);
  }

  struct MainOutput
  {
    std::vector<float> conditioning;
    float eosLogit{0};
  };

  struct MainInput
  {
    std::span<float> sequence;
    std::span<float> text;
  };

  MainOutput runMain(const MainInput& input)
  {
    const auto latent = static_cast<std::int64_t>(bundle.latentDim);
    const auto width = static_cast<std::int64_t>(bundle.conditioningDim);
    const std::array<std::int64_t, 3> sequenceShape{1, static_cast<std::int64_t>(input.sequence.size()) / latent, latent};
    const std::array<std::int64_t, 3> textShape{1, static_cast<std::int64_t>(input.text.size()) / width, width};
    auto* const sequenceData = input.sequence.empty() ? placeholder.data() : input.sequence.data();
    auto* const textData = input.text.empty() ? placeholder.data() : input.text.data();
    flowBinding->ClearBoundInputs();
    flowBinding->ClearBoundOutputs();
    flowBinding->BindOutput("conditioning", memory);
    flowBinding->BindOutput("eos_logit", memory);
    flowBinding->BindInput("sequence", Ort::Value::CreateTensor<float>(memory, sequenceData, input.sequence.size(),
                                                                       sequenceShape.data(), sequenceShape.size()));
    flowBinding->BindInput("text_embeddings", Ort::Value::CreateTensor<float>(memory, textData, input.text.size(),
                                                                              textShape.data(), textShape.size()));
    flowStates.bind(*flowBinding, memory);
    flowMain->Run(Ort::RunOptions{nullptr}, *flowBinding);
    auto outputs = flowBinding->GetOutputValues();
    flowStates.advance();
    MainOutput output;
    const auto conditioning = outputs.at(0).GetTensorTypeAndShapeInfo().GetElementCount();
    const auto* conditioningData = outputs.at(0).GetTensorData<float>();
    const auto rows = conditioning / static_cast<std::size_t>(bundle.conditioningDim);
    if (rows > 0) {
      const auto* last = conditioningData + ((rows - 1) * static_cast<std::size_t>(bundle.conditioningDim));
      output.conditioning.assign(last, last + bundle.conditioningDim);
    }
    if (outputs.at(1).GetTensorTypeAndShapeInfo().GetElementCount() > 0)
      output.eosLogit = *outputs.at(1).GetTensorData<float>();
    return output;
  }

  std::vector<float> condition(std::vector<std::int64_t> tokens)
  {
    const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(tokens.size())};
    auto input = Ort::Value::CreateTensor<std::int64_t>(memory, tokens.data(), tokens.size(), shape.data(), shape.size());
    const std::array<const char*, 1> inputNames{"token_ids"};
    const std::array<const char*, 1> outputNames{"text_embeddings"};
    auto outputs = textConditioner->Run(Ort::RunOptions{nullptr}, inputNames.data(), &input, 1, outputNames.data(), 1);
    const auto count = outputs.at(0).GetTensorTypeAndShapeInfo().GetElementCount();
    const auto* data = outputs.at(0).GetTensorData<float>();
    return {data, data + count};
  }

  struct FlowInput
  {
    std::span<float> conditioning;
    std::span<float> latent;
    PocketGeneration generation;
  };

  void sample(const FlowInput& input)
  {
    const auto deviation = std::sqrt(std::max(input.generation.temperature, 0.0F));
    if (deviation > 0.0F) {
      std::normal_distribution<float> noise(0.0F, deviation);
      for (auto& value : input.latent)
        value = noise(random);
    }
    else
      std::ranges::fill(input.latent, 0.0F);
    const auto steps = std::max(1, input.generation.lsdSteps);
    const std::array<std::int64_t, 2> conditionShape{1, static_cast<std::int64_t>(input.conditioning.size())};
    const std::array<std::int64_t, 2> timeShape{1, 1};
    const std::array<std::int64_t, 2> latentShape{1, static_cast<std::int64_t>(input.latent.size())};
    const std::array<const char*, 4> inputNames{"c", "s", "t", "x"};
    const std::array<const char*, 1> outputNames{"flow_dir"};
    for (int step = 0; step < steps; ++step) {
      auto start = static_cast<float>(step) / static_cast<float>(steps);
      auto target = static_cast<float>(step + 1) / static_cast<float>(steps);
      std::array<Ort::Value, 4> inputs{
          Ort::Value::CreateTensor<float>(memory, input.conditioning.data(), input.conditioning.size(),
                                          conditionShape.data(), conditionShape.size()),
          Ort::Value::CreateTensor<float>(memory, &start, 1, timeShape.data(), timeShape.size()),
          Ort::Value::CreateTensor<float>(memory, &target, 1, timeShape.data(), timeShape.size()),
          Ort::Value::CreateTensor<float>(memory, input.latent.data(), input.latent.size(), latentShape.data(),
                                          latentShape.size())};
      auto outputs = flowStep->Run(Ort::RunOptions{nullptr}, inputNames.data(), inputs.data(), inputs.size(),
                                   outputNames.data(), outputNames.size());
      const auto* direction = outputs.at(0).GetTensorData<float>();
      for (std::size_t index = 0; index < input.latent.size(); ++index)
        input.latent[index] += direction[index] / static_cast<float>(steps);
    }
  }

  struct DecodeInput
  {
    std::vector<float>& latents;
    bool fadeIn{false};
    const std::function<void(std::span<const float>)>& onAudio;
  };

  void decode(const DecodeInput& input)
  {
    if (input.latents.empty())
      return;
    const auto latent = static_cast<std::int64_t>(bundle.latentDim);
    const std::array<std::int64_t, 3> shape{1, static_cast<std::int64_t>(input.latents.size()) / latent, latent};
    mimiBinding->ClearBoundInputs();
    mimiBinding->ClearBoundOutputs();
    mimiBinding->BindOutput("audio", memory);
    mimiBinding->BindInput("latent", Ort::Value::CreateTensor<float>(memory, input.latents.data(), input.latents.size(),
                                                                     shape.data(), shape.size()));
    mimiStates.bind(*mimiBinding, memory);
    mimiDecoder->Run(Ort::RunOptions{nullptr}, *mimiBinding);
    auto outputs = mimiBinding->GetOutputValues();
    mimiStates.advance();
    input.latents.clear();
    const auto count = outputs.at(0).GetTensorTypeAndShapeInfo().GetElementCount();
    auto* audio = outputs.at(0).GetTensorMutableData<float>();
    const std::span<float> samples(audio, count);
    if (input.fadeIn) {
      const auto fade = static_cast<std::size_t>(kFadeSeconds * static_cast<double>(bundle.sampleRate));
      for (std::size_t index = 0; index < std::min(fade, samples.size()); ++index)
        samples[index] *= static_cast<float>(index) / static_cast<float>(std::max<std::size_t>(fade - 1, 1));
    }
    if (input.onAudio && !samples.empty())
      input.onAudio(samples);
  }

  struct DrainInput
  {
    LatentQueue& queue;
    std::atomic<bool>& halted;
    const PocketStreamInput& input;
  };

  void drain(const DrainInput& drainInput)
  {
    const auto latentDim = static_cast<std::size_t>(bundle.latentDim);
    const std::function<bool()> stopped = [&drainInput] {
      return drainInput.halted.load() || (drainInput.input.stopRequested && drainInput.input.stopRequested());
    };
    std::vector<float> batch;
    batch.reserve(latentDim * kMaxDecodeFrames);
    bool first = true;
    while (drainInput.queue.take({.batch = batch, .maxValues = latentDim * (first ? 1 : kMaxDecodeFrames), .stopped = stopped})) {
      if (stopped()) {
        drainInput.halted.store(true);
        return;
      }
      decode({.latents = batch, .fadeIn = first, .onAudio = drainInput.input.onAudio});
      first = false;
    }
    if (stopped())
      drainInput.halted.store(true);
  }

  PocketBundle bundle;
  UnigramTokenizer tokenizer;
  Ort::SessionOptions options;
  Ort::SessionOptions lightOptions;
  Ort::SessionOptions decoderOptions;
  Ort::MemoryInfo memory{Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
  std::unique_ptr<Ort::Session> textConditioner;
  std::unique_ptr<Ort::Session> flowMain;
  std::unique_ptr<Ort::Session> flowStep;
  std::unique_ptr<Ort::Session> mimiDecoder;
  std::unique_ptr<Ort::Session> voiceEncoder;
  std::unique_ptr<Ort::IoBinding> flowBinding;
  std::unique_ptr<Ort::IoBinding> mimiBinding;
  StateBank flowStates;
  StateBank mimiStates;
  std::array<float, 1> placeholder{};
  std::mt19937 random{std::random_device{}()};
};

PocketEngine::PocketEngine(const PocketEngineConfig& config) : impl_(std::make_unique<Impl>(config)) {}

PocketEngine::~PocketEngine() = default;

std::size_t PocketEngine::tokenCount(std::string_view text) const
{
  return impl_->tokenizer.encode(text).size();
}

int PocketEngine::sampleRate() const
{
  return impl_->bundle.sampleRate;
}

float PocketEngine::defaultTemperature() const
{
  return impl_->bundle.temperature;
}

bool PocketEngine::canClone() const
{
  return impl_->voiceEncoder != nullptr;
}

PocketVoice PocketEngine::loadVoice(const std::filesystem::path& path) const
{
  return loadPocketVoice({.path = path, .bundle = impl_->bundle});
}

PocketVoice PocketEngine::cloneVoice(std::span<const float> samples)
{
  auto& impl = *impl_;
  if (!impl.voiceEncoder)
    throw std::runtime_error("This Pocket model was exported without voice cloning");
  const auto rate = static_cast<double>(impl.bundle.sampleRate);
  const auto limit = std::min(samples.size(), static_cast<std::size_t>(kMaxReferenceSeconds * rate));
  auto audio = endOnPause(samples.first(limit), impl.bundle.sampleRate);
  const auto frame = static_cast<std::size_t>(impl.bundle.samplesPerFrame);
  audio.resize(((audio.size() + frame - 1) / frame) * frame, 0.0F);
  if (audio.empty())
    throw std::runtime_error("The reference recording is empty");
  const std::array<std::int64_t, 3> shape{1, 1, static_cast<std::int64_t>(audio.size())};
  auto input = Ort::Value::CreateTensor<float>(impl.memory, audio.data(), audio.size(), shape.data(), shape.size());
  const std::array<const char*, 1> inputNames{"audio"};
  const std::array<const char*, 1> outputNames{"prompt"};
  auto outputs = impl.voiceEncoder->Run(Ort::RunOptions{nullptr}, inputNames.data(), &input, 1, outputNames.data(), 1);
  const auto count = outputs.at(0).GetTensorTypeAndShapeInfo().GetElementCount();
  const auto* data = outputs.at(0).GetTensorData<float>();
  std::vector<float> prompt(data, data + count);
  const auto positions = static_cast<std::int64_t>(prompt.size() / static_cast<std::size_t>(impl.bundle.conditioningDim));
  if (positions + static_cast<std::int64_t>(kMaxChunkTokens) + kCloneGenerationRoom > impl.bundle.flowCapacity)
    throw std::runtime_error("The reference recording is too long for the Pocket context");
  const PocketVoice empty{.length = 0, .caches = {}};
  impl.flowStates.loadVoice({.voice = empty, .length = 0});
  static_cast<void>(impl.runMain({.sequence = {}, .text = prompt}));
  return impl.flowStates.extractVoice(positions);
}

void PocketEngine::stream(const PocketStreamInput& input)
{
  auto& impl = *impl_;
  const auto& bundle = impl.bundle;
  const auto prompt = preparePocketPrompt({.text = input.text, .bundle = bundle});
  if (prompt.text.empty())
    return;
  auto tokens = impl.tokenizer.encode(prompt.text);
  if (tokens.empty())
    return;
  const auto tokenTotal = static_cast<std::int64_t>(tokens.size());
  auto embeddings = impl.condition(std::move(tokens));
  if (input.generation.seed != 0)
    impl.random.seed(input.generation.seed);
  impl.flowStates.loadVoice({.voice = input.voice, .length = input.voice.length});
  impl.mimiStates.resetSmall();
  static_cast<void>(impl.runMain({.sequence = {}, .text = embeddings}));

  const auto frameRate = static_cast<double>(bundle.sampleRate) / static_cast<double>(bundle.samplesPerFrame);
  const auto estimate = static_cast<std::int64_t>(
      std::ceil(((static_cast<double>(tokenTotal) / bundle.tokensPerSecond) + bundle.genSecondsPadding) * frameRate));
  const auto flowRoom = static_cast<std::int64_t>(bundle.flowCapacity) - input.voice.length - tokenTotal;
  const auto mimiRoom = static_cast<std::int64_t>(bundle.mimiCapacity / bundle.mimiStepsPerLatent);
  const auto maxFrames = std::min({estimate, flowRoom, mimiRoom});
  if (maxFrames <= 0)
    throw std::runtime_error("The text chunk does not fit the Pocket context");
  if (input.stopRequested && input.stopRequested())
    return;

  const auto latentDim = static_cast<std::size_t>(bundle.latentDim);
  LatentQueue queue;
  std::atomic<bool> halted{false};
  std::exception_ptr failure;
  std::thread decoder([&] {
    try {
      impl.drain({.queue = queue, .halted = halted, .input = input});
    }
    catch (...) {
      failure = std::current_exception();
      halted.store(true);
    }
  });
  try {
    std::vector<float> latent(latentDim, std::numeric_limits<float>::quiet_NaN());
    std::optional<std::int64_t> eosStep;
    for (std::int64_t step = 0; step < maxFrames && !halted.load(); ++step) {
      auto output = impl.runMain({.sequence = latent, .text = {}});
      if (output.conditioning.empty())
        throw std::runtime_error("The Pocket backbone returned no conditioning");
      impl.sample({.conditioning = output.conditioning, .latent = latent, .generation = input.generation});
      if (!eosStep.has_value() && output.eosLogit > bundle.eosThreshold && step >= bundle.minFramesBeforeEos)
        eosStep = step;
      if (eosStep.has_value() && step >= *eosStep + prompt.framesAfterEos)
        break;
      queue.push(latent);
    }
  }
  catch (...) {
    halted.store(true);
    queue.finish();
    decoder.join();
    throw;
  }
  queue.finish();
  decoder.join();
  if (failure)
    std::rethrow_exception(failure);
}
