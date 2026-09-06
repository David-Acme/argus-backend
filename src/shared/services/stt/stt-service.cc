#include "stt-service.hxx"

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <sherpa-onnx/c-api/c-api.h>
#include <thread>

namespace
{

// Root of the on-disk STT models (models/stt by default); argus-stt points
// stt.models_dir at the read-only models volume.
std::string modelsDir()
{
  const std::string dir = ConfigService::getString("stt.models_dir");
  return dir.empty() ? std::string("models/stt") : dir;
}

SttEngine resolveEngine()
{
  const std::string e = ConfigService::getString("stt.engine");
  if (e == "canary")
    return SttEngine::Canary;
  if (e == "nemo_ctc" || e == "nemo-ctc" || e == "fastconformer")
    return SttEngine::NemoCtc;
  if (e == "nemo_transducer" || e == "nemo-transducer" ||
      e == "fastconformer_transducer")
    return SttEngine::NemoTransducer;
  if (e == "omnilingual")
    return SttEngine::Omnilingual;
  return SttEngine::Whisper;
}

std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                void (*)(const SherpaOnnxOfflineRecognizer*)>
createRecognizer(const std::string& lang)
{
  const std::string modelDir = modelsDir();
  auto nThreads = ThreadBudget::computeThreads();
  const SttEngine engine = resolveEngine();

  SherpaOnnxOfflineRecognizerConfig config{};

  // Keep the resolved paths alive for the duration of this function: the
  // recognizer copies the string contents during create, but a temporary
  // std::string + c_str() would dangle before then.
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
  else { // Whisper (default)
    encPath = modelDir + "/tiny-encoder.int8.onnx";
    decPath = modelDir + "/tiny-decoder.int8.onnx";
    tokPath = modelDir + "/tiny-tokens.txt";
    langPath = lang;
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

} // namespace

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

bool SttService::isSupportedLanguage(const std::string& lang)
{
  return lang == "es" || lang == "en" || lang == "auto";
}

void SttService::init()
{
  try {
    const std::string lang = configLanguage();
    recognizer_ = createRecognizer(lang);
    if (!recognizer_) {
      LOG_FATAL << "STT init failed: recognizer creation returned null";
      shutdown();
      return;
    }
    currentLang_ = lang;
    loaded_ = true;

    std::string engineName = "whisper";
    switch (resolveEngine()) {
      case SttEngine::Canary:
        engineName = "canary";
        break;
      case SttEngine::NemoCtc:
        engineName = "nemo_ctc";
        break;
      case SttEngine::NemoTransducer:
        engineName = "nemo_transducer";
        break;
      case SttEngine::Omnilingual:
        engineName = "omnilingual";
        break;
      case SttEngine::Whisper:
        engineName = "whisper";
        break;
    }

    LOG_INFO << "STT loaded: " << modelsDir()
             << " (" << engineName << ", " << lang
             << ", threads=" << ThreadBudget::computeThreads() << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "STT init failed: " << e.what();
    shutdown();
  }
}

bool SttService::setLanguage(const std::string& lang)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!isSupportedLanguage(lang))
    return false;
  auto recognizer = createRecognizer(lang);
  if (!recognizer)
    return false;
  recognizer_ = std::move(recognizer);
  currentLang_ = lang;
  LOG_INFO << "STT language set to " << lang;
  return true;
}

std::string SttService::language() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return currentLang_;
}

void SttService::shutdown()
{
  std::lock_guard<std::mutex> lock(mutex_);
  recognizer_.reset();
  loaded_ = false;

  LOG_INFO << "STT shutdown";
}

bool SttService::isLoaded() const
{
  return loaded_;
}

std::string SttService::transcribe(const std::vector<float>& audioSamples,
                                   int32_t sampleRate)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto* recognizer = recognizer_.get();
  if (!recognizer) {
    LOG_WARN << "STT: no recognizer loaded";
    return "";
  }

  std::unique_ptr<const SherpaOnnxOfflineStream,
                  void (*)(const SherpaOnnxOfflineStream*)>
      stream{SherpaOnnxCreateOfflineStream(recognizer),
             SherpaOnnxDestroyOfflineStream};

  if (!stream) {
    LOG_WARN << "STT: failed to create stream";
    return "";
  }

  SherpaOnnxAcceptWaveformOffline(stream.get(), sampleRate, audioSamples.data(),
                                  static_cast<int32_t>(audioSamples.size()));

  SherpaOnnxDecodeOfflineStream(recognizer, stream.get());

  std::string result;
  auto* r = SherpaOnnxGetOfflineStreamResult(stream.get());
  if (r && r->text) {
    result = r->text;
  }

  SherpaOnnxDestroyOfflineRecognizerResult(r);

  return result;
}

drogon::Task<std::string>
SttService::transcribeAsync(const std::vector<float>& audioSamples,
                            int32_t sampleRate, const std::string& lang)
{
  co_return co_await BlockingTask<std::string>(
      [this, audioSamples, sampleRate, lang]() {
        // Ruling BE: one global recognizer, rebuilt inside the blocking leg
        // only when the effective language differs. A failed rebuild falls
        // back to the current recognizer, like the voice session does.
        const std::string effective = lang.empty() ? configLanguage() : lang;
        if (effective != language() && !setLanguage(effective))
          LOG_WARN << "STT language switch to " << effective
                   << " failed; transcribing with the current recognizer";
        return transcribe(audioSamples, sampleRate);
      });
}
