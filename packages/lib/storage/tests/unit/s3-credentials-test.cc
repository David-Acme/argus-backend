#include <doctest/doctest.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <chrono>
#include <config/config-service.hxx>
#include <cstdint>
#include <cstdio>
#include <drogon/drogon.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <storage/details/s3-credentials.hxx>
#include <storage/details/s3-signing.hxx>
#include <storage/s3-storage-service.hxx>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <trantor/utils/Logger.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

constexpr int kWaitMillis = 10000;

s3_credentials::Credentials writerKeys()
{
  return {.accessKey = "WRITERKEY", .secretKey = "writersecret"};
}

struct ScratchFile
{
  std::string name;
  std::string body;
};

struct StorageSetup
{
  std::string endpoint;
  std::string credentialsFile;
};

struct CapturedCall
{
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
};

std::filesystem::path scratchPath(const std::string& name)
{
  return std::filesystem::temp_directory_path() /
         ("argus-s3-credentials-" + std::to_string(getpid()) + "-" + name);
}

std::string writeFile(const ScratchFile& file)
{
  const auto path = scratchPath(file.name);
  std::ofstream(path) << file.body;
  return path.string();
}

std::string credentialsJson(const s3_credentials::Credentials& keys)
{
  return R"({"credentials":{"accessKey":")" + keys.accessKey +
         R"(","secretKey":")" + keys.secretKey + R"("}})";
}

void useStorage(const StorageSetup& setup)
{
  ConfigService::setRuntimeString("storage.mode", "s3");
  ConfigService::setRuntimeString("storage.s3.endpoint", setup.endpoint);
  ConfigService::setRuntimeString("storage.s3.region", "us-east-1");
  ConfigService::setRuntimeString("storage.s3.bucket", "argus-test");
  ConfigService::setRuntimeString("storage.s3.credentials_file",
                                  setup.credentialsFile);
}

std::string lowered(std::string_view text)
{
  std::string output(text);
  for (auto& c : output)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return output;
}

CapturedCall parseCall(std::string_view head)
{
  CapturedCall call;
  const auto lineEnd = head.find("\r\n");
  const auto requestLine = head.substr(0, lineEnd);
  const auto methodEnd = requestLine.find(' ');
  const auto pathEnd = requestLine.find(' ', methodEnd + 1);
  if (methodEnd == std::string_view::npos || pathEnd == std::string_view::npos)
    return call;
  call.method = std::string(requestLine.substr(0, methodEnd));
  call.path =
      std::string(requestLine.substr(methodEnd + 1, pathEnd - methodEnd - 1));
  auto rest = lineEnd == std::string_view::npos ? std::string_view{}
                                                : head.substr(lineEnd + 2);
  while (!rest.empty()) {
    const auto next = rest.find("\r\n");
    const auto line = rest.substr(0, next);
    rest = next == std::string_view::npos ? std::string_view{}
                                          : rest.substr(next + 2);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos)
      continue;
    auto value = line.substr(colon + 1);
    value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
    call.headers[lowered(line.substr(0, colon))] = std::string(value);
  }
  return call;
}

std::string headerOf(const CapturedCall& call, const std::string& name)
{
  const auto found = call.headers.find(name);
  return found == call.headers.end() ? std::string{} : found->second;
}

std::string authorizationFor(const CapturedCall& call,
                             const s3_credentials::Credentials& keys)
{
  const auto payloadHash = headerOf(call, "x-amz-content-sha256");
  const auto timestamp = headerOf(call, "x-amz-date");
  return s3_signing::sign({
                              .method = call.method,
                              .canonicalUri = call.path,
                              .canonicalQuery = "",
                              .headers = {{"host", headerOf(call, "host")},
                                          {"x-amz-content-sha256", payloadHash},
                                          {"x-amz-date", timestamp}},
                              .payloadHash = payloadHash,
                              .accessKey = keys.accessKey,
                              .secretKey = keys.secretKey,
                              .region = "us-east-1",
                              .timestamp = timestamp,
                          })
      .authorization;
}

class CaptureServer
{
public:
  CaptureServer() : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
  {
    if (listener_ < 0)
      throw std::runtime_error("capture server: socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 4) != 0) {
      ::close(listener_);
      throw std::runtime_error("capture server: bind");
    }
    socklen_t length = sizeof(address);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
  }

  ~CaptureServer() { ::close(listener_); }

  CaptureServer(const CaptureServer&) = delete;
  CaptureServer& operator=(const CaptureServer&) = delete;
  CaptureServer(CaptureServer&&) = delete;
  CaptureServer& operator=(CaptureServer&&) = delete;

  [[nodiscard]] std::string endpoint() const
  {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  [[nodiscard]] std::string serveOne() const
  {
    pollfd waiting{.fd = listener_, .events = POLLIN, .revents = 0};
    if (::poll(&waiting, 1, kWaitMillis) <= 0)
      return {};
    const int connection = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0)
      return {};
    std::string head;
    std::array<char, 4096> block{};
    while (head.find("\r\n\r\n") == std::string::npos) {
      pollfd readable{.fd = connection, .events = POLLIN, .revents = 0};
      if (::poll(&readable, 1, kWaitMillis) <= 0)
        break;
      const auto received = ::recv(connection, block.data(), block.size(), 0);
      if (received <= 0)
        break;
      head.append(block.data(), static_cast<std::size_t>(received));
    }
    constexpr std::string_view kAnswer =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    const auto sent =
        ::send(connection, kAnswer.data(), kAnswer.size(), MSG_NOSIGNAL);
    ::close(connection);
    if (std::cmp_not_equal(sent, kAnswer.size()))
      return {};
    return head.substr(0, head.find("\r\n\r\n"));
  }

private:
  int listener_;
  std::uint16_t port_ = 0;
};

class LoopRunner
{
public:
  LoopRunner()
      : loop_(drogon::app().getLoop()), thread_([loop = loop_] {
          loop->moveToCurrentThread();
          loop->loop();
        })
  {
    for (int i = 0; i < kWaitMillis && !loop_->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ~LoopRunner()
  {
    loop_->quit();
    thread_.join();
  }

  LoopRunner(const LoopRunner&) = delete;
  LoopRunner& operator=(const LoopRunner&) = delete;
  LoopRunner(LoopRunner&&) = delete;
  LoopRunner& operator=(LoopRunner&&) = delete;

  [[nodiscard]] bool running() const { return loop_->isRunning(); }

private:
  trantor::EventLoop* loop_;
  std::thread thread_;
};

CapturedCall fetchThrough(const CaptureServer& server)
{
  std::string head;
  std::jthread serving([&server, &head] { head = server.serveOne(); });
  const S3StorageService storage;
  const auto body = drogon::sync_wait(storage.get("faces/1/1.jpg"));
  serving.join();
  CHECK(body == "ok");
  return parseCall(head);
}

}

TEST_CASE("the writer's own credentials file is read")
{
  const auto path =
      writeFile({.name = "own.json", .body = credentialsJson(writerKeys())});

  const auto fromDisk = s3_credentials::fromFile(path);
  CHECK(fromDisk.has_value());
  const auto read = fromDisk.value_or(s3_credentials::Credentials{});
  CHECK(read.accessKey == "WRITERKEY");
  CHECK(read.secretKey == "writersecret");

  std::filesystem::remove(path);
}

TEST_CASE("the admin API's answer is read with the fields it adds")
{
  const auto path = writeFile(
      {.name = "answer.json",
       .body =
           R"({"credentials":{"accessKey":"JAO8WCU7XUN0E5ETQFO8","secretKey":"generated","sessionToken":"","expiration":"9999-12-31T23:59:59Z"}})"
           "\n"});

  const auto read = s3_credentials::fromFile(path);
  REQUIRE(read.has_value());
  CHECK(read->accessKey == "JAO8WCU7XUN0E5ETQFO8");
  CHECK(read->secretKey == "generated");

  std::filesystem::remove(path);
}

TEST_CASE("a credentials file that cannot be used is read as nothing")
{
  const std::vector<std::string> unusable{
      "",
      "not json",
      "[]",
      R"({"credentials":[]})",
      R"({"accessKey":"FLATKEY","secretKey":"flatsecret"})",
      R"({"credentials":{"accessKey":"ONLYKEY"}})",
      R"({"credentials":{"secretKey":"onlysecret"}})",
      R"({"credentials":{"accessKey":"","secretKey":"emptykey"}})",
      R"({"credentials":{"accessKey":"EMPTYSECRET","secretKey":""}})",
      R"({"credentials":{"accessKey":7,"secretKey":"numberkey"}})",
  };
  for (std::size_t i = 0; i < unusable.size(); ++i) {
    CAPTURE(unusable[i]);
    const auto path =
        writeFile({.name = "unusable-" + std::to_string(i) + ".json",
                   .body = unusable[i]});
    CHECK_FALSE(s3_credentials::fromFile(path).has_value());
    std::filesystem::remove(path);
  }

  const auto missing = scratchPath("missing.json").string();
  CHECK_FALSE(s3_credentials::fromFile(missing).has_value());

  const auto directory = scratchPath("directory");
  std::filesystem::create_directory(directory);
  CHECK_FALSE(s3_credentials::fromFile(directory.string()).has_value());
  std::filesystem::remove(directory);
}

TEST_CASE("a writer is configured from its credentials file alone")
{
  const S3StorageService storage;
  const auto path = writeFile(
      {.name = "service.json", .body = credentialsJson(writerKeys())});

  useStorage({.endpoint = "http://127.0.0.1:9", .credentialsFile = ""});
  CHECK_FALSE(storage.isConfigured());

  useStorage({.endpoint = "http://127.0.0.1:9", .credentialsFile = path});
  CHECK(storage.isConfigured());

  std::filesystem::remove(path);
  CHECK_FALSE(storage.isConfigured());

  ConfigService::setRuntimeString("storage.mode", "");
  CHECK_FALSE(storage.isConfigured());
}

TEST_CASE("a request is signed with the credentials file's keys, and refused "
          "once the file is gone")
{
  const LoopRunner loop;
  REQUIRE(loop.running());
  const CaptureServer server;
  const S3StorageService storage;
  const auto path =
      writeFile({.name = "signed.json", .body = credentialsJson(writerKeys())});
  useStorage({.endpoint = server.endpoint(), .credentialsFile = path});

  const auto call = fetchThrough(server);
  CHECK(call.method == "GET");
  CHECK(call.path == "/argus-test/faces/1/1.jpg");
  const auto signature = headerOf(call, "authorization");
  CHECK(signature.starts_with("AWS4-HMAC-SHA256 Credential=WRITERKEY/"));
  CHECK(signature == authorizationFor(call, writerKeys()));

  std::filesystem::remove(path);
  CHECK_THROWS_AS(drogon::sync_wait(storage.get("faces/1/1.jpg")),
                  std::runtime_error);
  CHECK_FALSE(storage.isConfigured());
}
