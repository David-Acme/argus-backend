#include "go2rtc-manager.hxx"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <fcntl.h>
#include <fstream>
#include <shared/services/config-service/config-service.hxx>
#include <arpa/inet.h>
#include <netinet/in.h>
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

bool isPrivateHost(const std::string& host)
{
  if (host == "localhost" || host.rfind("127.", 0) == 0)
    return true;
  if (host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0)
    return true;
  if (host.rfind("172.", 0) == 0) {
    const auto dot = host.find('.', 4);
    if (dot != std::string::npos) {
      const int second = std::atoi(host.substr(4, dot - 4).c_str());
      if (second >= 16 && second <= 31)
        return true;
    }
  }
  return false;
}

} // namespace

std::vector<Go2rtcSource> Go2rtcManager::sources_;
std::mutex Go2rtcManager::mutex_;
std::atomic<bool> Go2rtcManager::stopping_{false};
std::atomic<bool> Go2rtcManager::healthy_{false};
int64_t Go2rtcManager::pid_ = 0;
int Go2rtcManager::restarts_ = 0;
std::string Go2rtcManager::lastError_;
std::string Go2rtcManager::binPath_ = kDefaultBin;
std::string Go2rtcManager::configPath_ = kDefaultConfig;
std::string Go2rtcManager::apiAddr_ = kDefaultApi;
std::string Go2rtcManager::rtspAddr_ = kDefaultRtsp;
int Go2rtcManager::maxRestarts_ = 8;

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
        c == '$' || c == '`')
      return false;
  }

  auto hostStart = url.find("://");
  if (hostStart == std::string::npos)
    return false;
  hostStart += 3;
  const auto at = url.find('@', hostStart);
  if (at != std::string::npos)
    hostStart = at + 1;
  auto hostEnd = url.find_first_of(":/?", hostStart);
  if (hostEnd == std::string::npos)
    hostEnd = url.size();
  const std::string host = url.substr(hostStart, hostEnd - hostStart);

  // RTSP and the Tapo protocols are unauthenticated on the wire; a public host
  // here would leak camera credentials outside the LAN.
  return isPrivateHost(host);
}

std::string Go2rtcManager::apiBase()
{
  return "http://" + apiAddr_;
}

std::string Go2rtcManager::streamName(int64_t cameraId)
{
  return "cam" + std::to_string(cameraId);
}

bool Go2rtcManager::writeConfig()
{
  std::ofstream out(configPath_, std::ios::trunc);
  if (!out.is_open()) {
    lastError_ = "cannot write " + configPath_;
    return false;
  }

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
  out.close();

  // The file embeds camera credentials.
  ::chmod(configPath_.c_str(), S_IRUSR | S_IWUSR);
  return true;
}

bool Go2rtcManager::spawn()
{
  if (::access(binPath_.c_str(), X_OK) != 0) {
    lastError_ = "go2rtc binary not executable: " + binPath_;
    LOG_ERROR << "Go2rtc: " << lastError_;
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    lastError_ = std::string("fork failed: ") + std::strerror(errno);
    return false;
  }

  if (pid == 0) {
    ::setsid();
    const int logFd =
        ::open("go2rtc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logFd >= 0) {
      ::dup2(logFd, STDOUT_FILENO);
      ::dup2(logFd, STDERR_FILENO);
      ::close(logFd);
    }
    // Credentials live in the config file, never in argv: /proc/<pid>/cmdline
    // is world-readable.
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
  const bool ok = ::connect(sock, reinterpret_cast<sockaddr*>(&addr),
                            sizeof(addr)) == 0;
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
  lastError_ = "go2rtc did not become ready";
  LOG_WARN << "Go2rtc: " << lastError_;
  return false;
}

void Go2rtcManager::supervise()
{
  int backoffMs = 250;
  while (!stopping_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (stopping_.load(std::memory_order_relaxed))
      break;

    const bool ok = healthCheck();
    healthy_.store(ok, std::memory_order_relaxed);
    if (ok) {
      backoffMs = 250;
      continue;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_relaxed))
      break;
    if (restarts_ >= maxRestarts_) {
      lastError_ = "go2rtc restart limit reached";
      LOG_ERROR << "Go2rtc: " << lastError_ << " (" << restarts_
                << "); giving up until the next init()";
      return;
    }

    ++restarts_;
    LOG_WARN << "Go2rtc: unhealthy, restart " << restarts_ << "/"
             << maxRestarts_ << " in " << backoffMs << "ms";
    terminate();
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
    backoffMs = std::min(backoffMs * 2, 30000);
    writeConfig();
    spawn();
  }
}

void Go2rtcManager::init()
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (const auto v = ConfigService::getString("streaming.go2rtc_bin"); !v.empty())
    binPath_ = v;
  if (const auto v = ConfigService::getString("streaming.go2rtc_config"); !v.empty())
    configPath_ = v;
  if (const auto v = ConfigService::getString("streaming.go2rtc_api"); !v.empty())
    apiAddr_ = v;
  if (const auto v = ConfigService::getString("streaming.go2rtc_rtsp"); !v.empty())
    rtspAddr_ = v;
  if (const int v = ConfigService::getInt("streaming.max_restarts"); v > 0)
    maxRestarts_ = v;

  stopping_.store(false, std::memory_order_relaxed);
  restarts_ = 0;

  if (!writeConfig()) {
    LOG_ERROR << "Go2rtc: " << lastError_;
    return;
  }
  if (!spawn())
    return;
  waitReady(5000);

  std::thread(&Go2rtcManager::supervise).detach();
  LOG_INFO << "Go2rtc: supervisor running (max_restarts=" << maxRestarts_ << ")";
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
  s.restarts = restarts_;
  s.pid = pid_;
  s.lastError = lastError_;
  return s;
}

bool Go2rtcManager::addSource(const Go2rtcSource& source)
{
  if (!isSafeName(source.name)) {
    LOG_WARN << "Go2rtc: rejected unsafe stream name";
    return false;
  }
  if (!isSafeUrl(source.url)) {
    LOG_WARN << "Go2rtc: rejected unsafe or non-private source url";
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sources_.begin(), sources_.end(),
                         [&](const Go2rtcSource& s) {
                           return s.name == source.name;
                         });
  if (it != sources_.end())
    it->url = source.url;
  else
    sources_.push_back(source);

  if (!writeConfig())
    return false;
  if (pid_ > 0) {
    terminate();
    if (!spawn())
      return false;
    return waitReady(5000);
  }
  return true;
}

bool Go2rtcManager::removeSource(const std::string& name)
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto before = sources_.size();
  sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                [&](const Go2rtcSource& s) {
                                  return s.name == name;
                                }),
                 sources_.end());
  if (sources_.size() == before)
    return false;
  if (!writeConfig())
    return false;
  if (pid_ > 0) {
    terminate();
    if (!spawn())
      return false;
    return waitReady(5000);
  }
  return true;
}
