#include "provision-process.hxx"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <ranges>
#include <string>
#include <utility>

extern char** environ;

namespace
{
constexpr std::string_view kOwnPrefix = "ARGUS_TTS_";
constexpr std::size_t kReadChunk = 4096;

std::filesystem::path canonicalOrEmpty(const std::filesystem::path& path)
{
  std::error_code error;
  auto resolved = std::filesystem::canonical(path, error);
  return error ? std::filesystem::path{} : resolved;
}

std::string printable(std::string_view line)
{
  std::string clean;
  clean.reserve(line.size());
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (line[index] == '\x1b') {
      while (index < line.size() && line[index] != 'm')
        ++index;
      continue;
    }
    if (line[index] != '\r')
      clean.push_back(line[index]);
  }
  return clean;
}

bool writable(const std::filesystem::path& path)
{
  return ::access(path.c_str(), W_OK) == 0;
}

std::vector<std::string> childEnvironment(const std::vector<std::string>& extra)
{
  std::vector<std::string> variables;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view variable(*entry);
    if (!variable.starts_with(kOwnPrefix))
      variables.emplace_back(variable);
  }
  variables.insert(variables.end(), extra.begin(), extra.end());
  return variables;
}

std::vector<char*> pointersTo(std::vector<std::string>& strings)
{
  std::vector<char*> pointers;
  pointers.reserve(strings.size() + 1);
  for (auto& text : strings)
    pointers.push_back(text.data());
  pointers.push_back(nullptr);
  return pointers;
}

class SpawnSetup
{
public:
  SpawnSetup()
  {
    ::posix_spawn_file_actions_init(&actions_);
    ::posix_spawnattr_init(&attributes_);
  }

  ~SpawnSetup()
  {
    ::posix_spawn_file_actions_destroy(&actions_);
    ::posix_spawnattr_destroy(&attributes_);
  }

  SpawnSetup(const SpawnSetup&) = delete;
  SpawnSetup& operator=(const SpawnSetup&) = delete;
  SpawnSetup(SpawnSetup&&) = delete;
  SpawnSetup& operator=(SpawnSetup&&) = delete;

  void redirect(int writeEnd, int readEnd)
  {
    ::posix_spawn_file_actions_adddup2(&actions_, writeEnd, STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions_, writeEnd, STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions_, readEnd);
    ::posix_spawn_file_actions_addclose(&actions_, writeEnd);
    ::posix_spawnattr_setflags(&attributes_, static_cast<short>(POSIX_SPAWN_SETPGROUP));
    ::posix_spawnattr_setpgroup(&attributes_, 0);
  }

  [[nodiscard]] const posix_spawn_file_actions_t* actions() const { return &actions_; }
  [[nodiscard]] const posix_spawnattr_t* attributes() const { return &attributes_; }

private:
  posix_spawn_file_actions_t actions_{};
  posix_spawnattr_t attributes_{};
};

void forwardLines(int readEnd, const std::function<void(std::string_view)>& onLine)
{
  std::array<char, kReadChunk> buffer{};
  std::string pending;
  while (true) {
    const auto count = ::read(readEnd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    pending.append(buffer.data(), static_cast<std::size_t>(count));
    std::size_t newline = 0;
    while ((newline = pending.find('\n')) != std::string::npos) {
      const auto line = printable(std::string_view(pending).substr(0, newline));
      if (!line.empty() && onLine)
        onLine(line);
      pending.erase(0, newline + 1);
    }
  }
  if (const auto line = printable(pending); !line.empty() && onLine)
    onLine(line);
}
}

bool onPath(std::string_view program)
{
  const char* const path = std::getenv("PATH");
  if (path == nullptr)
    return false;
  for (const auto entry : std::string_view(path) | std::views::split(':')) {
    const std::string_view directory(entry.begin(), entry.end());
    if (directory.empty())
      continue;
    const auto candidate = std::filesystem::path(directory) / program;
    if (::access(candidate.c_str(), X_OK) == 0)
      return true;
  }
  return false;
}

std::filesystem::path defaultToolchainPython()
{
  std::filesystem::path work;
  if (const char* const configured = std::getenv("ARGUS_POCKET_WORK"); configured != nullptr && *configured != '\0')
    work = configured;
  else if (const char* const cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && *cache != '\0')
    work = std::filesystem::path(cache) / "argus" / "pocket-export";
  else if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0')
    work = std::filesystem::path(home) / ".cache" / "argus" / "pocket-export";
  else
    return {};
  return work / "venv" / "bin" / "python";
}

std::optional<std::filesystem::path> provisionScriptFor(const ProvisioningPaths& paths)
{
  const auto models = canonicalOrEmpty(paths.modelsDir);
  if (models.empty() || models.filename() != "tts" || models.parent_path().filename() != "models")
    return std::nullopt;
  std::error_code error;
  if (std::filesystem::weakly_canonical(paths.pocketDir, error) != models / "pocket" || error)
    return std::nullopt;
  auto script = models.parent_path().parent_path() / "services" / "tts" / "scripts" / "provision.sh";
  if (!std::filesystem::is_regular_file(script, error))
    return std::nullopt;
  return script;
}

ProvisioningHost probeProvisioningHost(const ProvisioningPaths& paths)
{
  ProvisioningHost host{.script = provisionScriptFor(paths), .canDownload = false, .canExport = false};
  if (!host.script)
    return host;
  const auto target = std::filesystem::exists(paths.pocketDir) ? paths.pocketDir : paths.modelsDir;
  host.canDownload = writable(target) && onPath("bash") && onPath("curl");
  std::error_code error;
  const bool toolchain = onPath("uv") ||
                         (!paths.toolchainPython.empty() && std::filesystem::is_regular_file(paths.toolchainPython, error));
  host.canExport = host.canDownload && toolchain;
  return host;
}

int runProvisioning(const ProvisionCommand& command)
{
  std::array<int, 2> pipeEnds{-1, -1};
  if (::pipe(pipeEnds.data()) != 0)
    return -1;
  const auto [readEnd, writeEnd] = pipeEnds;

  std::vector<std::string> arguments{"bash", command.script.string()};
  arguments.insert(arguments.end(), command.arguments.begin(), command.arguments.end());
  auto variables = childEnvironment(command.environment);
  auto argv = pointersTo(arguments);
  auto envp = pointersTo(variables);

  SpawnSetup setup;
  setup.redirect(writeEnd, readEnd);
  pid_t child = 0;
  const int spawned = ::posix_spawnp(&child, "bash", setup.actions(), setup.attributes(), argv.data(), envp.data());
  ::close(writeEnd);
  if (spawned != 0) {
    ::close(readEnd);
    return -1;
  }
  if (command.onStart)
    command.onStart(child);
  forwardLines(readEnd, command.onLine);
  ::close(readEnd);

  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}
