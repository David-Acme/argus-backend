#include "stt-service.hxx"

#include <drogon/drogon.h>
#include <sherpa-onnx/c-api/c-api.h>
#include <thread>

std::unique_ptr<const SherpaOnnxOfflineRecognizer,
                void (*)(const SherpaOnnxOfflineRecognizer*)>
    SttService::recognizer_{nullptr, SherpaOnnxDestroyOfflineRecognizer};
bool SttService::loaded_ = false;
void SttService::init()
{
  try {
    const std::string modelDir = "models/stt";
    auto nThreads = static_cast<int32_t>(4);

    auto encoderPath = modelDir + "/tiny-encoder.int8.onnx";
    auto decoderPath = modelDir + "/tiny-decoder.int8.onnx";
    auto tokensPath = modelDir + "/tiny-tokens.txt";

    SherpaOnnxOfflineRecognizerConfig config{};
    config.model_config.whisper.encoder = encoderPath.c_str();
    config.model_config.whisper.decoder = decoderPath.c_str();
    config.model_config.whisper.language = "es";
    config.model_config.whisper.task = "transcribe";
    config.model_config.tokens = tokensPath.c_str();
    config.model_config.debug = 0;
    config.model_config.provider = "cpu";
    config.model_config.num_threads = nThreads;

    auto* raw = SherpaOnnxCreateOfflineRecognizer(&config);
    if (!raw) {
      throw std::runtime_error("failed to create recognizer from: " + modelDir);
    }
    recognizer_.reset(raw);

    loaded_ = true;

    LOG_INFO << "STT loaded: " << modelDir
             << " (whisper-tiny, es, threads=" << nThreads << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "STT init failed: " << e.what();
    shutdown();
  }
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
