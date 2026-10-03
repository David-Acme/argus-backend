#include "stt-service.hxx"

#include <drogon/drogon.h>
#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/thread-budget.hxx>
#include <sherpa-onnx/c-api/c-api.h>
#include <stt/stt-errors.hxx>
#include <algorithm>
#include <thread>
#include <utility>

namespace
{

std::string modelsDir()
{
  const std::string dir = ConfigService::getString("stt.models_dir");
  return dir.empty() ? std::string("models/stt") : dir;
}

std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                void (*)(const SherpaOnnxOfflineRecognizer*)>
createRecognizer(SttEngine engine, const std::string& lang)
{
  const std::string modelDir = modelsDir();
  auto nThreads = ThreadBudget::computeThreads();

  SherpaOnnxOfflineRecognizerConfig config{};

  std::string encPath, decPath, tokPath, modelPath, langPath;

  if (engine == SttEngine::Canary) {
    encPath = modelDir + "/canary-encoder.int8.onnx";
    decPath = modelDir + "/canary-decoder.int8.onnx";
    tokPath = modelDir + "/canary-tokens.txt";
    langPath = lang;
    config.model_config.canary.encoder = encPath.c_str();
    config.model_config.canary.decoder = decPath.c_str();
    config.model_config.canary.src_lang = langPath.c_str();
    config.model_config.canary.tgt_lang = langPath.c_str();
    config.model_config.canary.use_pnc = 1;
    config.model_config.tokens = tokPath.c_str();
  }
  else if (engine == SttEngine::NemoCtc) {
    modelPath = modelDir + "/nemo-ctc-model.int8.onnx";
    tokPath = modelDir + "/nemo-ctc-tokens.txt";
    config.model_config.nemo_ctc.model = modelPath.c_str();
    config.model_config.tokens = tokPath.c_str();
  }
  else if (engine == SttEngine::Omnilingual) {
    modelPath = modelDir + "/omnilingual-model.int8.onnx";
    tokPath = modelDir + "/omnilingual-tokens.txt";
    config.model_config.omnilingual.model = modelPath.c_str();
    config.model_config.tokens = tokPath.c_str();
  }
  else if (engine == SttEngine::NemoTransducer) {
    encPath = modelDir + "/nemo-transducer-encoder.int8.onnx";
    decPath = modelDir + "/nemo-transducer-decoder.int8.onnx";
    modelPath = modelDir + "/nemo-transducer-joiner.int8.onnx";
    tokPath = modelDir + "/nemo-transducer-tokens.txt";
    config.model_config.transducer.encoder = encPath.c_str();
    config.model_config.transducer.decoder = decPath.c_str();
    config.model_config.transducer.joiner = modelPath.c_str();
    config.model_config.model_type = "nemo_transducer";
    config.model_config.tokens = tokPath.c_str();
  }
  else {
    encPath = modelDir + "/tiny-encoder.int8.onnx";
    decPath = modelDir + "/tiny-decoder.int8.onnx";
    tokPath = modelDir + "/tiny-tokens.txt";
    langPath = lang == "auto" ? std::string() : lang;
    config.model_config.whisper.encoder = encPath.c_str();
    config.model_config.whisper.decoder = decPath.c_str();
    config.model_config.whisper.language = langPath.c_str();
    config.model_config.whisper.task = "transcribe";
    config.model_config.tokens = tokPath.c_str();
  }

  config.model_config.debug = 0;
  config.model_config.provider = "cpu";
  config.model_config.num_threads = nThreads;

  auto* raw = SherpaOnnxCreateOfflineRecognizer(&config);
  return {raw, SherpaOnnxDestroyOfflineRecognizer};
}

}

SttService::SttService()
    : recognizer_(nullptr, SherpaOnnxDestroyOfflineRecognizer)
{
}

SttService::~SttService()
{
  shutdown();
}

SttService& SttService::instance()
{
  static SttService service;
  return service;
}

std::string SttService::configLanguage()
{
  const std::string lang = ConfigService::getString("stt.language");
  return lang.empty() ? std::string("es") : lang;
}

SttEngine SttService::configEngine()
{
  const std::string e = ConfigService::getString("stt.engine");
  if (e == "whisper")
    return SttEngine::Whisper;
  if (e == "canary")
    return SttEngine::Canary;
  if (e == "nemo_ctc" || e == "nemo-ctc" || e == "fastconformer")
    return SttEngine::NemoCtc;
  if (e == "omnilingual")
    return SttEngine::Omnilingual;
  return SttEngine::NemoTransducer;
}

std::string_view SttService::engineName(SttEngine engine)
{
  switch (engine) {
    case SttEngine::Whisper:
      return "whisper";
    case SttEngine::Canary:
      return "canary";
    case SttEngine::NemoCtc:
      return "nemo_ctc";
    case SttEngine::NemoTransducer:
      return "nemo_transducer";
    case SttEngine::Omnilingual:
      return "omnilingual";
  }
  return "nemo_transducer";
}

bool SttService::languageBound(SttEngine engine)
{
  return engine == SttEngine::Whisper || engine == SttEngine::Canary;
}

bool SttService::isSupportedLanguage(const std::string& lang)
{
  const auto& languages = supportedLanguages();
  return std::ranges::find(languages, lang) != languages.end();
}

const std::vector<std::string>& SttService::supportedLanguages()
{
  static const std::vector<std::string> languages{"es", "en", "auto"};
  return languages;
}

void SttService::init()
{
  try {
    const std::string lang = configLanguage();
    engine_ = configEngine();
    recognizer_ = createRecognizer(engine_, lang);
    if (!recognizer_) {
      LOG_FATAL << "STT init failed: recognizer creation returned null";
      shutdown();
      return;
    }
    currentLang_ = lang;
    loaded_ = true;

    LOG_INFO << "STT loaded: " << modelsDir()
             << " (" << engineName(engine_) << ", " << lang
             << ", threads=" << ThreadBudget::computeThreads() << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "STT init failed: " << e.what();
    shutdown();
  }
}

bool SttService::setLanguage(const std::string& lang)
{
  std::scoped_lock lock(mutex_);
  if (!isSupportedLanguage(lang))
    return false;
  if (!languageBound(engine_)) {
    currentLang_ = lang;
    return true;
  }
  auto recognizer = createRecognizer(engine_, lang);
  if (!recognizer)
    return false;
  recognizer_ = std::move(recognizer);
  currentLang_ = lang;
  LOG_INFO << "STT language set to " << lang;
  return true;
}

std::string SttService::language() const
{
  std::scoped_lock lock(mutex_);
  return currentLang_;
}

void SttService::shutdown()
{
  std::scoped_lock lock(mutex_);
  recognizer_.reset();
  loaded_ = false;

  LOG_INFO << "STT shutdown";
}

bool SttService::isLoaded() const
{
  return loaded_;
}

std::string SttService::transcribe(const TranscribeRequest& request)
{
  const std::string effective =
      request.lang.empty() ? configLanguage() : request.lang;
  if (effective != language() && !setLanguage(effective))
    LOG_WARN << "STT language switch to " << effective
             << " failed; transcribing with the current recognizer";
  return decode(request.samples, request.sampleRate);
}

std::string SttService::decode(const std::vector<float>& samples,
                               int32_t sampleRate)
{
  std::scoped_lock lock(mutex_);

  auto* recognizer = recognizer_.get();
  if (!recognizer)
    throw ResponseException(SttErrors::SpeechEngineNotLoaded);

  std::unique_ptr<const SherpaOnnxOfflineStream,
                  void (*)(const SherpaOnnxOfflineStream*)>
      stream{SherpaOnnxCreateOfflineStream(recognizer),
             SherpaOnnxDestroyOfflineStream};

  if (!stream)
    throw ResponseException(SttErrors::InternalError);

  SherpaOnnxAcceptWaveformOffline(stream.get(), sampleRate, samples.data(),
                                  static_cast<int32_t>(samples.size()));

  SherpaOnnxDecodeOfflineStream(recognizer, stream.get());

  std::string result;
  auto* r = SherpaOnnxGetOfflineStreamResult(stream.get());
  if (r && r->text) {
    result = r->text;
  }

  SherpaOnnxDestroyOfflineRecognizerResult(r);

  return result;
}

drogon::Task<std::string> SttService::transcribeAsync(TranscribeRequest request)
{
  co_return co_await BlockingTask<std::string>(
      [this, request = std::move(request)]() { return transcribe(request); });
}
