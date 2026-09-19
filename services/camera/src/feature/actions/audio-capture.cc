#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <feature/actions/audio-capture.hxx>
#include <memory>
#include <mutex>
#include <poll.h>
#include <shared/wrapper/audio/endpoint-detector.hxx>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{
constexpr int kSampleRate = 16000;

std::string boundedSeconds(int seconds)
{
  return std::to_string(std::clamp(seconds, 1, 10));
}

struct ReadLoopInput
{
  int fd{0};
  int deadlineMs{0};
  std::string& out;
  EndpointDetector* detector{nullptr};
  bool* endpointed{nullptr};
};

enum class ReadOutcome : uint8_t
{
  Eof = 0,
  Endpointed,
  Timeout,
  Error
};

// Reads the child's stdout until EOF, the deadline or an endpoint.
ReadOutcome readPipe(const ReadLoopInput& input)
{
  const auto start = std::chrono::steady_clock::now();
  char buffer[4096];
  std::vector<int16_t> samples;
  samples.reserve(sizeof(buffer));
  uint8_t leftover = 0;
  bool hasLeftover = false;
  while (true) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    const int remaining = input.deadlineMs - static_cast<int>(elapsed);
    if (remaining <= 0)
      return ReadOutcome::Timeout;

    pollfd entry{};
    entry.fd = input.fd;
    entry.events = POLLIN;
    const int ready = ::poll(&entry, 1, remaining);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return ReadOutcome::Error;
    }
    if (ready == 0)
      return ReadOutcome::Timeout;

    const ssize_t count = ::read(input.fd, buffer, sizeof(buffer));
    if (count > 0) {
      input.out.append(buffer, static_cast<size_t>(count));
      if (input.detector != nullptr) {
        const size_t bytes = static_cast<size_t>(count);
        samples.clear();
        size_t index = 0;
        if (hasLeftover && bytes > 0) {
          const uint16_t low = leftover;
          const uint16_t high = static_cast<uint8_t>(buffer[0]);
          samples.push_back(static_cast<int16_t>(low | (high << 8)));
          index = 1;
          hasLeftover = false;
        }
        for (; index + 1 < bytes; index += 2) {
          const uint16_t low = static_cast<uint8_t>(buffer[index]);
          const uint16_t high = static_cast<uint8_t>(buffer[index + 1]);
          samples.push_back(static_cast<int16_t>(low | (high << 8)));
        }
        if (index < bytes) {
          leftover = static_cast<uint8_t>(buffer[index]);
          hasLeftover = true;
        }
        if (!samples.empty()) {
          const EndpointStatus status =
              input.detector->process(samples.data(), samples.size());
          if (status.endpointed) {
            if (input.endpointed != nullptr)
              *input.endpointed = true;
            return ReadOutcome::Endpointed;
          }
        }
      }
      continue;
    }
    if (count == 0)
      return ReadOutcome::Eof;
    if (errno == EINTR)
      continue;
    return ReadOutcome::Error;
  }
}
} // namespace

namespace
{
std::mutex gCaptureMutex;
audio_capture::CaptureFunction gCaptureOverride;
} // namespace

void audio_capture::setCaptureFunctionForTest(CaptureFunction function)
{
  std::lock_guard lock(gCaptureMutex);
  gCaptureOverride = std::move(function);
}

static AudioCaptureResult captureReal(const AudioCaptureInput& input)
{
  AudioCaptureResult result;
  if (input.url.empty()) {
    result.status = AudioCaptureStatus::InvalidAudio;
    result.error = "capture url is empty";
    return result;
  }

  int pipeFds[2];
  if (::pipe(pipeFds) != 0) {
    result.status = AudioCaptureStatus::SpawnFailed;
    result.error = std::strerror(errno);
    return result;
  }

  const std::string seconds = boundedSeconds(input.seconds);
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    result.status = AudioCaptureStatus::SpawnFailed;
    result.error = std::strerror(errno);
    return result;
  }

  if (child == 0) {
    ::close(pipeFds[0]);
    ::dup2(pipeFds[1], STDOUT_FILENO);
    ::close(pipeFds[1]);
    const int nullFd = ::open("/dev/null", O_WRONLY);
    if (nullFd >= 0)
      ::dup2(nullFd, STDERR_FILENO);
    ::execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error",
             "-nostdin", "-rtsp_transport", "tcp", "-t", seconds.c_str(), "-i",
             input.url.c_str(), "-vn", "-ac", "1", "-ar", "16000", "-f",
             "s16le", "pipe:1", static_cast<char*>(nullptr));
    ::_exit(127);
  }

  ::close(pipeFds[1]);
  std::string raw;
  const int deadlineMs = (std::stoi(seconds) + 6) * 1000;
  std::unique_ptr<EndpointDetector> detector;
  bool endpointed = false;
  if (input.endpoint)
    detector = std::make_unique<EndpointDetector>(
        audio_endpoint::cameraListenDefaults());
  const ReadOutcome outcome = readPipe({.fd = pipeFds[0],
                                        .deadlineMs = deadlineMs,
                                        .out = raw,
                                        .detector = detector.get(),
                                        .endpointed = &endpointed});
  ::close(pipeFds[0]);

  if (outcome == ReadOutcome::Timeout || outcome == ReadOutcome::Error) {
    ::kill(child, SIGKILL);
    result.status = outcome == ReadOutcome::Timeout
                        ? AudioCaptureStatus::Timeout
                        : AudioCaptureStatus::ReadFailed;
    result.error = outcome == ReadOutcome::Timeout ? "capture timeout"
                                                   : "capture read failed";
  }

  int childStatus = 0;
  ::waitpid(child, &childStatus, 0);

  if (outcome == ReadOutcome::Timeout || outcome == ReadOutcome::Error)
    return result;

  const bool exitedCleanly =
      WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0;
  if (!exitedCleanly) {
    result.status = AudioCaptureStatus::InvalidAudio;
    result.error = "ffmpeg exited without success";
    return result;
  }

  if (raw.size() < 2) {
    result.status = AudioCaptureStatus::InvalidAudio;
    result.error = "no audio captured";
    return result;
  }

  result.samples.resize(raw.size() / sizeof(int16_t));
  std::memcpy(result.samples.data(), raw.data(),
              result.samples.size() * sizeof(int16_t));
  result.speechDetected = detector != nullptr && detector->speechDetected();
  result.endpointed = endpointed;
  result.ok = true;
  result.status = AudioCaptureStatus::Success;
  return result;
}

AudioCaptureResult audio_capture::capture(const AudioCaptureInput& input)
{
  audio_capture::CaptureFunction override;
  {
    std::lock_guard lock(gCaptureMutex);
    override = gCaptureOverride;
  }
  if (override)
    return override(input);
  return captureReal(input);
}
