#include "ffmpeg-microphone-source.hxx"

#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/vocabulary/camera-stream-role.hxx>

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace
{
constexpr size_t kReadBytes = 4096;
}

FfmpegMicrophoneSource::FfmpegMicrophoneSource(std::string url) : url_(std::move(url)) {}

FfmpegMicrophoneSource::~FfmpegMicrophoneSource()
{
  close();
}

bool FfmpegMicrophoneSource::open()
{
  close();
  if (url_.empty())
    return false;
  std::array<int, 2> fds{-1, -1};
  if (::pipe2(fds.data(), O_CLOEXEC) != 0)
    return false;
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }
  if (child == 0) {
    ::close(fds[0]);
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[1]);
    const int nullFd = ::open("/dev/null", O_WRONLY);
    if (nullFd >= 0)
      ::dup2(nullFd, STDERR_FILENO);
    ::execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-rtsp_transport",
             "tcp", "-i", url_.c_str(), "-vn", "-ac", "1", "-ar", "16000", "-f", "s16le",
             "-flush_packets", "1", "pipe:1", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(fds[1]);
  child_ = child;
  fd_ = fds[0];
  leftover_.reset();
  return true;
}

MicrophoneRead FfmpegMicrophoneSource::read(std::chrono::milliseconds timeout)
{
  if (fd_ < 0)
    return {.status = MicrophoneReadStatus::Ended, .samples = {}};
  pollfd entry{};
  entry.fd = fd_;
  entry.events = POLLIN;
  const int ready = ::poll(&entry, 1, static_cast<int>(timeout.count()));
  if (ready < 0)
    return {.status = errno == EINTR ? MicrophoneReadStatus::Idle : MicrophoneReadStatus::Ended, .samples = {}};
  if (ready == 0)
    return {.status = MicrophoneReadStatus::Idle, .samples = {}};
  std::array<uint8_t, kReadBytes> buffer{};
  const ssize_t count = ::read(fd_, buffer.data(), buffer.size());
  if (count == 0)
    return {.status = MicrophoneReadStatus::Ended, .samples = {}};
  if (count < 0)
    return {.status = errno == EINTR || errno == EAGAIN ? MicrophoneReadStatus::Idle : MicrophoneReadStatus::Ended,
            .samples = {}};
  const auto bytes = static_cast<size_t>(count);
  MicrophoneRead out{.status = MicrophoneReadStatus::Samples, .samples = {}};
  out.samples.reserve((bytes / 2) + 1);
  size_t index = 0;
  if (leftover_) {
    const auto low = static_cast<uint16_t>(*leftover_);
    const auto high = static_cast<uint16_t>(buffer[0]);
    out.samples.push_back(static_cast<int16_t>(static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U))));
    leftover_.reset();
    index = 1;
  }
  for (; index + 1 < bytes; index += 2) {
    const auto low = static_cast<uint16_t>(buffer[index]);
    const auto high = static_cast<uint16_t>(buffer[index + 1]);
    out.samples.push_back(static_cast<int16_t>(static_cast<uint16_t>(low | static_cast<uint16_t>(high << 8U))));
  }
  if (index < bytes)
    leftover_ = buffer[index];
  return out;
}

void FfmpegMicrophoneSource::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (child_ > 0) {
    ::kill(child_, SIGTERM);
    int status = 0;
    if (::waitpid(child_, &status, WNOHANG) == 0) {
      ::usleep(50000);
      if (::waitpid(child_, &status, WNOHANG) == 0) {
        ::kill(child_, SIGKILL);
        ::waitpid(child_, &status, 0);
      }
    }
    child_ = -1;
  }
  leftover_.reset();
}

MicrophoneSourceFactory ffmpegMicrophoneSources()
{
  return [](int64_t cameraId) -> std::unique_ptr<MicrophoneSource> {
    const std::string url = Go2rtcManager::instance().rtspBase() + "/" +
                            Go2rtcManager::sourceFor(cameraId, CameraStreamRole::Listening);
    return std::make_unique<FfmpegMicrophoneSource>(url);
  };
}
