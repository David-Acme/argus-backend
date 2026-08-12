#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/tapo/tapo-api.hxx>
#include <shared/services/tapo/tapo-audio.hxx>
#include <shared/services/tapo/tapo-talk-client.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <string>
#include <vector>

namespace
{

TtsService gTts;

struct ProbeOptions
{
  std::string host;
  std::string cameraUser;
  std::string cameraPassword;
  std::string cloudUser{"admin"};
  std::string cloudPassword;
  std::string transport{"auto"};
  std::string rawPayload;
  std::string talkText;
  std::string talkFile;
  std::string talkFraming{"none"};
  std::string talkMode{"aec"};
  std::string dayNightMode;
  std::string tsDumpPath;
  int talkPacketMs{20};
  int controlPort{443};
  int mediaPort{8800};
  int eventHours{24};
  int64_t ptzX{0};
  int64_t ptzY{0};
  int64_t ptzStep{0};
  bool doInfo{false};
  bool doCreds{false};
  bool doBatch{false};
  bool doPtz{false};
  bool doPtzStep{false};
  bool doEvents{false};
  bool doTalk{false};
  bool doAudioConfig{false};
  bool doPresets{false};
  bool doOffline{false};
  bool verbose{false};
};

void printUsage()
{
  std::cout
      << "argus-tapo-probe — Phase 1 validation against a real Tapo camera\n\n"
      << "Usage: argus-tapo-probe --host <ip> [credentials] [actions]\n\n"
      << "Credentials\n"
      << "  --user <name>          camera account user (Tapo app > Advanced)\n"
      << "  --pass <secret>        camera account password\n"
      << "  --cloud-user <name>    control/talk fallback user (default: "
         "admin)\n"
      << "  --cloud-pass <secret>  TP-Link cloud password (required for "
         "--talk)\n"
      << "  --control-port <n>     default 443\n"
      << "  --media-port <n>       default 8800\n"
      << "  --transport <mode>     auto | secure_passthrough | legacy_stok\n\n"
      << "Actions\n"
      << "  --info                 get_device_info + resolved transport\n"
      << "  --creds                report which credential/transport pair "
         "works\n"
      << "  --batch                grouped status via multipleRequest\n"
      << "  --presets              list PTZ presets\n"
      << "  --audio                microphone/speaker config (encode_type, "
         "rate)\n"
      << "  --ptz <x,y>            absolute motor move\n"
      << "  --ptz-step <angle>     relative motor step\n"
      << "  --day-night <mode>     auto | on | off\n"
      << "  --events [hours]       searchDetectionList over the last N hours\n"
      << "  --raw '<json>'         send a raw request payload\n"
      << "  --talk \"text\"          synthesize with TTS and play on the "
         "camera\n"
      << "  --talk-file <wav>      play a 16-bit PCM WAV on the camera\n"
      << "  --talk-framing <mode>  none | negative | chunked (default none)\n"
      << "  --ts-dump <file.ts>    write 1s of A-law tone as MPEG-TS (offline\n"
      << "                         muxer check, needs no camera)\n"
      << "  --offline              run checks that need no camera and exit\n"
      << "  --verbose              dump full JSON responses\n";
}

bool parseArgs(int argc, char** argv, ProbeOptions& options)
{
  const auto next = [&](int& index) -> std::string {
    if (index + 1 >= argc)
      return {};
    return argv[++index];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--help" || flag == "-h")
      return false;
    else if (flag == "--host")
      options.host = next(i);
    else if (flag == "--user")
      options.cameraUser = next(i);
    else if (flag == "--pass")
      options.cameraPassword = next(i);
    else if (flag == "--cloud-user")
      options.cloudUser = next(i);
    else if (flag == "--cloud-pass")
      options.cloudPassword = next(i);
    else if (flag == "--control-port")
      options.controlPort = std::atoi(next(i).c_str());
    else if (flag == "--media-port")
      options.mediaPort = std::atoi(next(i).c_str());
    else if (flag == "--transport")
      options.transport = next(i);
    else if (flag == "--info")
      options.doInfo = true;
    else if (flag == "--creds")
      options.doCreds = true;
    else if (flag == "--batch")
      options.doBatch = true;
    else if (flag == "--presets")
      options.doPresets = true;
    else if (flag == "--audio")
      options.doAudioConfig = true;
    else if (flag == "--ptz") {
      const std::string value = next(i);
      const size_t comma = value.find(',');
      options.ptzX = std::atoll(value.substr(0, comma).c_str());
      options.ptzY = comma == std::string::npos
                         ? 0
                         : std::atoll(value.substr(comma + 1).c_str());
      options.doPtz = true;
    }
    else if (flag == "--ptz-step") {
      options.ptzStep = std::atoll(next(i).c_str());
      options.doPtzStep = true;
    }
    else if (flag == "--day-night")
      options.dayNightMode = next(i);
    else if (flag == "--events") {
      options.doEvents = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        options.eventHours = std::atoi(next(i).c_str());
    }
    else if (flag == "--raw")
      options.rawPayload = next(i);
    else if (flag == "--talk") {
      options.talkText = next(i);
      options.doTalk = true;
    }
    else if (flag == "--talk-file") {
      options.talkFile = next(i);
      options.doTalk = true;
    }
    else if (flag == "--talk-framing")
      options.talkFraming = next(i);
    else if (flag == "--ts-dump")
      options.tsDumpPath = next(i);
    else if (flag == "--offline")
      options.doOffline = true;
    else if (flag == "--verbose")
      options.verbose = true;
    else {
      std::cerr << "unknown flag: " << flag << "\n";
      return false;
    }
  }
  return !options.host.empty() || !options.tsDumpPath.empty() ||
         options.doOffline;
}

void loadDefaults(ProbeOptions& options)
{
  ConfigService::load("config.toml");
  const int controlPort = ConfigService::getInt("tapo.control_port");
  const int mediaPort = ConfigService::getInt("tapo.media_port");
  const std::string transport = ConfigService::getString("tapo.transport");
  if (controlPort > 0 && options.controlPort == 443)
    options.controlPort = controlPort;
  if (mediaPort > 0 && options.mediaPort == 8800)
    options.mediaPort = mediaPort;
  if (!transport.empty() && options.transport == "auto")
    options.transport = transport;

  const std::string framing = ConfigService::getString("tapo.talk_framing");
  if (!framing.empty() && options.talkFraming == "none")
    options.talkFraming = framing;
  const std::string mode = ConfigService::getString("tapo.talk_mode");
  if (!mode.empty())
    options.talkMode = mode;
  const int packetMs = ConfigService::getInt("tapo.talk_packet_ms");
  if (packetMs > 0)
    options.talkPacketMs = packetMs;
}

std::vector<TapoCredentialCandidate> candidatesOf(const ProbeOptions& options)
{
  std::vector<TapoCredentialCandidate> candidates;
  if (!options.cameraUser.empty() && !options.cameraPassword.empty())
    candidates.push_back({.label = "camera_account",
                          .username = options.cameraUser,
                          .password = options.cameraPassword});
  if (!options.cloudPassword.empty())
    candidates.push_back({.label = "cloud_admin",
                          .username = options.cloudUser,
                          .password = options.cloudPassword});
  return candidates;
}

void report(const std::string& title, const TapoResult& result, bool verbose)
{
  std::cout << "\n── " << title << " ──\n";
  if (!result.ok) {
    std::cout << "  FAIL  " << result.error;
    if (result.errorCode != 0)
      std::cout << " (error_code " << result.errorCode << ")";
    std::cout << "\n";
    if (verbose && !result.data.isNull())
      std::cout << result.data.toStyledString() << "\n";
    return;
  }
  std::cout << "  OK\n";
  if (verbose || result.data.isNull())
    std::cout << result.data.toStyledString() << "\n";
  else
    std::cout << json_util::toString(result.data) << "\n";
}

std::vector<int16_t> toPcm16(const std::vector<float>& pcm)
{
  std::vector<int16_t> out;
  out.reserve(pcm.size());
  for (const float sample : pcm) {
    const float clamped =
        sample < -1.0f ? -1.0f : (sample > 1.0f ? 1.0f : sample);
    out.push_back(static_cast<int16_t>(clamped * 32767.0f));
  }
  return out;
}

int runTalk(const ProbeOptions& options)
{
  if (options.cloudPassword.empty()) {
    std::cerr << "--talk requires --cloud-pass (the TP-Link cloud password)\n";
    return 1;
  }

  TapoTalkAudio audio;
  if (!options.talkFile.empty()) {
    const auto wav = tapo_audio::readWav(options.talkFile);
    if (!wav.ok) {
      std::cerr << "cannot read " << options.talkFile << ": " << wav.error
                << "\n";
      return 1;
    }
    audio.samples = tapo_audio::downmixToMono(wav.samples, wav.channels);
    audio.sampleRate = wav.sampleRate;
  }
  else {
    std::cout << "synthesizing with TTS...\n";
    gTts.init();
    if (!gTts.isLoaded()) {
      std::cerr << "TTS service could not be initialized\n";
      return 1;
    }
    const auto pcm = gTts.synthesize({.text = options.talkText,
                                      .lang = TtsLang::ES,
                                      .voiceId = "M3",
                                      .quality = TtsQuality::Auto,
                                      .speed = gTts.defaultSpeed()});
    audio.samples = toPcm16(pcm);
    audio.sampleRate = gTts.sampleRate();
  }

  TapoTalkConfig config;
  config.host = options.host;
  config.port = options.mediaPort;
  config.username = options.cloudUser;
  config.cloudPassword = options.cloudPassword;
  config.mode = options.talkMode;
  config.packetMs = options.talkPacketMs;
  config.framing = tapoTalkFramingFromString(options.talkFraming);

  TapoTalkClient client(config);
  const auto opened = client.open();
  std::cout << "\n── talk session ──\n";
  if (!opened.ok) {
    std::cout << "  FAIL  " << opened.error << "\n";
    std::cout << client.state().toStyledString();
    return 1;
  }
  std::cout << "  OK\n" << client.state().toStyledString();

  const CancellationToken token;
  const auto sent = client.send(audio, token);
  report("talk playback", sent, options.verbose);
  client.close();
  return sent.ok ? 0 : 1;
}

int runTsDump(const std::string& path)
{
  constexpr int kRate = 8000;
  constexpr int kPacketMs = 20;
  constexpr int kSamplesPerPacket = kRate * kPacketMs / 1000;
  std::vector<int16_t> tone(kRate);
  for (size_t i = 0; i < tone.size(); ++i)
    tone[i] = static_cast<int16_t>(
        12000.0 *
        std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / kRate));
  const auto encoded = tapo_audio::encodeALaw(tone);

  TapoTsMuxer muxer{TapoTsConfig{}};
  std::string stream;
  int64_t pts = 0;
  for (size_t offset = 0; offset < encoded.size();
       offset += kSamplesPerPacket) {
    const size_t take =
        std::min<size_t>(kSamplesPerPacket, encoded.size() - offset);
    if (offset == 0)
      stream += muxer.tables();
    stream += muxer.frame(
        {.payload =
             std::vector<uint8_t>(encoded.begin() + static_cast<long>(offset),
                                  encoded.begin() +
                                      static_cast<long>(offset + take)),
         .pts90k = pts});
    pts += 90000 * kPacketMs / 1000;
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "cannot write " << path << "\n";
    return 1;
  }
  out.write(stream.data(), static_cast<long>(stream.size()));

  const bool aligned = stream.size() % 188 == 0;
  bool syncOk = true;
  for (size_t i = 0; i < stream.size(); i += 188) {
    if (static_cast<uint8_t>(stream[i]) != 0x47) {
      syncOk = false;
      break;
    }
  }
  std::cout << "── mpeg-ts muxer self-check ──\n"
            << "  bytes:          " << stream.size() << "\n"
            << "  packets:        " << stream.size() / 188 << "\n"
            << "  188-aligned:    " << (aligned ? "yes" : "NO") << "\n"
            << "  sync bytes ok:  " << (syncOk ? "yes" : "NO") << "\n"
            << "  written to:     " << path << "\n";
  return aligned && syncOk ? 0 : 1;
}

int runOffline()
{
  int failures = 0;
  const auto check = [&](const char* what, bool got, bool want) {
    const bool ok = got == want;
    if (!ok)
      ++failures;
    std::printf("  [%s] %-52s got=%d want=%d\n", ok ? "OK" : "FAIL", what,
                static_cast<int>(got), static_cast<int>(want));
  };
  std::printf("── offline checks ──\n");

  std::vector<int16_t> fullScale(64, -32768);
  const auto gained = tapoApplySpeakerGain(
      {.samples = fullScale, .maxGain = 3.0, .targetPeak = 26000.0});
  check("AGC does not invert phase at full scale", gained[0] < 0, true);
  check("AGC does not amplify what already saturates", gained[0] <= -20000,
        true);

  std::printf("  %s (%d failures)\n",
              failures == 0 ? "ALL OK" : "THERE ARE FAILURES", failures);
  return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
  ProbeOptions options;
  if (!parseArgs(argc, argv, options)) {
    printUsage();
    return 1;
  }
  if (!options.tsDumpPath.empty())
    return runTsDump(options.tsDumpPath);
  if (options.doOffline)
    return runOffline();
  loadDefaults(options);

  const bool anyControlAction =
      options.doInfo || options.doCreds || options.doBatch || options.doPtz ||
      options.doPtzStep || options.doEvents || options.doPresets ||
      options.doAudioConfig || !options.rawPayload.empty() ||
      !options.dayNightMode.empty();
  if (!anyControlAction && !options.doTalk)
    options.doInfo = true;

  int status = 0;

  if (anyControlAction) {
    const auto candidates = candidatesOf(options);
    if (candidates.empty()) {
      std::cerr << "no credentials: pass --user/--pass or --cloud-pass\n";
      return 1;
    }

    TapoClientConfig config;
    config.host = options.host;
    config.port = options.controlPort;
    config.candidates = candidates;
    config.transport = tapoTransportPreferenceFromString(options.transport);

    TapoApi api(config);
    const auto connected = api.connect();
    std::cout << "── connection ──\n";
    if (!connected.ok) {
      std::cout << "  FAIL  " << connected.error;
      if (connected.errorCode != 0)
        std::cout << " (error_code " << connected.errorCode << ")";
      std::cout << "\n";
      return 1;
    }
    std::cout << "  OK\n" << api.state().toStyledString();

    if (options.doCreds)
      std::cout << "\n── credentials ──\n  working pair: "
                << api.state()["credential"].asString() << " over "
                << api.state()["transport"].asString() << " (hash "
                << api.state()["hashAlgorithm"].asString() << ")\n";

    if (options.doInfo)
      report("get_device_info", api.getDeviceInfo(), options.verbose);

    if (options.doBatch) {
      const auto batch = api.getStatus();
      std::cout << "\n── multipleRequest status ──\n";
      std::cout << batch.toJson().toStyledString();
      if (options.verbose)
        std::cout << batch.raw.toStyledString();
      if (!batch.ok)
        status = 1;
    }

    if (options.doPresets)
      report("presets", api.getPresets(), options.verbose);

    if (options.doAudioConfig)
      report("audio config", api.getAudioConfig(), options.verbose);

    if (options.doPtz)
      report("motorMove", api.move({.x = options.ptzX, .y = options.ptzY}),
             options.verbose);

    if (options.doPtzStep)
      report("motorMoveStep", api.step({.angle = options.ptzStep}),
             options.verbose);

    if (!options.dayNightMode.empty())
      report("day/night", api.setDayNight({.mode = options.dayNightMode}),
             options.verbose);

    if (options.doEvents) {
      const int64_t now =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      report("searchDetectionList",
             api.searchDetectionList(
                 {.startTime =
                      now - static_cast<int64_t>(options.eventHours) * 3600,
                  .endTime = now,
                  .startIndex = 0,
                  .endIndex = 999}),
             true);
    }

    if (!options.rawPayload.empty()) {
      const Json::Value payload = json_util::fromString(options.rawPayload);
      if (payload.isNull()) {
        std::cerr << "--raw payload is not valid JSON\n";
        status = 1;
      }
      else {
        report("raw", api.callRaw(payload), true);
      }
    }
  }

  if (options.doTalk)
    status |= runTalk(options);

  return status;
}
