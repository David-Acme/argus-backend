#include <shared/services/tapo/tapo-crypto.hxx>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr const char* kBoundary = "--client-stream-boundary--";
constexpr const char* kUserAgent = "Tapo CameraClient Android";

std::string toLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string readSecret(const std::string& path)
{
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

struct Args
{
  std::string host;
  int port{8800};
  std::string passwordFile;
  std::string cameraUser{"acme01"};
  std::string cloudUser;
  bool help{false};
};

Args parse(int argc, char** argv)
{
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto next = [&]() -> std::string {
      return i + 1 < argc ? std::string(argv[++i]) : std::string();
    };
    if (flag == "--host")
      args.host = next();
    else if (flag == "--port")
      args.port = std::atoi(next().c_str());
    else if (flag == "--password-file")
      args.passwordFile = next();
    else if (flag == "--camera-user")
      args.cameraUser = next();
    else if (flag == "--cloud-user")
      args.cloudUser = next();
    else
      args.help = true;
  }
  return args;
}

std::string exchange(const Args& args, const std::string& authorization)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return {};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(args.port));
  if (::inet_pton(AF_INET, args.host.c_str(), &address.sin_addr) != 1) {
    ::close(fd);
    return {};
  }
  timeval timeout{};
  timeout.tv_sec = 8;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return {};
  }
  std::ostringstream head;
  head << "POST /stream HTTP/1.1\r\n"
       << "Host: " << args.host << ":" << args.port << "\r\n"
       << "Content-Type: multipart/mixed;boundary=" << kBoundary << "\r\n"
       << "User-Agent: " << kUserAgent << "\r\n"
       << "Connection: close\r\n";
  if (!authorization.empty())
    head << "Authorization: " << authorization << "\r\n";
  head << "\r\n";
  const std::string request = head.str();
  ::send(fd, request.data(), request.size(), 0);
  std::string answer;
  char buffer[4096];
  while (answer.find("\r\n\r\n") == std::string::npos) {
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0)
      break;
    answer.append(buffer, static_cast<size_t>(got));
  }
  ::close(fd);
  return answer;
}

std::optional<std::string> headerValue(const std::string& answer,
                                       const std::string& key)
{
  std::istringstream in(answer);
  std::string line;
  const std::string needle = toLower(key) + ":";
  while (std::getline(in, line)) {
    const std::string lowered = toLower(line);
    if (lowered.rfind(needle, 0) == 0)
      return line.substr(needle.size() + (line.size() > needle.size() ? 1 : 0));
  }
  return std::nullopt;
}

struct Variant
{
  std::string userLabel;
  std::string username;
  std::string passwordLabel;
  std::string password;
  std::string algorithm;
  bool lowercaseHex{false};
};

std::string buildAuthorization(const tapo_crypto::DigestChallenge& challenge,
                               const Variant& variant)
{
  const tapo_crypto::DigestInput input{
      .username = variant.username,
      .password = variant.password,
      .realm = challenge.realm,
      .nonce = challenge.nonce,
      .qop = challenge.qop,
      .opaque = challenge.opaque,
      .algorithm = variant.algorithm,
      .method = "POST",
      .uri = "/stream",
      .cnonce = "0123456789abcdef",
      .nonceCount = 1};
  const std::string header = tapo_crypto::buildDigestHeader(input);
  return variant.lowercaseHex ? toLower(header) : header;
}

void usage()
{
  std::cout << "argus-tapo-probe --host <ip> --password-file <path>\n"
               "                 [--port 8800] [--camera-user <user>] "
               "[--cloud-user <user>]\n"
               "Probes the Tapo talk channel with a matrix of authentication "
               "variants and prints one line per attempt (labels only; the "
               "password is never printed).\n";
}

}

int main(int argc, char** argv)
{
  const Args args = parse(argc, argv);
  if (args.help || args.host.empty() || args.passwordFile.empty()) {
    usage();
    return args.help ? 0 : 1;
  }
  const std::string password = readSecret(args.passwordFile);
  if (password.empty()) {
    std::cerr << "the password file is empty or unreadable\n";
    return 1;
  }

  const std::string challengeAnswer = exchange(args, {});
  const auto header = headerValue(challengeAnswer, "WWW-Authenticate");
  if (!header) {
    std::cerr << "no digest challenge from " << args.host << ":" << args.port
              << " ("
              << challengeAnswer.substr(0, challengeAnswer.find("\r\n"))
              << ")\n";
    return 1;
  }
  const tapo_crypto::DigestChallenge challenge =
      tapo_crypto::parseDigestChallenge(*header);
  std::cout << "endpoint POST /stream port " << args.port
            << " realm=" << challenge.realm
            << " algorithm=" << challenge.algorithm
            << " encrypt_type=" << challenge.encryptType
            << " qop=" << challenge.qop << "\n";

  std::vector<Variant> variants;
  const std::vector<std::pair<std::string, std::string>> users{
      {"camera", args.cameraUser},
      {"cloud", args.cloudUser},
      {"admin", "admin"}};
  const std::vector<std::pair<std::string, std::string>> passwords{
      {"plain", password},
      {"md5", tapo_crypto::md5Hex(password)},
      {"sha256", tapo_crypto::sha256Hex(password)}};
  for (const auto& [userLabel, username] : users) {
    if (username.empty())
      continue;
    for (const auto& [passwordLabel, value] : passwords) {
      for (const std::string& algorithm : {"MD5", "SHA-256"}) {
        for (const bool lowercase : {false, true}) {
          variants.push_back({.userLabel = userLabel,
                              .username = username,
                              .passwordLabel = passwordLabel,
                              .password = value,
                              .algorithm = algorithm,
                              .lowercaseHex = lowercase});
        }
      }
    }
  }

  int accepted = 0;
  for (const auto& variant : variants) {
    const std::string fresh = exchange(args, {});
    const auto freshHeader = headerValue(fresh, "WWW-Authenticate");
    if (!freshHeader) {
      std::cout << "user=" << variant.userLabel
                << " pass=" << variant.passwordLabel
                << " -> no fresh challenge ("
                << fresh.substr(0, fresh.find("\r\n")) << ")\n";
      continue;
    }
    const tapo_crypto::DigestChallenge freshChallenge =
        tapo_crypto::parseDigestChallenge(*freshHeader);
    const std::string answer =
        exchange(args, buildAuthorization(freshChallenge, variant));
    const std::string status =
        answer.substr(0, answer.find("\r\n"));
    const bool ok = status.find(" 200 ") != std::string::npos;
    if (ok)
      ++accepted;
    std::cout << "user=" << variant.userLabel
              << " pass=" << variant.passwordLabel
              << " digest=" << variant.algorithm
              << " hex=" << (variant.lowercaseHex ? "lower" : "upper")
              << " -> " << (status.empty() ? "no answer" : status)
              << (ok ? "  ACCEPTED" : "") << "\n";
  }
  std::cout << "variants=" << variants.size() << " accepted=" << accepted << "\n";
  return accepted > 0 ? 0 : 2;
}
