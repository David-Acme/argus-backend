#include <link/tls/tls-channel.hxx>

#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace tunnel::link
{
namespace
{
constexpr std::array<unsigned char, 13> kAlpnWire{
    12, 'a', 'r', 'g', 'u', 's', '-', 'l', 'i', 'n', 'k', '/', '2'};
constexpr std::string_view kAlpnWireName = "argus-link/2";
constexpr size_t kReadChunk = static_cast<size_t>(64) * 1024;
constexpr size_t kWriteChunk = static_cast<size_t>(16) * 1024;

std::string lastError()
{
  const unsigned long code = ERR_get_error();
  if (code == 0)
    return "no error detail";
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return buffer.data();
}

std::string subjectPublicKeyDerOf(X509* certificate)
{
  EVP_PKEY* key = X509_get_pubkey(certificate);
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

TlsChannel::TlsChannel(const Params& params)
    : loop_(*params.loop),
      fd_(openSocket(params)),
      host_(params.host),
      serverName_(params.serverName.empty() ? params.host : params.serverName),
      pinnedKey_(params.pinnedSubjectPublicKeyDer),
      sendLimit_(params.sendLimit),
      sendHardCap_(params.sendHardCap),
      callbacks_(params.callbacks)
{
  if (params.context == nullptr)
    throw std::runtime_error("TlsChannel requires a TLS context");
  ssl_ = SSL_new(params.context);
  if (ssl_ == nullptr)
    throw std::runtime_error("SSL_new failed: " + lastError());
  SSL_set_fd(ssl_, fd_.get());
  SSL_set_connect_state(ssl_);
  SSL_set_options(ssl_, SSL_OP_NO_RENEGOTIATION);
  SSL_set_verify(ssl_, SSL_VERIFY_NONE, nullptr);
  SSL_set_alpn_protos(ssl_, kAlpnWire.data(),
                      static_cast<unsigned int>(kAlpnWire.size()));
  if (!serverName_.empty())
    SSL_set_tlsext_host_name(ssl_, serverName_.c_str());
}

int TlsChannel::openSocket(const Params& params)
{
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(params.port);
  if (::inet_pton(AF_INET, params.host.c_str(), &address.sin_addr) != 1)
    throw std::runtime_error("invalid node address: " + params.host);
  const int fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    throw std::runtime_error("socket failed");
  if (params.sndBuf > 0) {
    int size = params.sndBuf;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
          0 &&
      errno != EINPROGRESS) {
    ::close(fd);
    throw std::runtime_error("connect to " + params.host + ":" +
                             std::to_string(params.port) + " failed");
  }
  return fd;
}

TlsChannel::Ptr TlsChannel::connect(const Params& params)
{
  if (params.loop == nullptr)
    throw std::runtime_error("TlsChannel requires a poll loop");
  Ptr channel = Ptr(new TlsChannel(params));
  channel->loop_.watch(channel->fd_.get(), channel);
  channel->loop_.update(
      {.fd = channel->fd_.get(),
       .events = EPOLLOUT | EPOLLRDHUP,
       .actor = channel});
  return channel;
}

TlsChannel::~TlsChannel()
{
  if (!closed_)
    loop_.unwatch(fd_.get());
  if (ssl_ != nullptr)
    SSL_free(ssl_);
}

void TlsChannel::handleEvents(uint32_t events)
{
  ++dispatchDepth_;
  dispatch(events);
  --dispatchDepth_;
  applyStaged();
}

void TlsChannel::applyStaged()
{
  if (dispatchDepth_ > 0 || !staged_)
    return;
  staged_ = false;
  callbacks_ = std::move(stagedCallbacks_);
  stagedCallbacks_ = Callbacks{};
}

void TlsChannel::dispatch(uint32_t events)
{
  if (closed_)
    return;
  if (connecting_) {
    if (events & (EPOLLOUT | EPOLLERR))
      completeConnection();
    else if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP))
      close();
    return;
  }
  if (!ready_)
    stepHandshake();
  if (closed_)
    return;
  if (ready_) {
    if (events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP))
      readAvailable();
    if (closed_)
      return;
    if (events & EPOLLOUT)
      flush();
    if (closed_)
      return;
    if (events & EPOLLERR) {
      close();
      return;
    }
  }
  updateInterest();
}

void TlsChannel::completeConnection()
{
  int error = 0;
  socklen_t length = sizeof(error);
  if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
      error != 0) {
    LOG_WARN << "argus-tunnel: cannot reach the cloud node " << serverName_
             << " on " << host_ << ":"
             << std::strerror(error == 0 ? ECONNREFUSED : error);
    close();
    return;
  }
  connecting_ = false;
  int one = 1;
  ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  stepHandshake();
  if (!closed_ && !ready_)
    updateInterest();
}

void TlsChannel::stepHandshake()
{
  const int result = SSL_do_handshake(ssl_);
  if (result != 1) {
    const int error = SSL_get_error(ssl_, result);
    if (error == SSL_ERROR_WANT_READ) {
      wantWrite_ = false;
      return;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      wantWrite_ = true;
      return;
    }
    LOG_WARN << "argus-tunnel: TLS handshake with " << serverName_
             << " failed: "
             << (error == SSL_ERROR_SSL ? lastError()
                                        : "the node closed the connection");
    close();
    return;
  }
  verifyPeer();
}

void TlsChannel::verifyPeer()
{
  const unsigned char* selected = nullptr;
  unsigned int selectedLength = 0;
  SSL_get0_alpn_selected(ssl_, &selected, &selectedLength);
  const std::string_view negotiated{
      selected == nullptr ? "" : reinterpret_cast<const char*>(selected),
      selectedLength};
  if (negotiated != kAlpnWireName) {
    LOG_WARN << "argus-tunnel: the cloud node did not select " << kAlpnWireName;
    close();
    return;
  }
  X509* certificate = SSL_get1_peer_certificate(ssl_);
  if (certificate == nullptr) {
    LOG_WARN << "argus-tunnel: the cloud node presented no certificate";
    close();
    return;
  }
  const std::string presented = subjectPublicKeyDerOf(certificate);
  X509_free(certificate);
  if (pinnedKey_.empty() || presented != pinnedKey_) {
    LOG_WARN << "argus-tunnel: the cloud node's key is not the pinned one";
    close();
    return;
  }
  ready_ = true;
  updateInterest();
  if (callbacks_.onConnected)
    callbacks_.onConnected(*this);
}

void TlsChannel::readAvailable()
{
  if (eofSeen_ || readPaused_ || closed_ || !ready_)
    return;
  std::array<char, kReadChunk> chunk{};
  while (true) {
    const int received =
        SSL_read(ssl_, chunk.data(), static_cast<int>(chunk.size()));
    if (received > 0) {
      if (callbacks_.onRead && !closeWhenFlushed_)
        callbacks_.onRead(*this, chunk.data(), static_cast<size_t>(received));
      if (closed_ || readPaused_)
        return;
      continue;
    }
    const int error = SSL_get_error(ssl_, received);
    if (error == SSL_ERROR_WANT_READ) {
      wantWrite_ = false;
      updateInterest();
      return;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
      wantWrite_ = true;
      updateInterest();
      return;
    }
    if (error == SSL_ERROR_ZERO_RETURN ||
        (error == SSL_ERROR_SYSCALL && errno == 0)) {
      eofSeen_ = true;
      if (callbacks_.onEof)
        callbacks_.onEof(*this);
      if (sentShutdown_ && !closed_)
        close();
      return;
    }
    close();
    return;
  }
}

void TlsChannel::flush()
{
  if (closed_ || !ready_)
    return;
  while (!sendBuffer_.empty()) {
    const int requested =
        static_cast<int>(std::min<size_t>(sendBuffer_.size(), kWriteChunk));
    const int written = SSL_write(ssl_, sendBuffer_.data(), requested);
    if (written > 0) {
      sendBuffer_.erase(0, static_cast<size_t>(written));
      if (sendBuffer_.empty())
        wantWrite_ = false;
      continue;
    }
    const int error = SSL_get_error(ssl_, written);
    if (error == SSL_ERROR_WANT_WRITE) {
      wantWrite_ = true;
      break;
    }
    if (error == SSL_ERROR_WANT_READ) {
      wantWrite_ = false;
      break;
    }
    close();
    return;
  }
  if (sendBuffer_.empty() && congestedNotified_) {
    congestedNotified_ = false;
    if (callbacks_.onDrained)
      callbacks_.onDrained(*this);
  }
  if (sendBuffer_.empty() && closeWhenFlushed_ && !sentShutdown_ && !closed_) {
    sentShutdown_ = true;
    SSL_shutdown(ssl_);
    if (eofSeen_)
      close();
  }
}

void TlsChannel::send(std::string_view data)
{
  if (closed_)
    return;
  sendBuffer_.append(data);
  if (ready_)
    flush();
  if (closed_)
    return;
  if (sendBuffer_.size() >= sendHardCap_) {
    close();
    return;
  }
  if (sendBuffer_.size() >= sendLimit_ && !congestedNotified_) {
    congestedNotified_ = true;
    if (callbacks_.onCongested)
      callbacks_.onCongested(*this);
  }
  updateInterest();
}

void TlsChannel::setCallbacks(Callbacks callbacks)
{
  if (dispatchDepth_ > 0) {
    stagedCallbacks_ = std::move(callbacks);
    staged_ = true;
    return;
  }
  callbacks_ = std::move(callbacks);
}

void TlsChannel::setReadPaused(bool paused)
{
  if (readPaused_ == paused || closed_)
    return;
  readPaused_ = paused;
  updateInterest();
}

void TlsChannel::closeWhenFlushed()
{
  if (closed_ || closeWhenFlushed_)
    return;
  closeWhenFlushed_ = true;
  readPaused_ = false;
  flush();
  if (!closed_)
    updateInterest();
}

void TlsChannel::close()
{
  if (closed_)
    return;
  closed_ = true;
  loop_.unwatch(fd_.get());
  fd_.reset();
  loop_.retain(sharedFromThis());
  if (callbacks_.onClosed)
    callbacks_.onClosed(*this);
}

void TlsChannel::updateInterest()
{
  if (closed_)
    return;
  loop_.update(
      {.fd = fd_.get(), .events = interestEvents(), .actor = weak_from_this()});
}

uint32_t TlsChannel::interestEvents() const
{
  if (connecting_)
    return EPOLLOUT | EPOLLRDHUP;
  uint32_t events = EPOLLRDHUP;
  if (!eofSeen_)
    events |= EPOLLIN;
  if (readPaused_)
    events &= ~static_cast<uint32_t>(EPOLLIN);
  if (!ready_ || !sendBuffer_.empty() || wantWrite_)
    events |= EPOLLOUT;
  return events;
}
}
