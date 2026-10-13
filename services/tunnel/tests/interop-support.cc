#include "interop-support.hxx"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace tunnel::test
{
namespace
{
constexpr int kReadChunk = 16 * 1024;
constexpr int kAcceptTimeoutMs = 200;

std::string sslError()
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
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      ::listen(fd, 8) < 0) {
    ::close(fd);
    throw std::runtime_error("cannot listen on the loopback");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    ::close(fd);
    throw std::runtime_error("getsockname failed");
  }
  port = ntohs(address.sin_port);
  return fd;
}

std::string spkiOf(const std::string& certificatePath)
{
  BIO* bio = BIO_new_file(certificatePath.c_str(), "r");
  if (bio == nullptr)
    return {};
  X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (certificate == nullptr)
    return {};
  EVP_PKEY* key = X509_get_pubkey(certificate);
  X509_free(certificate);
  if (key == nullptr)
    return {};
  unsigned char* encoded = nullptr;
  const int length = i2d_PUBKEY(key, &encoded);
  std::string der;
  if (length > 0 && encoded != nullptr) {
    der.assign(reinterpret_cast<const char*>(encoded),
               static_cast<size_t>(length));
  }
  if (encoded != nullptr)
    OPENSSL_free(encoded);
  EVP_PKEY_free(key);
  return der;
}
}

ChildProcess::~ChildProcess()
{
  terminate();
}

bool ChildProcess::spawn(const std::vector<std::string>& arguments)
{
  if (arguments.empty())
    return false;
  const pid_t child = ::fork();
  if (child < 0)
    return false;
  if (child == 0) {
    ::setsid();
    ::unsetenv("ARGUS_CLOUD_DSN");
    ::unsetenv("ARGUS_CLOUD_TEST_DSN");
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments)
      argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    ::_exit(127);
  }
  pid_ = child;
  return true;
}

void ChildProcess::requestStop()
{
  if (pid_ >= 0)
    ::kill(pid_, SIGTERM);
}

bool ChildProcess::running() const
{
  if (pid_ < 0)
    return false;
  int status = 0;
  return ::waitpid(pid_, &status, WNOHANG) == 0;
}

void ChildProcess::terminate()
{
  if (pid_ < 0)
    return;
  ::kill(pid_, SIGTERM);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  ::kill(pid_, SIGKILL);
  int status = 0;
  (void)::waitpid(pid_, &status, 0);
  pid_ = -1;
}

HomeTlsServer::HomeTlsServer(const Params& params) : params_(params)
{
  listenFd_ = listenLoopback(port_);
  worker_ = std::jthread([this] { serve(); });
}

HomeTlsServer::~HomeTlsServer()
{
  stop();
}

void HomeTlsServer::stop()
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

void HomeTlsServer::serve()
{
  SSL_CTX* context = SSL_CTX_new(TLS_server_method());
  if (context == nullptr)
    return;
  SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
  if (SSL_CTX_use_certificate_chain_file(
          context, params_.certificatePath.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, params_.keyPath.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(context);
    return;
  }
  std::array<char, kReadChunk> chunk{};
  while (!stopped_.load()) {
    pollfd descriptor{};
    descriptor.fd = listenFd_;
    descriptor.events = POLLIN;
    if (::poll(&descriptor, 1, kAcceptTimeoutMs) <= 0)
      continue;
    const int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0)
      continue;
    accepted_.fetch_add(1);
    SSL* ssl = SSL_new(context);
    if (ssl == nullptr) {
      ::close(fd);
      continue;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500L * 1000;
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
      break;
    }
    if (handshaken) {
      while (!stopped_.load()) {
        const int received =
            SSL_read(ssl, chunk.data(), static_cast<int>(chunk.size()));
        if (received > 0) {
          int written = 0;
          while (written < received) {
            const int sent = SSL_write(ssl, chunk.data() + written,
                                       received - written);
            if (sent <= 0)
              break;
            written += sent;
          }
          echoed_.fetch_add(static_cast<size_t>(received));
          continue;
        }
        const int error = SSL_get_error(ssl, received);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
          continue;
        break;
      }
    }
    SSL_free(ssl);
    ::close(fd);
  }
  SSL_CTX_free(context);
}

TunnelApp::TunnelApp(const Params& params)
{
  fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0)
    throw std::runtime_error("socket failed");
  timeval timeout{};
  timeout.tv_sec = params.timeoutMs / 1000;
  timeout.tv_usec = static_cast<long>(params.timeoutMs % 1000) * 1000L;
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(params.port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
      0) {
    ::close(fd_);
    fd_ = -1;
    lastError_ = "cannot connect to the relay device port";
    return;
  }
  SSL_CTX* context = SSL_CTX_new(TLS_client_method());
  if (context == nullptr) {
    lastError_ = "SSL_CTX_new failed";
    return;
  }
  context_ = context;
  SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
  SSL* ssl = SSL_new(context);
  if (ssl == nullptr) {
    lastError_ = "SSL_new failed";
    return;
  }
  ssl_ = ssl;
  SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
  SSL_set_fd(ssl, fd_);
  SSL_set_tlsext_host_name(ssl, params.serverName.c_str());
  if (SSL_connect(ssl) != 1) {
    lastError_ = "the app handshake failed: " + sslError();
    return;
  }
  const std::string pinned = spkiOf(params.pinnedCertificatePath);
  X509* peer = SSL_get1_peer_certificate(ssl);
  if (peer == nullptr) {
    lastError_ = "the home presented no certificate";
    return;
  }
  EVP_PKEY* key = X509_get_pubkey(peer);
  unsigned char* encoded = nullptr;
  const int length = key == nullptr ? 0 : i2d_PUBKEY(key, &encoded);
  std::string presented;
  if (length > 0 && encoded != nullptr) {
    presented.assign(reinterpret_cast<const char*>(encoded),
                     static_cast<size_t>(length));
  }
  if (encoded != nullptr)
    OPENSSL_free(encoded);
  if (key != nullptr)
    EVP_PKEY_free(key);
  X509_free(peer);
  handshaken_ = !pinned.empty() && presented == pinned;
  if (!handshaken_)
    lastError_ = "the certificate is not the pinned one";
}

TunnelApp::~TunnelApp()
{
  if (ssl_ != nullptr)
    SSL_free(static_cast<SSL*>(ssl_));
  if (context_ != nullptr)
    SSL_CTX_free(static_cast<SSL_CTX*>(context_));
  if (fd_ >= 0)
    ::close(fd_);
}

bool TunnelApp::write(std::string_view bytes)
{
  auto* ssl = static_cast<SSL*>(ssl_);
  if (ssl == nullptr)
    return false;
  size_t offset = 0;
  while (offset < bytes.size()) {
    const int written = SSL_write(ssl, bytes.data() + offset,
                                  static_cast<int>(bytes.size() - offset));
    if (written <= 0)
      return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

std::string TunnelApp::read(size_t bytes, std::chrono::milliseconds timeout)
{
  auto* ssl = static_cast<SSL*>(ssl_);
  std::string received;
  received.reserve(bytes);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (ssl != nullptr && received.size() < bytes) {
    std::array<char, kReadChunk> chunk{};
    const int count =
        SSL_read(ssl, chunk.data(), static_cast<int>(chunk.size()));
    if (count > 0) {
      received.append(chunk.data(), static_cast<size_t>(count));
      continue;
    }
    const int error = SSL_get_error(ssl, count);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
      break;
    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }
  return received;
}

std::string TunnelApp::request(const RequestInput& input)
{
  if (!write(input.bytes))
    return {};
  return read(input.expected, input.timeout);
}

uint16_t freeLoopbackPort()
{
  uint16_t port = 0;
  const int fd = listenLoopback(port);
  ::close(fd);
  if (port == 0)
    throw std::runtime_error("no loopback port is free");
  return port;
}

bool writeFile(const std::filesystem::path& path, std::string_view text)
{
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!stream.good())
    return false;
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  return stream.good();
}

bool waitForPort(uint16_t port, std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool open =
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        0;
    ::close(fd);
    if (open)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }
  return false;
}
}
