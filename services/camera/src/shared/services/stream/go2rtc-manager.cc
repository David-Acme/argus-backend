#include "go2rtc-manager.hxx"

#include <algorithm>
#include <array>
#include <charconv>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <fcntl.h>
#include <optional>
#include <sstream>
#include <ranges>
#include <string_view>
#include <vector>
#include <netinet/in.h>
#include <config/config-service.hxx>
#include <shared/utils/network-address/private-address.hxx>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace
{

constexpr const char* kDefaultBin = "third_party/go2rtc/go2rtc";
constexpr const char* kDefaultConfig = "go2rtc.yaml";
constexpr const char* kDefaultApi = "127.0.0.1:1984";
constexpr const char* kDefaultRtsp = "127.0.0.1:8554";
constexpr const char* kDefaultWebRtcListen = ":8555";
constexpr const char* kSubStreamSuffix = "-sub";
constexpr const char* kOpusAudioSuffix = "-opus";
constexpr size_t kMaxListenBytes = 64;
constexpr auto kHealthyResetAfter = std::chrono::seconds(60);
constexpr auto kSuperviseTick = std::chrono::milliseconds(500);
constexpr auto kRestartCoalesce = std::chrono::milliseconds(750);
constexpr std::string_view kCredentialPrefix = "ARGUS_SRC_";

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

int portOf(std::string_view digits)
{
  int port = 0;
  if (digits.empty() || digits.size() > 5)
    return 0;
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
  if (error != std::errc{} || end != digits.data() + digits.size())
    return 0;
  return port <= 65535 ? port : 0;
}

bool isPort(std::string_view digits)
{
  return portOf(digits) > 0;
}

bool isAddressCharacter(unsigned char c)
{
  return std::isalnum(c) != 0 || c == '.' || c == ':' || c == '[' || c == ']' || c == '-';
}

std::vector<std::string> splitList(std::string_view list)
{
  std::vector<std::string> items;
  while (!list.empty()) {
    const auto comma = list.find(',');
    std::string_view item = list.substr(0, comma);
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())) != 0)
      item.remove_prefix(1);
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())) != 0)
      item.remove_suffix(1);
    if (!item.empty())
      items.emplace_back(item);
    if (comma == std::string_view::npos)
      break;
    list.remove_prefix(comma + 1);
  }
  return items;
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

struct UserInfoSpan
{
  size_t begin{0};
  size_t length{0};
};

std::optional<UserInfoSpan> userInfoOf(std::string_view url)
{
  const auto scheme = url.find("://");
  if (scheme == std::string_view::npos)
    return std::nullopt;
  const size_t begin = scheme + 3;
  const std::string_view rest = url.substr(begin);
  const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
  const auto at = authority.rfind('@');
  if (at == std::string_view::npos || at == 0)
    return std::nullopt;
  return UserInfoSpan{.begin = begin, .length = at};
}

}

Go2rtcManager::Go2rtcManager()
    : binPath_(kDefaultBin), configPath_(kDefaultConfig), apiAddr_(kDefaultApi),
      rtspAddr_(kDefaultRtsp), webrtc_{.listen = kDefaultWebRtcListen, .candidates = {}}
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
  return std::ranges::all_of(name, [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
  });
}

bool Go2rtcManager::isSafeUrl(const std::string& url)
{
  if (url.empty() || url.size() > 512)
    return false;
  if (!url.starts_with("rtsp://") && !url.starts_with("tapo://") &&
      !url.starts_with("http://"))
    return false;
  for (unsigned char c : url) {
    if (c < 0x20 || c == 0x7F)
      return false;
    if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\\' ||
        c == '$' || c == '`' || c == ' ' || c == '#')
      return false;
  }

  return network_address::isCameraAddress(authorityHost(url));
}

bool Go2rtcManager::isSafeListen(const std::string& listen)
{
  if (listen.empty() || listen.size() > kMaxListenBytes)
    return false;
  const auto colon = listen.rfind(':');
  if (colon == std::string::npos)
    return false;
  const std::string_view host = std::string_view(listen).substr(0, colon);
  return std::ranges::all_of(host, isAddressCharacter) &&
         isPort(std::string_view(listen).substr(colon + 1));
}

bool Go2rtcManager::isSafeCandidate(const std::string& candidate)
{
  if (candidate.empty() || candidate.size() > kMaxListenBytes ||
      !std::ranges::all_of(candidate, isAddressCharacter))
    return false;
  const auto colon = candidate.rfind(':');
  const auto bracket = candidate.rfind(']');
  const bool hasPort = colon != std::string::npos &&
                       (bracket == std::string::npos || colon > bracket);
  return !hasPort || isPort(std::string_view(candidate).substr(colon + 1));
}

std::string Go2rtcManager::renderConfig(const Go2rtcConfigInput& input)
{
  const bool webrtc = isSafeListen(input.webrtc.listen);
  std::ostringstream out;
  out << "api:\n";
  out << "  listen: \"" << input.api << "\"\n";
  out << "rtsp:\n";
  out << "  listen: \"" << input.rtsp << "\"\n";
  out << "webrtc:\n";
  out << "  listen: \"" << (webrtc ? input.webrtc.listen : std::string()) << "\"\n";
  if (webrtc) {
    out << "  ice_servers: []\n";
    auto safe = input.webrtc.candidates | std::views::filter(isSafeCandidate);
    if (!std::ranges::empty(safe)) {
      out << "  candidates:\n";
      for (const auto& candidate : safe)
        out << "    - " << candidate << "\n";
    }
  }
  out << "log:\n";
  out << "  level: info\n";
  out << "streams:\n";
  const auto isServed = [](const Go2rtcSource& s) {
    return isSafeName(s.name) && isSafeUrl(s.url);
  };
  for (const auto& s : input.sources | std::views::filter(isServed)) {
    out << "  " << s.name << ": " << sealedUrl(s) << "\n";
    if (webrtc)
      out << "  " << opusAudioSource(s.name) << ": ffmpeg:" << s.name
          << "#video=copy#audio=opus\n";
  }
  const auto isWarmStream = [&isServed](const Go2rtcSource& s) {
    return s.preload && isServed(s);
  };
  if (std::ranges::any_of(input.sources, isWarmStream)) {
    out << "preload:\n";
    for (const auto& s : input.sources | std::views::filter(isWarmStream))
      out << "  " << s.name << ":\n";
  }
  return out.str();
}

std::string Go2rtcManager::credentialVariable(const std::string& name)
{
  std::string variable(kCredentialPrefix);
  variable.reserve(variable.size() + name.size());
  for (const unsigned char c : name)
    variable.push_back(std::isalnum(c) != 0 ? static_cast<char>(std::toupper(c)) : '_');
  return variable;
}

std::string Go2rtcManager::sealedUrl(const Go2rtcSource& source)
{
  const auto userInfo = userInfoOf(source.url);
  if (!userInfo)
    return source.url;
  std::string url = source.url;
  url.replace(userInfo->begin, userInfo->length, "${" + credentialVariable(source.name) + "}");
  return url;
}

std::vector<std::string> Go2rtcManager::credentialEnvironment(
    const std::vector<Go2rtcSource>& sources)
{
  std::vector<std::string> environment;
  for (const auto& source : sources) {
    if (!isSafeName(source.name) || !isSafeUrl(source.url))
      continue;
    if (const auto userInfo = userInfoOf(source.url))
      environment.push_back(credentialVariable(source.name) + "=" +
                            source.url.substr(userInfo->begin, userInfo->length));
  }
  return environment;
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

std::string Go2rtcManager::opusAudioSource(const std::string& source)
{
  return source + kOpusAudioSuffix;
}

bool Go2rtcManager::writeConfig()
{
  const std::string contents = renderConfig({.api = apiAddr_,
                                             .rtsp = rtspAddr_,
                                             .webrtc = webrtc_,
                                             .sources = sources_});
  if (!writePrivateFile({.path = configPath_, .contents = contents})) {
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

  std::vector<std::string> environment = credentialEnvironment(sources_);
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (!std::string_view(*entry).starts_with(kCredentialPrefix))
      environment.emplace_back(*entry);
  }
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (auto& entry : environment)
    envp.push_back(entry.data());
  envp.push_back(nullptr);

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
    const std::array<const char*, 4> argv{binPath_.c_str(), "-config",
                                          configPath_.c_str(), nullptr};
    ::execve(binPath_.c_str(), const_cast<char* const*>(argv.data()), envp.data());
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
  const int port = colon == std::string::npos ? 0 : portOf(std::string_view(apiAddr_).substr(colon + 1));

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

bool Go2rtcManager::waitFor(const std::stop_token& stop, std::chrono::milliseconds limit)
{
  std::unique_lock lock(wakeMutex_);
  auto until = std::chrono::steady_clock::now() + limit;
  if (restartDueAt_ && *restartDueAt_ < until)
    until = *restartDueAt_;
  wake_.wait_until(lock, stop, until,
                   [this, until] { return restartDueAt_ && *restartDueAt_ < until; });
  return !stop.stop_requested();
}

bool Go2rtcManager::restartDue()
{
  std::scoped_lock lock(wakeMutex_);
  if (!restartDueAt_ || std::chrono::steady_clock::now() < *restartDueAt_)
    return false;
  restartDueAt_.reset();
  return true;
}

void Go2rtcManager::serveRequestedRestart()
{
  std::scoped_lock lock(mutex_);
  if (stopping_.load(std::memory_order_relaxed) || pid_ <= 0)
    return;
  const bool ok = respawn();
  restartsServed_.fetch_add(1, std::memory_order_acq_rel);
  LOG_INFO << "Go2rtc: restarted on request" << (ok ? "" : " (not ready yet)");
}

void Go2rtcManager::supervise(std::stop_token stop)
{
  int backoffMs = 250;
  bool exhausted = false;
  auto healthySince = std::chrono::steady_clock::now();
  while (waitFor(stop, kSuperviseTick)) {
    if (restartDue()) {
      serveRequestedRestart();
      healthySince = std::chrono::steady_clock::now();
      continue;
    }
    {
      std::scoped_lock lock(mutex_);
      if (stopping_.load(std::memory_order_relaxed))
        break;
      const bool ok = healthCheck();
      healthy_.store(ok, std::memory_order_relaxed);
      if (ok) {
        backoffMs = 250;
        exhausted = false;
        if (std::chrono::steady_clock::now() - healthySince >= kHealthyResetAfter)
          restarts_.store(0);
        continue;
      }
      if (restarts_.load() >= maxRestarts_) {
        if (!exhausted) {
          setError("go2rtc restart limit reached");
          LOG_ERROR << "Go2rtc: restart limit reached (" << restarts_.load()
                    << "); no more crash restarts until the next init()";
        }
        exhausted = true;
        continue;
      }
      const int restart = restarts_.fetch_add(1) + 1;
      LOG_WARN << "Go2rtc: unhealthy, restart " << restart << "/"
               << maxRestarts_ << " in " << backoffMs << "ms";
      terminate();
    }
    if (!waitFor(stop, std::chrono::milliseconds(backoffMs)))
      break;
    backoffMs = std::min(backoffMs * 2, 30000);
    std::scoped_lock lock(mutex_);
    if (stopping_.load(std::memory_order_relaxed))
      break;
    writeConfig();
    spawn();
    healthySince = std::chrono::steady_clock::now();
  }
}

void Go2rtcManager::stopSupervisor()
{
  if (!supervisor_.joinable())
    return;
  supervisor_.request_stop();
  wake_.notify_all();
  if (supervisor_.get_id() != std::this_thread::get_id())
    supervisor_.join();
}

void Go2rtcManager::configure()
{
  std::scoped_lock lock(mutex_);

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
  if (ConfigService::hasKey("streaming.webrtc_listen"))
    webrtc_.listen = ConfigService::getString("streaming.webrtc_listen");
  webrtc_.candidates = splitList(ConfigService::getString("streaming.webrtc_candidates"));
  if (!webrtc_.listen.empty() && !isSafeListen(webrtc_.listen))
    LOG_WARN << "Go2rtc: streaming.webrtc_listen is not host:port; WebRTC stays off";
  webrtcOn_.store(isSafeListen(webrtc_.listen), std::memory_order_release);
  std::scoped_lock hostsLock(hostsMutex_);
  answerHosts_ = answerHostsOf(webrtc_.candidates);
}

void Go2rtcManager::start()
{
  stopSupervisor();
  {
    std::scoped_lock lock(mutex_);
    stopping_.store(false, std::memory_order_relaxed);
    restarts_.store(0);

    if (!writeConfig()) {
      LOG_ERROR << "Go2rtc: cannot write " << configPath_;
      return;
    }
    if (!spawn())
      return;
    waitReady(5000);
  }

  supervisor_ = std::jthread([this](std::stop_token stop) { supervise(std::move(stop)); });
  LOG_INFO << "Go2rtc: supervisor running (max_restarts=" << maxRestarts_
           << ")";
}

void Go2rtcManager::init()
{
  configure();
  start();
}

void Go2rtcManager::shutdown()
{
  stopping_.store(true, std::memory_order_relaxed);
  stopSupervisor();
  std::scoped_lock lock(mutex_);
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

bool Go2rtcManager::respawn()
{
  if (!writeConfig())
    return false;
  if (pid_ <= 0)
    return true;
  terminate();
  if (!spawn())
    return false;
  return waitReady(5000);
}

bool Go2rtcManager::applySources(const Go2rtcSourceChange& change)
{
  std::scoped_lock lock(mutex_);
  if (!merge(change))
    return true;
  return respawn();
}

void Go2rtcManager::requestRestart()
{
  {
    std::scoped_lock lock(wakeMutex_);
    if (!restartDueAt_)
      restartDueAt_ = std::chrono::steady_clock::now() + kRestartCoalesce;
  }
  wake_.notify_all();
}

int64_t Go2rtcManager::restartsServed() const
{
  return restartsServed_.load(std::memory_order_acquire);
}

bool Go2rtcManager::webrtcEnabled() const
{
  return webrtcOn_.load(std::memory_order_acquire);
}

std::vector<std::string> Go2rtcManager::answerHostsOf(const std::vector<std::string>& candidates)
{
  std::vector<std::string> hosts;
  for (const auto& candidate : candidates) {
    if (!isSafeCandidate(candidate))
      continue;
    if (candidate.starts_with("stun:"))
      return {};
    std::string_view host = candidate;
    const auto bracket = host.rfind(']');
    const auto colon = host.rfind(':');
    if (host.starts_with('[') && bracket != std::string_view::npos)
      host = host.substr(1, bracket - 1);
    else if (colon != std::string_view::npos && host.find(':') == colon)
      host = host.substr(0, colon);
    hosts.emplace_back(host);
  }
  return hosts;
}

std::vector<std::string> Go2rtcManager::webrtcAnswerHosts()
{
  std::scoped_lock lock(hostsMutex_);
  return answerHosts_;
}
