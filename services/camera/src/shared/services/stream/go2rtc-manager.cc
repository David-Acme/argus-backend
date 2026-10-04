#include "go2rtc-manager.hxx"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <fcntl.h>
#include <sstream>
#include <string_view>
#include <netinet/in.h>
#include <config/config-service.hxx>
#include <shared/utils/network-address/private-address.hxx>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace
{

constexpr const char* kDefaultBin = "third_party/go2rtc/go2rtc";
constexpr const char* kDefaultConfig = "go2rtc.yaml";
constexpr const char* kDefaultApi = "127.0.0.1:1984";
constexpr const char* kDefaultRtsp = "127.0.0.1:8554";
constexpr const char* kSubStreamSuffix = "-sub";
constexpr auto kHealthyResetAfter = std::chrono::seconds(60);

struct PrivateFile
{
  const std::string& path;
  std::string contents;
};

bool writePrivateFile(const PrivateFile& file)
{
  const std::string staging = file.path + ".tmp";
  const int fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
  if (fd < 0)
    return false;
  ::fchmod(fd, S_IRUSR | S_IWUSR);
  std::string_view pending = file.contents;
  while (!pending.empty()) {
    const ssize_t written = ::write(fd, pending.data(), pending.size());
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      ::close(fd);
      ::unlink(staging.c_str());
      return false;
    }
    pending.remove_prefix(static_cast<size_t>(written));
  }
  ::close(fd);
  return ::rename(staging.c_str(), file.path.c_str()) == 0;
}

std::string authorityHost(std::string_view url)
{
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos)
    return {};
  std::string_view authority = url.substr(scheme + 3);
  authority = authority.substr(0, authority.find_first_of("/?#"));
  if (const auto at = authority.rfind('@'); at != std::string_view::npos)
    authority = authority.substr(at + 1);
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    return close == std::string_view::npos ? std::string{}
                                           : std::string(authority.substr(1, close - 1));
  }
  return std::string(authority.substr(0, authority.find(':')));
}

}

Go2rtcManager::Go2rtcManager()
    : binPath_(kDefaultBin), configPath_(kDefaultConfig), apiAddr_(kDefaultApi),
      rtspAddr_(kDefaultRtsp)
{
}

Go2rtcManager::~Go2rtcManager()
{
  shutdown();
}

Go2rtcManager& Go2rtcManager::instance()
{
  static Go2rtcManager manager;
  return manager;
}

bool Go2rtcManager::isSafeName(const std::string& name)
{
  if (name.empty() || name.size() > 64)
    return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
  });
}

bool Go2rtcManager::isSafeUrl(const std::string& url)
{
  if (url.empty() || url.size() > 512)
    return false;
  if (url.rfind("rtsp://", 0) != 0 && url.rfind("tapo://", 0) != 0 &&
      url.rfind("http://", 0) != 0)
    return false;
  for (unsigned char c : url) {
    if (c < 0x20 || c == 0x7F)
      return false;
    if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\\' ||
        c == '$' || c == '`' || c == ' ' || c == '#')
      return false;
  }

  return network_address::isPrivate(authorityHost(url));
}

std::string Go2rtcManager::apiBase()
{
  return "http://" + apiAddr_;
}

std::string Go2rtcManager::rtspBase()
{
  return "rtsp://" + rtspAddr_;
}

std::string Go2rtcManager::sourceName(int64_t cameraId, CameraStream stream)
{
  std::string name = "cam" + std::to_string(cameraId);
  if (stream == CameraStream::Sub)
    name += kSubStreamSuffix;
  return name;
}

std::string Go2rtcManager::sourceFor(int64_t cameraId, CameraStreamRole role)
{
  return sourceName(cameraId, camera_stream_role::streamFor(role));
}

bool Go2rtcManager::writeConfig()
{
  std::ostringstream out;
  out << "api:\n";
  out << "  listen: \"" << apiAddr_ << "\"\n";
  out << "rtsp:\n";
  out << "  listen: \"" << rtspAddr_ << "\"\n";
  out << "webrtc:\n";
  out << "  listen: \"\"\n";
  out << "log:\n";
  out << "  level: info\n";
  out << "streams:\n";
  for (const auto& s : sources_) {
    if (!isSafeName(s.name) || !isSafeUrl(s.url))
      continue;
    out << "  " << s.name << ": " << s.url << "\n";
  }
  const auto isWarmStream = [](const Go2rtcSource& s) {
    return s.preload && isSafeName(s.name) && isSafeUrl(s.url);
  };
  if (std::ranges::any_of(sources_, isWarmStream)) {
    out << "preload:\n";
    for (const auto& s : sources_) {
      if (isWarmStream(s))
        out << "  " << s.name << ":\n";
    }
  }

  if (!writePrivateFile({.path = configPath_, .contents = out.str()})) {
    setError("cannot write " + configPath_);
    return false;
  }
  return true;
}

bool Go2rtcManager::spawn()
{
  if (::access(binPath_.c_str(), X_OK) != 0) {
    LOG_ERROR << "Go2rtc: binary not executable: " << binPath_;
    setError("go2rtc binary not executable: " + binPath_);
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    setError(std::string("fork failed: ") + std::strerror(errno));
    return false;
  }

  if (pid == 0) {
    ::setsid();
    const int logFd = ::open("go2rtc.log", O_WRONLY | O_CREAT | O_APPEND,
                             S_IRUSR | S_IWUSR);
    if (logFd >= 0) {
      ::fchmod(logFd, S_IRUSR | S_IWUSR);
      ::dup2(logFd, STDOUT_FILENO);
      ::dup2(logFd, STDERR_FILENO);
      ::close(logFd);
    }
    const char* argv[] = {binPath_.c_str(), "-config", configPath_.c_str(),
                          nullptr};
    ::execv(binPath_.c_str(), const_cast<char* const*>(argv));
    ::_exit(127);
  }

  pid_ = pid;
  LOG_INFO << "Go2rtc: started pid=" << pid << " api=" << apiAddr_;
  return true;
}

void Go2rtcManager::terminate()
{
  if (pid_ <= 0)
    return;

  ::kill(static_cast<pid_t>(pid_), SIGTERM);
  for (int i = 0; i < 50; ++i) {
    int st = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &st, WNOHANG);
    if (r == static_cast<pid_t>(pid_) || r < 0) {
      pid_ = 0;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
  int st = 0;
  ::waitpid(static_cast<pid_t>(pid_), &st, 0);
  pid_ = 0;
}

bool Go2rtcManager::healthCheck()
{
  if (pid_ <= 0)
    return false;
  int st = 0;
  if (::waitpid(static_cast<pid_t>(pid_), &st, WNOHANG) ==
      static_cast<pid_t>(pid_)) {
    pid_ = 0;
    return false;
  }

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return false;

  const auto colon = apiAddr_.find(':');
  const std::string host = apiAddr_.substr(0, colon);
  const int port = std::atoi(apiAddr_.c_str() + colon + 1);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  const bool ok =
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(sock);
  return ok;
}

bool Go2rtcManager::waitReady(int maxMs)
{
  const int stepMs = 100;
  for (int waited = 0; waited < maxMs; waited += stepMs) {
    if (healthCheck())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
  }
  setError("go2rtc did not become ready");
  LOG_WARN << "Go2rtc: did not become ready";
  return false;
}

void Go2rtcManager::supervise()
{
  int backoffMs = 250;
  auto healthySince = std::chrono::steady_clock::now();
  while (!stopping_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (stopping_.load(std::memory_order_relaxed))
      break;

    std::scoped_lock lock(mutex_);
    if (stopping_.load(std::memory_order_relaxed))
      break;
    const bool ok = healthCheck();
    healthy_.store(ok, std::memory_order_relaxed);
    if (ok) {
      backoffMs = 250;
      if (std::chrono::steady_clock::now() - healthySince >= kHealthyResetAfter)
        restarts_.store(0);
      continue;
    }

    if (restarts_.load() >= maxRestarts_) {
      setError("go2rtc restart limit reached");
      LOG_ERROR << "Go2rtc: restart limit reached (" << restarts_.load()
                << "); giving up until the next init()";
      return;
    }

    const int restart = restarts_.fetch_add(1) + 1;
    LOG_WARN << "Go2rtc: unhealthy, restart " << restart << "/"
             << maxRestarts_ << " in " << backoffMs << "ms";
    terminate();
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    backoffMs = std::min(backoffMs * 2, 30000);
    writeConfig();
    spawn();
    healthySince = std::chrono::steady_clock::now();
  }
}

void Go2rtcManager::init()
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (const auto v = ConfigService::getString("streaming.go2rtc_bin");
      !v.empty())
    binPath_ = v;
  if (const auto v = ConfigService::getString("streaming.go2rtc_config");
      !v.empty())
    configPath_ = v;
  if (const auto v = ConfigService::getString("streaming.go2rtc_api");
      !v.empty())
    apiAddr_ = v;
  if (const auto v = ConfigService::getString("streaming.go2rtc_rtsp");
      !v.empty())
    rtspAddr_ = v;
  if (const int v = ConfigService::getInt("streaming.max_restarts"); v > 0)
    maxRestarts_ = v;

  stopping_.store(false, std::memory_order_relaxed);
  restarts_.store(0);

  if (!writeConfig()) {
    LOG_ERROR << "Go2rtc: cannot write " << configPath_;
    return;
  }
  if (!spawn())
    return;
  waitReady(5000);

  std::thread(&Go2rtcManager::supervise, this).detach();
  LOG_INFO << "Go2rtc: supervisor running (max_restarts=" << maxRestarts_
           << ")";
}

void Go2rtcManager::shutdown()
{
  stopping_.store(true, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  terminate();
  healthy_.store(false, std::memory_order_relaxed);
  LOG_INFO << "Go2rtc shutdown";
}

bool Go2rtcManager::isRunning()
{
  return pid_ > 0;
}

Go2rtcStatus Go2rtcManager::status()
{
  Go2rtcStatus s;
  s.running = pid_ > 0;
  s.healthy = healthy_.load(std::memory_order_relaxed);
  s.restarts = restarts_.load();
  s.pid = pid_;
  std::scoped_lock lock(errorMutex_);
  s.lastError = lastError_;
  return s;
}

void Go2rtcManager::setError(std::string error)
{
  std::scoped_lock lock(errorMutex_);
  lastError_ = std::move(error);
}

bool Go2rtcManager::merge(const Go2rtcSourceChange& change)
{
  bool changed = false;
  for (const auto& name : change.removals) {
    const auto removed = std::erase_if(
        sources_, [&](const Go2rtcSource& s) { return s.name == name; });
    changed = changed || removed > 0;
  }
  for (const auto& source : change.upserts) {
    if (!isSafeName(source.name)) {
      LOG_WARN << "Go2rtc: rejected unsafe stream name";
      continue;
    }
    if (!isSafeUrl(source.url)) {
      LOG_WARN << "Go2rtc: rejected unsafe or non-private source url";
      continue;
    }
    const auto it = std::ranges::find(sources_, source.name, &Go2rtcSource::name);
    if (it == sources_.end()) {
      sources_.push_back(source);
      changed = true;
    }
    else if (it->url != source.url || it->preload != source.preload) {
      it->url = source.url;
      it->preload = source.preload;
      changed = true;
    }
  }
  return changed;
}

bool Go2rtcManager::applySources(const Go2rtcSourceChange& change)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!merge(change))
    return true;
  if (!writeConfig())
    return false;
  if (pid_ <= 0)
    return true;
  terminate();
  if (!spawn())
    return false;
  return waitReady(5000);
}
