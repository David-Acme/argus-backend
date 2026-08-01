#include "stt-service.hxx"

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <sherpa-onnx/c-api/c-api.h>
#include <thread>

std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                void (*)(const SherpaOnnxOfflineRecognizer*)>
    SttService::recognizer_{nullptr, SherpaOnnxDestroyOfflineRecognizer};
bool SttService::loaded_ = false;
std::mutex SttService::mutex_;

namespace
{

std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                void (*)(const SherpaOnnxOfflineRecognizer*)>
createRecognizer(const std::string& lang)
{
  const std::string modelDir = "models/stt";
  auto nThreads = ThreadBudget::computeThreads();

  auto encoderPath = modelDir + "/tiny-encoder.int8.onnx";
  auto decoderPath = modelDir + "/tiny-decoder.int8.onnx";
  auto tokensPath = modelDir + "/tiny-tokens.txt";

  SherpaOnnxOfflineRecognizerConfig config{};
  config.model_config.whisper.encoder = encoderPath.c_str();
  config.model_config.whisper.decoder = decoderPath.c_str();
  config.model_config.whisper.language = lang.c_str();
  config.model_config.whisper.task = "transcribe";
  config.model_config.tokens = tokensPath.c_str();
  config.model_config.debug = 0;
  config.model_config.provider = "cpu";
  config.model_config.num_threads = nThreads;

  auto* raw = SherpaOnnxCreateOfflineRecognizer(&config);
  return {raw, SherpaOnnxDestroyOfflineRecognizer};
}

} // namespace

void SttService::init()
{
  try {
    const std::string lang =
        ConfigService::getString("stt.language").empty()
            ? "es"
            : ConfigService::getString("stt.language");
    recognizer_ = createRecognizer(lang);

    loaded_ = true;

    LOG_INFO << "STT loaded: models/stt"
             << " (whisper-tiny, " << lang << ", threads="
             << ThreadBudget::computeThreads() << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "STT init failed: " << e.what();
    shutdown();
  }
}

bool SttService::setLanguage(const std::string& lang)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (lang != "es" && lang != "en" && lang != "auto")
    return false;
  auto recognizer = createRecognizer(lang);
  if (!recognizer)
    return false;
  recognizer_ = std::move(recognizer);
  LOG_INFO << "STT language set to " << lang;
  return true;
}

void SttService::shutdown()
{
  recognizer_.reset();
  loaded_ = false;

  LOG_INFO << "STT shutdown";
}

bool SttService::isLoaded()
{
  return loaded_;
}

std::string SttService::transcribe(const std::vector<float>& audioSamples,
                                   int32_t sampleRate)
{
  std::lock_guard<std::mutex> lock(mutex_);

  auto* recognizer = recognizer_.get();

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
                            int32_t sampleRate)
{
  co_return co_await BlockingTask<std::string>(
      [audioSamples, sampleRate]() {
        return SttService::transcribe(audioSamples, sampleRate);
      });
}
