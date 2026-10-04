#pragma once

#include <shared/services/tapo/tapo-crypto.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fake_talk
{

inline constexpr const char* kCloudPassword = "cloud-secret";

std::string lower(std::string value)
{
  for (auto& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

class FakeConnection
{
public:
  explicit FakeConnection(int fd) : fd_(fd) {}

  ~FakeConnection()
  {
    if (fd_ >= 0)
      ::close(fd_);
  }

  std::string readHead()
  {
    const size_t split = fillUntil("\r\n\r\n");
    if (split == std::string::npos)
      return {};
    const std::string head = pending_.substr(0, split + 4);
    pending_.erase(0, split + 4);
    return head;
  }

  std::string readBody(size_t count)
  {
    while (pending_.size() < count && fill())
      ;
    if (pending_.size() < count)
      return {};
    std::string body = pending_.substr(0, count);
    pending_.erase(0, count);
    return body;
  }

  bool write(const std::string& data)
  {
    size_t sent = 0;
    while (sent < data.size()) {
      const auto n =
          ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      sent += static_cast<size_t>(n);
    }
    return true;
  }

private:
  size_t fillUntil(const std::string& marker)
  {
    for (;;) {
      const size_t found = pending_.find(marker);
      if (found != std::string::npos)
        return found;
      if (!fill())
        return std::string::npos;
    }
  }

  bool fill()
  {
    std::array<char, 4096> buffer{};
    const auto n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n <= 0)
      return false;
    pending_.append(buffer.data(), static_cast<size_t>(n));
    return true;
  }

  int fd_;
  std::string pending_;
};

struct HeaderLookup
{
  const std::string& head;
  const std::string& name;
};

std::string headerValueOf(const HeaderLookup& input)
{
  const std::string& head = input.head;
  const std::string& name = input.name;
  const std::string needle = name + ": ";
  const size_t begin = head.find(needle);
  if (begin == std::string::npos)
    return {};
  const size_t end = head.find("\r\n", begin);
  return head.substr(begin + needle.size(),
                     end == std::string::npos
                         ? std::string::npos
                         : end - begin - needle.size());
}

struct DigestLookup
{
  const std::string& authorization;
  const std::string& name;
};

std::string digestFieldOf(const DigestLookup& input)
{
  const std::string& authorization = input.authorization;
  const std::string& name = input.name;
  const std::string needle = name + "=";
  const size_t begin = authorization.find(needle);
  if (begin == std::string::npos)
    return {};
  size_t start = begin + needle.size();
  const bool quoted =
      start < authorization.size() && authorization[start] == '"';
  if (quoted)
    ++start;
  const size_t end = quoted ? authorization.find('"', start)
                            : authorization.find(',', start);
  return authorization.substr(
      start, end == std::string::npos ? std::string::npos : end - start);
}

size_t contentLengthOf(const std::string& head)
{
  const std::string length = headerValueOf({.head = head, .name = "Content-Length"});
  if (length.empty())
    return 0;
  const auto value = static_cast<size_t>(std::stoul(length));
  if (value > static_cast<size_t>(1024) * 1024)
    return 0;
  return value;
}

std::string sessionAnswer()
{
  const std::string body =
      "{\"type\":\"response\",\"seq\":1,\"params\":{\"talk\":"
      "{\"session_id\":31415,\"mode\":\"aec\"}}}";
  return "----device-stream-boundary--\r\n"
         "Content-Type: application/json\r\n"
         "X-Session-Id: 31415\r\n"
         "X-If-Encrypt: 0\r\n"
         "Content-Length: " +
         std::to_string(body.size()) + "\r\n\r\n" + body;
}

class FakeTalkChannel
{
public:
  enum class Mode : uint8_t
  {
    AcceptReference,
    RejectAll,
    Authless
  };

  explicit FakeTalkChannel(Mode mode, std::string encryptType = "3")
      : mode_(mode), encryptType_(std::move(encryptType))
  {
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listen_, 8);
    socklen_t len = sizeof(addr);
    ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }

  ~FakeTalkChannel() { stop(); }

  int port() const { return port_; }

  int connections() const
  {
    std::scoped_lock lock(mutex_);
    return connections_;
  }

  std::string answeredAuthorization() const
  {
    std::scoped_lock lock(mutex_);
    return answeredAuthorization_;
  }

  std::string challengeNonce() const
  {
    std::scoped_lock lock(mutex_);
    return challengeNonce_;
  }

  std::string keyExchangeNonce() const
  {
    std::scoped_lock lock(mutex_);
    return keyExchangeNonce_;
  }

  std::string sessionBody() const
  {
    std::scoped_lock lock(mutex_);
    return sessionBody_;
  }

  std::vector<std::string> audioParts() const
  {
    std::scoped_lock lock(mutex_);
    return audioParts_;
  }

  std::vector<std::string> audioSessionIds() const
  {
    std::scoped_lock lock(mutex_);
    return audioSessionIds_;
  }

  bool stopEncrypted() const
  {
    std::scoped_lock lock(mutex_);
    return stopEncrypted_;
  }

  std::string stopPlaintext() const
  {
    std::scoped_lock lock(mutex_);
    if (!stopEncrypted_)
      return stopBody_;
    const std::string nonce = keyExchangeNonce_;
    const auto key = tapo_crypto::md5Raw(
        nonce + ":" + tapo_crypto::sha256Hex(kCloudPassword));
    const auto iv = tapo_crypto::md5Raw(std::string("admin") + ":" + nonce);
    const auto plain = tapo_crypto::aes128CbcDecrypt(
        {.data = std::vector<uint8_t>(stopBody_.begin(), stopBody_.end()),
         .key = key,
         .iv = iv});
    return {plain.begin(), plain.end()};
  }

  void stop()
  {
    if (listen_ < 0)
      return;
    stopping_.store(true);
    ::shutdown(listen_, SHUT_RDWR);
    ::close(listen_);
    listen_ = -1;
    if (thread_.joinable())
      thread_.join();
  }

private:
  std::string digestChallenge(const std::string& nonce) const
  {
    return "HTTP/1.0 401 Unauthorized\r\n"
           "Server: Streamd\r\n"
           "Content-Length: 0\r\n"
           "WWW-Authenticate: Digest realm=\"TP-Link IP-Camera\","
           "algorithm=\"MD5\",encrypt_type=\"" +
           encryptType_ +
           "\",qop=\"auth\","
           "nonce=\"" +
           nonce +
           "\",opaque=\"64943214654649846565646421\"\r\n"
           "Connection: close\r\n\r\n";
  }

  std::string acceptedHead()
  {
    const std::string nonce = tapo_crypto::randomHex(32);
    {
      std::scoped_lock lock(mutex_);
      keyExchangeNonce_ = nonce;
    }
    return "HTTP/1.0 200 OK\r\n"
           "Content-Type: multipart/mixed;boundary="
           "--device-stream-boundary--\r\n"
           "Key-Exchange: version=\"1\", algorithm=\"aes-128-cbc\", "
           "username=\"admin\", nonce=\"" +
           nonce +
           "\", encrypt_type=\"" +
           encryptType_ +
           "\"\r\n"
           "Connection: keep-alive\r\n\r\n";
  }

  std::string authlessHead()
  {
    const std::string nonce = tapo_crypto::randomHex(32);
    {
      std::scoped_lock lock(mutex_);
      keyExchangeNonce_ = nonce;
    }
    return "HTTP/1.0 200 OK\r\n"
           "Content-Type: multipart/mixed;boundary="
           "--device-stream-boundary--\r\n"
           "Key-Exchange: version=\"1\", algorithm=\"aes-128-cbc\", "
           "username=\"none\", nonce=\"" +
           nonce +
           "\", encrypt_type=\"" +
           encryptType_ +
           "\"\r\n"
           "Connection: keep-alive\r\n\r\n";
  }

  bool digestMatches(const std::string& authorization) const
  {
    const std::string username = digestFieldOf({.authorization = authorization, .name = "username"});
    const std::string realm = digestFieldOf({.authorization = authorization, .name = "realm"});
    const std::string nonce = digestFieldOf({.authorization = authorization, .name = "nonce"});
    const std::string uri = digestFieldOf({.authorization = authorization, .name = "uri"});
    const std::string nc = digestFieldOf({.authorization = authorization, .name = "nc"});
    const std::string cnonce = digestFieldOf({.authorization = authorization, .name = "cnonce"});
    const std::string qop = digestFieldOf({.authorization = authorization, .name = "qop"});
    const std::string response = digestFieldOf({.authorization = authorization, .name = "response"});
    const std::string password =
        encryptType_ == "3" ? tapo_crypto::sha256Hex(kCloudPassword)
                            : tapo_crypto::md5Hex(kCloudPassword);
    const std::string ha1 = lower(
        tapo_crypto::md5Hex(username + ":" + realm + ":" + password));
    const std::string ha2 = lower(tapo_crypto::md5Hex("POST:" + uri));
    const std::string expected = lower(tapo_crypto::md5Hex(
        ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2));
    return expected == lower(response);
  }

  std::string nextChallenge()
  {
    const std::string nonce = tapo_crypto::randomHex(32);
    {
      std::scoped_lock lock(mutex_);
      if (challengeNonce_.empty())
        challengeNonce_ = nonce;
    }
    return digestChallenge(nonce);
  }

  void serveConnection(FakeConnection& connection)
  {
    std::string issuedNonce;
    for (;;) {
      const std::string head = connection.readHead();
      if (head.empty())
        return;
      const size_t bodyLength = contentLengthOf(head);
      std::string body;
      if (bodyLength > 0)
        body = connection.readBody(bodyLength);
      const std::string authorization = headerValueOf({.head = head, .name = "Authorization"});

      if (mode_ == Mode::Authless) {
        if (!connection.write(authlessHead()))
          return;
        if (!serveSessionPart(connection))
          return;
        return;
      }

      if (authorization.empty()) {
        issuedNonce = tapo_crypto::randomHex(32);
        {
          std::scoped_lock lock(mutex_);
          if (challengeNonce_.empty())
            challengeNonce_ = issuedNonce;
        }
        if (!connection.write(digestChallenge(issuedNonce)))
          return;
        continue;
      }

      const bool answersThisConnection =
          !issuedNonce.empty() &&
          authorization.find("nonce=\"" + issuedNonce + "\"") !=
              std::string::npos;
      if (mode_ == Mode::RejectAll || !answersThisConnection ||
          !digestMatches(authorization)) {
        if (!connection.write(nextChallenge()))
          return;
        continue;
      }

      {
        std::scoped_lock lock(mutex_);
        answeredAuthorization_ = authorization;
      }
      if (!connection.write(acceptedHead()))
        return;
      if (!serveSessionPart(connection))
        return;
      return;
    }
  }

  bool serveSessionPart(FakeConnection& connection)
  {
    const std::string part = connection.readHead();
    if (part.empty())
      return false;
    const size_t partBody = contentLengthOf(part);
    std::string payload;
    if (partBody > 0)
      payload = connection.readBody(partBody);
    {
      std::scoped_lock lock(mutex_);
      sessionBody_ = payload;
    }
    if (!connection.write(sessionAnswer()))
      return false;

    for (;;) {
      const std::string next = connection.readHead();
      if (next.empty())
        return true;
      const size_t length = contentLengthOf(next);
      std::string data;
      if (length > 0)
        data = connection.readBody(length);
      if (headerValueOf({.head = next, .name = "Content-Type"}) == "audio/mp2t") {
        std::scoped_lock lock(mutex_);
        audioParts_.push_back(data);
        audioSessionIds_.push_back(headerValueOf({.head = next, .name = "X-Session-Id"}));
        continue;
      }
      std::scoped_lock lock(mutex_);
      stopBody_ = data;
      stopEncrypted_ = headerValueOf({.head = next, .name = "X-If-Encrypt"}) == "1";
      return true;
    }
  }

  void serve()
  {
    while (!stopping_.load()) {
      const int fd = ::accept(listen_, nullptr, nullptr);
      if (fd < 0)
        break;
      {
        std::scoped_lock lock(mutex_);
        ++connections_;
      }
      FakeConnection connection(fd);
      serveConnection(connection);
    }
  }

  int listen_{-1};
  int port_{0};
  Mode mode_;
  std::string encryptType_;
  int connections_{0};
  std::string answeredAuthorization_;
  std::string challengeNonce_;
  std::string keyExchangeNonce_;
  std::string sessionBody_;
  std::string stopBody_;
  std::vector<std::string> audioParts_;
  std::vector<std::string> audioSessionIds_;
  bool stopEncrypted_{false};
  mutable std::mutex mutex_;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}
