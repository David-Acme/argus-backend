#include "tls-hello.hxx"

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <array>
#include <memory>
#include <stdexcept>

namespace tunnel::test
{
namespace
{
struct ContextRelease
{
  void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
};

struct SslRelease
{
  void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};
}

std::string clientHelloFor(const std::string& serverName)
{
  const std::unique_ptr<SSL_CTX, ContextRelease> context{
      SSL_CTX_new(TLS_client_method())};
  if (!context)
    throw std::runtime_error("SSL_CTX_new failed");
  const std::unique_ptr<SSL, SslRelease> ssl{SSL_new(context.get())};
  if (!ssl)
    throw std::runtime_error("SSL_new failed");
  BIO* incoming = BIO_new(BIO_s_mem());
  BIO* outgoing = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(incoming, -1);
  SSL_set_bio(ssl.get(), incoming, outgoing);
  if (!serverName.empty())
    SSL_set_tlsext_host_name(ssl.get(), serverName.c_str());
  SSL_set_connect_state(ssl.get());
  SSL_do_handshake(ssl.get());
  std::string hello;
  std::array<char, 4096> chunk{};
  while (BIO_pending(outgoing) > 0) {
    const int count = BIO_read(outgoing, chunk.data(), static_cast<int>(chunk.size()));
    if (count <= 0)
      break;
    hello.append(chunk.data(), static_cast<size_t>(count));
  }
  return hello;
}
}
