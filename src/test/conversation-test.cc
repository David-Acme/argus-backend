#include "conversation-test.hxx"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <drogon/drogon.h>
#include <iostream>
#include <memory>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/services/vision/vision-service.hxx>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int kSampleRate = 16000;
constexpr int kRecordSeconds = 5;

constexpr const char* kSystemPrompt =
    "Eres Argus, un sistema de seguridad inteligente para el hogar. "
    "Estas en fase de pruebas. Responde siempre en espanol, de forma "
    "clara y profesional. Eres un asistente de seguridad que monitorea "
    "camaras y alerta sobre riesgos. Si te preguntan algo fuera de "
    "ese ambito, indica amablemente que solo ayudas con seguridad.";

int selectedMicCard = -1;
int selectedSpeakerCard = -1;

std::vector<int> scanAlsaCards()
{
  std::vector<int> cards;
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen("arecord -l 2>/dev/null",
                                                   "r"),
                                             pclose);
  if (!pipe)
    return cards;

  char buf[256];
  std::string output;
  while (fgets(buf, sizeof(buf), pipe.get()))
    output += buf;

  size_t pos = 0;
  while ((pos = output.find("card ", pos)) != std::string::npos) {
    size_t colon = output.find(':', pos);
    if (colon != std::string::npos) {
      std::string numStr = output.substr(pos + 5, colon - pos - 5);
      int card = std::stoi(numStr);
      cards.push_back(card);
    }
    pos = colon;
  }
  return cards;
}

std::string cardName(int card)
{
  std::string cmd = "arecord -l 2>/dev/null | grep 'card " +
                    std::to_string(card) + ":' | head -1";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe)
    return "Unknown";

  char buf[256];
  std::string result;
  while (fgets(buf, sizeof(buf), pipe.get()))
    result += buf;

  size_t bracket = result.find('[');
  if (bracket != std::string::npos) {
    size_t end = result.find(']', bracket);
    if (end != std::string::npos)
      return result.substr(bracket + 1, end - bracket - 1);
  }
  return "Card " + std::to_string(card);
}

bool testMic(int card)
{
  std::string testFile = "/tmp/argus-mic-test.raw";
  std::string cmd = "timeout 2 arecord -D hw:" + std::to_string(card) +
                    ",0 -f S16_LE -r " + std::to_string(kSampleRate) +
                    " -c 1 -t raw " + testFile + " 2>/dev/null";
  system(cmd.c_str());

  std::unique_ptr<FILE, int (*)(FILE*)> f(fopen(testFile.c_str(), "rb"),
                                          fclose);
  if (!f)
    return false;

  fseek(f.get(), 0, SEEK_END);
  long sz = ftell(f.get());
  return sz > 1000;
}

int selectMic()
{
  auto cards = scanAlsaCards();

  if (cards.empty()) {
    LOG_WARN << "No audio capture devices found.";
    return -1;
  }

  std::cout << "\n\033[1;36mAvailable microphones:\033[0m\n";
  for (size_t i = 0; i < cards.size(); ++i) {
    std::cout << "  [" << (i + 1) << "] hw:" << cards[i] << ",0 - "
              << cardName(cards[i]) << "\n";
  }

  std::cout << "\nSelect microphone [1-" << cards.size() << "]: ";
  std::string line;
  std::getline(std::cin, line);

  int choice = 0;
  try {
    choice = std::stoi(line);
  }
  catch (...) {
    choice = 0;
  }

  if (choice < 1 || choice > static_cast<int>(cards.size())) {
    std::cout << "Invalid selection, using first device.\n";
    choice = 1;
  }

  int selected = cards[choice - 1];
  std::cout << "Selected: hw:" << selected << ",0 - " << cardName(selected)
            << "\n";

  std::cout << "Testing mic... " << std::flush;
  if (testMic(selected)) {
    std::cout << "OK\n\n";
  }
  else {
    std::cout << "FAIL (no audio captured)\n\n";
  }

  return selected;
}

std::vector<float> s16ToFloat(const std::vector<int16_t>& s16)
{
  std::vector<float> f32(s16.size());
  for (size_t i = 0; i < s16.size(); ++i) {
    f32[i] = static_cast<float>(s16[i]) / 32768.0f;
  }
  return f32;
}

std::vector<int16_t> floatToS16(const std::vector<float>& f32)
{
  std::vector<int16_t> s16(f32.size());
  for (size_t i = 0; i < f32.size(); ++i) {
    auto v = std::round(f32[i] * 32767.0f);
    v = std::max(-32768.0f, std::min(32767.0f, v));
    s16[i] = static_cast<int16_t>(v);
  }
  return s16;
}

std::vector<int16_t> captureMic()
{
  std::string cmd = "arecord -D hw:" + std::to_string(selectedMicCard) +
                    ",0 -f S16_LE -r " + std::to_string(kSampleRate) +
                    " -c 1 -t raw -d " + std::to_string(kRecordSeconds) +
                    " 2>/dev/null";

  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) {
    LOG_ERROR << "[test] Failed to open mic capture";
    return {};
  }

  std::vector<int16_t> samples;
  int16_t buf[1024];
  while (!feof(pipe.get())) {
    size_t n = fread(buf, sizeof(int16_t), 1024, pipe.get());
    if (n == 0)
      break;
    samples.insert(samples.end(), buf, buf + n);
  }

  LOG_INFO << "[test] Captured " << samples.size() << " samples (~"
           << (samples.size() / kSampleRate) << "s)";
  return samples;
}

void playAudio(const std::vector<float>& pcm, int sampleRate)
{
  if (selectedSpeakerCard < 0)
    return;

  auto s16 = floatToS16(pcm);

  std::string cmd = "aplay -D plughw:" + std::to_string(selectedSpeakerCard) +
                    ",0 -f S16_LE -r " + std::to_string(sampleRate) +
                    " -c 1 -t raw 2>/dev/null";

  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "w"), pclose);
  if (!pipe) {
    LOG_ERROR << "[test] Failed to open audio playback";
    return;
  }

  fwrite(s16.data(), sizeof(int16_t), s16.size(), pipe.get());
}

} // namespace

namespace argus::test
{

void runConversationTest()
{
  int micCard = selectMic();
  if (micCard < 0) {
    LOG_ERROR << "No microphone available. Aborting.";
    return;
  }
  selectedMicCard = micCard;
  selectedSpeakerCard = micCard;

  LOG_INFO << "Loading services...";
  TtsService::init();
  LlmService::init();
  SttService::init();
  VisionService::init();

  if (!TtsService::isLoaded() || !LlmService::isLoaded() ||
      !SttService::isLoaded()) {
    LOG_FATAL << "Essential services failed to load. Aborting.";
    return;
  }

  LOG_INFO << "========== ARGUS - Conversation Test ==========";
  LOG_INFO << "System: Security assistant (ES)";
  LOG_INFO << "Mic: " << cardName(selectedMicCard) << " (hw:" << selectedMicCard
           << ")";
  LOG_INFO << "Press ENTER to speak, type 'q' + ENTER to quit.";
  LOG_INFO << "===============================================";

  std::vector<ChatMessage> history;
  bool running = true;

  while (running) {
    std::cout
        << "\n\033[1;32m[ARGUS]\033[0m Press ENTER to speak (q to quit): ";
    std::cout.flush();

    std::string line;
    std::getline(std::cin, line);
    if (line == "q" || line == "Q") {
      running = false;
      break;
    }

    std::cout << "\033[1;33m[REC]\033[0m Recording " << kRecordSeconds
              << "s... speak now" << std::endl;
    auto rawSamples = captureMic();

    if (rawSamples.empty()) {
      std::cout << "\033[1;31m[ERR]\033[0m No audio captured.\n";
      continue;
    }

    auto audioF32 = s16ToFloat(rawSamples);

    auto startStt = std::chrono::steady_clock::now();
    auto transcription = SttService::transcribe(audioF32, kSampleRate);
    auto elapsedStt = std::chrono::steady_clock::now() - startStt;

    if (transcription.empty()) {
      std::cout << "\033[1;31m[STT]\033[0m No speech detected\n";
      continue;
    }

    std::cout << "\033[1;36m[STT]\033[0m " << transcription << " ("
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     elapsedStt)
                     .count()
              << "ms)" << std::endl;

    std::cout << "\033[1;34m[LLM]\033[0m " << std::flush;
    auto startLlm = std::chrono::steady_clock::now();

    ChatRequest req;
    req.messages.push_back({"system",
                            std::string(kSystemPrompt)});
    for (size_t i = 0; i < history.size() && i < 8; ++i) {
      req.messages.push_back(history[i]);
    }
    req.messages.push_back({"user", transcription});
    req.maxTokens = 1024;

    std::string fullResponse;

    LlmService::chatStream(req,
                           [&](const std::string& token, bool done) {
                             if (!token.empty()) {
                               std::cout << token << std::flush;
                               fullResponse.append(token);
                             }
                           });

    auto elapsedLlm = std::chrono::steady_clock::now() - startLlm;

    if (fullResponse.empty()) {
      fullResponse = "Lo siento, no pude procesar tu solicitud.";
      std::cout << fullResponse << std::endl;
    }

    std::cout << " ("
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     elapsedLlm)
                     .count()
              << "ms)" << std::endl;

    history.push_back({"user", transcription});
    history.push_back({"assistant", fullResponse});
    if (history.size() > 8) {
      history.erase(history.begin(), history.begin() + 2);
    }

    std::cout << "\033[1;35m[TTS]\033[0m " << std::flush;
    auto startTts = std::chrono::steady_clock::now();
    TtsRequest ttsReq;
    ttsReq.text = fullResponse;
    ttsReq.lang = TtsLang::ES;
    ttsReq.voiceId = "M3";
    ttsReq.speed = 1.05f;

    std::vector<float> allAudio;
    int ttsChunks = 0;
    TtsService::synthesizeStream(
        ttsReq,
        [&](const std::vector<float>& chunkPcm) {
          ttsChunks++;
          std::cout << "." << std::flush;
          allAudio.insert(allAudio.end(), chunkPcm.begin(), chunkPcm.end());
        });
    auto elapsedTts = std::chrono::steady_clock::now() - startTts;

    std::cout << "\r\033[1;35m[TTS]\033[0m Done " << ttsChunks
              << " chunks ("
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     elapsedTts)
                     .count()
              << "ms)" << std::endl;

    if (!allAudio.empty()) {
      playAudio(allAudio, TtsService::sampleRate());
    }
    std::cout
        << "\033[90m[PERF]\033[0m STT="
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsedStt)
               .count()
        << "ms LLM="
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsedLlm)
               .count()
        << "ms TTS="
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsedTts)
               .count()
        << "ms" << std::endl;
  }

  LOG_INFO << "Shutting down services...";
  VisionService::shutdown();
  SttService::shutdown();
  LlmService::shutdown();
  TtsService::shutdown();
  LOG_INFO << "Goodbye.";
}

} // namespace argus::test
