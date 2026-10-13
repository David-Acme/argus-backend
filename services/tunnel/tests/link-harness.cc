#include "link-harness.hxx"

#include <link/tls/pinned-key.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace tunnel::test
{
namespace
{
constexpr int kConnectTimeoutMs = 4000;
constexpr int kReadChunk = 16 * 1024;
constexpr std::string_view kArgusLinkV2 = "argus-link/2";

std::string lastError()
{
  const unsigned long code = ERR_get_error();
  if (code == 0)
    return "no error detail";
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return buffer.data();
}

int listenLoopback(uint16_t& port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("socket failed");
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ::close(fd);
    throw std::runtime_error("bind failed");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0 ||
      ::listen(fd, 8) < 0) {
    ::close(fd);
    throw std::runtime_error("listen failed");
  }
  port = ntohs(address.sin_port);
  return fd;
}

int acceptWithTimeout(int listenFd, int timeoutMs)
{
  pollfd descriptor{};
  descriptor.fd = listenFd;
  descriptor.events = POLLIN;
  const int ready = ::poll(&descriptor, 1, timeoutMs);
  if (ready <= 0)
    return -1;
  return ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
}

std::string commonNameOf(X509* certificate)
{
  X509_NAME* subject = X509_get_subject_name(certificate);
  if (subject == nullptr)
    return {};
  const int index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  if (index < 0)
    return {};
  X509_NAME_ENTRY* entry = X509_NAME_get_entry(subject, index);
  if (entry == nullptr)
    return {};
  ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
  if (data == nullptr)
    return {};
  const unsigned char* bytes = ASN1_STRING_get0_data(data);
  const int length = ASN1_STRING_length(data);
  if (bytes == nullptr || length <= 0)
    return {};
  return std::string{reinterpret_cast<const char*>(bytes),
                     static_cast<size_t>(length)};
}

int selectAlpn(SSL*, const unsigned char** out, unsigned char* outlen,
               const unsigned char* in, unsigned int inlen, void*)
{
  static constexpr unsigned char kWire[] = {12, 'a', 'r', 'g', 'u', 's', '-',
                                            'l', 'i', 'n', 'k', '/', '2'};
  if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, kWire,
                            sizeof(kWire), in, inlen) !=
      OPENSSL_NPN_NEGOTIATED) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  return SSL_TLSEXT_ERR_OK;
}

}

FakeNodeOptions nodeOptionsFrom(const LinkFixtureOptions& options);

bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return predicate();
}

EchoServer::EchoServer()
{
  listenFd_ = listenLoopback(port_);
  worker_ = std::jthread([this] { serve(); });
}

EchoServer::~EchoServer()
{
  stop();
}

void EchoServer::stop()
{
  if (stopped_.exchange(true))
    return;
  if (listenFd_ >= 0)
    ::shutdown(listenFd_, SHUT_RDWR);
  if (worker_.joinable())
    worker_.join();
  if (listenFd_ >= 0) {
    ::close(listenFd_);
    listenFd_ = -1;
  }
}

void EchoServer::serve()
{
  std::array<char, kReadChunk> chunk{};
  while (!stopped_.load()) {
    const int fd = acceptWithTimeout(listenFd_, 200);
    if (fd < 0)
      continue;
    accepted_.fetch_add(1);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200 * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (!stopped_.load()) {
      const ssize_t count = ::recv(fd, chunk.data(), chunk.size(), 0);
      if (count > 0) {
        received_.fetch_add(static_cast<size_t>(count));
        ssize_t written = 0;
        while (written < count) {
          const ssize_t sent =
              ::send(fd, chunk.data() + written,
                     static_cast<size_t>(count - written), MSG_NOSIGNAL);
          if (sent <= 0)
            break;
          written += sent;
        }
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                        errno == EINTR))
        continue;
      break;
    }
    ::close(fd);
  }
}

FakeNode::FakeNode(FakeNodeOptions options) : options_(std::move(options))
{
  listenFd_ = listenLoopback(port_);
  worker_ = std::jthread([this] { serve(); });
}

FakeNode::~FakeNode()
{
  stop();
}

void FakeNode::stop()
{
  if (stopped_.exchange(true))
    return;
  {
    std::lock_guard<std::mutex> lock{mutex_};
    if (ssl_ != nullptr)
      ::shutdown(SSL_get_fd(ssl_), SHUT_RDWR);
  }
  if (listenFd_ >= 0)
    ::shutdown(listenFd_, SHUT_RDWR);
  if (worker_.joinable())
    worker_.join();
  if (listenFd_ >= 0) {
    ::close(listenFd_);
    listenFd_ = -1;
  }
}

void FakeNode::serve()
{
  SSL_CTX* context = SSL_CTX_new(TLS_server_method());
  if (context == nullptr) {
    std::lock_guard<std::mutex> lock{mutex_};
    state_.lastError = "SSL_CTX_new failed: " + lastError();
    return;
  }
  SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION);
  SSL_CTX_set_alpn_select_cb(context, selectAlpn, nullptr);
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) {
    return 1;
  });
  if (SSL_CTX_use_certificate_chain_file(
          context, options_.certificatePath.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, options_.keyPath.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    std::lock_guard<std::mutex> lock{mutex_};
    state_.lastError = "cannot load the node certificate: " + lastError();
    SSL_CTX_free(context);
    return;
  }
  while (!stopped_.load()) {
    const int fd = acceptWithTimeout(listenFd_, 200);
    if (fd < 0)
      continue;
    connections_.fetch_add(1);
    SSL* ssl = SSL_new(context);
    if (ssl == nullptr)
      continue;
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500 * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    bool handshaken = false;
    while (!stopped_.load()) {
      const int result = SSL_accept(ssl);
      if (result == 1) {
        handshaken = true;
        break;
      }
      const int error = SSL_get_error(ssl, result);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        continue;
      {
        std::lock_guard<std::mutex> lock{mutex_};
        state_.lastError = "the home handshake failed: " + lastError();
      }
      break;
    }
    if (!handshaken) {
      SSL_free(ssl);
      ::close(fd);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock{mutex_};
      ssl_ = ssl;
      state_ = State{};
      frames_.clear();
      state_.handshaken = true;
      if (X509* peer = SSL_get1_peer_certificate(ssl)) {
        state_.peerCommonName = commonNameOf(peer);
        X509_free(peer);
      }
    }
    if (options_.sendHello)
      sendHello();
    readLoop(ssl);
    {
      std::lock_guard<std::mutex> lock{mutex_};
      if (ssl_ == ssl)
        ssl_ = nullptr;
    }
    SSL_free(ssl);
    ::close(fd);
    if (!options_.acceptReconnects)
      break;
  }
  SSL_CTX_free(context);
}

bool FakeNode::sendHello()
{
  return sendFrame(link::encodeHello(options_.capabilities));
}

void FakeNode::closeConnection()
{
  std::lock_guard<std::mutex> lock{mutex_};
  if (ssl_ != nullptr)
    ::shutdown(SSL_get_fd(ssl_), SHUT_RDWR);
}

bool FakeNode::sendFrame(std::string_view frame)
{
  std::lock_guard<std::mutex> lock{mutex_};
  if (ssl_ == nullptr || stopped_.load())
    return false;
  size_t offset = 0;
  while (offset < frame.size()) {
    const int written =
        SSL_write(ssl_, frame.data() + offset,
                  static_cast<int>(frame.size() - offset));
    if (written <= 0)
      return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

void FakeNode::readLoop(SSL* ssl)
{
  link::FrameParser parser;
  std::array<char, kReadChunk> chunk{};
  while (!stopped_.load()) {
    const int received =
        SSL_read(ssl, chunk.data(), static_cast<int>(chunk.size()));
    if (received > 0) {
      parser.feed(
          std::string_view{chunk.data(), static_cast<size_t>(received)});
      if (parser.failed()) {
        std::lock_guard<std::mutex> lock{mutex_};
        state_.violations.push_back(parser.error());
        break;
      }
      while (parser.hasFrame())
        handleFrame(ssl, parser.popFrame());
      continue;
    }
    const int error = SSL_get_error(ssl, received);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
      continue;
    break;
  }
}

void FakeNode::handleFrame(SSL* ssl, const link::Frame& frame)
{
  (void)ssl;
  if (frame.type == link::FrameType::Ping && options_.answerPings)
    (void)sendFrame(link::encodePong(link::decodeNonce(frame)));
  std::lock_guard<std::mutex> lock{mutex_};
  state_.helloSeen = state_.helloSeen ||
                     frame.type == link::FrameType::Hello;
  frames_.push_back(frame);
}

std::optional<link::Frame> FakeNode::nextFrame(int timeoutMs)
{
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
  std::unique_lock<std::mutex> lock{mutex_};
  while (frames_.empty()) {
    if (stopped_.load() || std::chrono::steady_clock::now() >= deadline)
      return std::nullopt;
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    lock.lock();
  }
  link::Frame frame = std::move(frames_.front());
  frames_.pop_front();
  return frame;
}

FakeNode::State FakeNode::state() const
{
  std::lock_guard<std::mutex> lock{mutex_};
  return state_;
}

FakeNodeOptions nodeOptionsFrom(const LinkFixtureOptions& options)
{
  FakeNodeOptions node;
  node.certificatePath = options.node.certificatePath;
  node.keyPath = options.node.keyPath;
  node.capabilities = options.nodeOptions.capabilities;
  node.sendHello = options.nodeOptions.sendHello;
  node.answerPings = options.nodeOptions.answerPings;
  node.acceptReconnects = options.nodeOptions.acceptReconnects;
  return node;
}

LinkFixture::LinkFixture(LinkFixtureOptions options)
    : options_(std::move(options)),
      node_(nodeOptionsFrom(options_))
{
  tunnel::link::LinkOptions link = options_.link;
  link.host = "127.0.0.1";
  link.port = node_.port();
  link.certificatePath = options_.home.certificatePath;
  link.keyPath = options_.home.keyPath;
  link.pinnedSubjectPublicKeyDer =
      options_.pinnedSubjectPublicKeyDer.empty()
          ? tunnel::link::subjectPublicKeyDerOfPem(options_.node.publicKeyPem)
                .value_or(std::string{})
          : options_.pinnedSubjectPublicKeyDer;
  if (link.gatewayHost.empty())
    link.gatewayHost = "127.0.0.1";
  link.gatewayPort =
      options_.gatewayPort == 0 ? echo_.port() : options_.gatewayPort;
  const tunnel::link::LinkClient::Deps deps{.loop = loop_,
                                            .options = link,
                                            .observer = options_.observer};
  client_ = std::make_unique<tunnel::link::LinkClient>(deps);
  loopThread_ = std::thread([this] { loop_.run(); });
  client_->start();
}

LinkFixture::~LinkFixture()
{
  if (client_ != nullptr)
    client_->stop();
  loop_.stop();
  if (loopThread_.joinable())
    loopThread_.join();
  client_.reset();
  node_.stop();
  echo_.stop();
}

bool LinkFixture::waitConnected(int timeoutMs)
{
  if (!waitFor([this] { return client_->connected(); }, timeoutMs))
    return false;
  drainFrames();
  return true;
}

void LinkFixture::drainFrames()
{
  while (const auto frame = node_.nextFrame(20)) {
    const tunnel::link::Frame current =
        frame.value_or(tunnel::link::Frame{});
    if (current.type == tunnel::link::FrameType::Hello && !hello_)
      hello_ = current;
  }
}

tunnel::link::Frame LinkFixture::awaitFrame(int timeoutMs)
{
  const auto frame = node_.nextFrame(timeoutMs);
  if (!frame)
    throw std::runtime_error("no frame arrived from the home client");
  return *frame;
}
}
