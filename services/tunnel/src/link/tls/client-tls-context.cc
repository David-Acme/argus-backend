#include <link/tls/client-tls-context.hxx>

#include <array>
#include <stdexcept>
#include <string>

#include <openssl/err.h>

namespace tunnel::link
{
namespace
{
std::string lastError()
{
  const unsigned long code = ERR_get_error();
  if (code == 0)
    return "no error detail";
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return buffer.data();
}
}

ClientTlsContext::ClientTlsContext(const Params& params)
{
  context_ = SSL_CTX_new(TLS_client_method());
  if (context_ == nullptr)
    throw std::runtime_error("SSL_CTX_new failed: " + lastError());
  SSL_CTX_set_min_proto_version(context_, TLS1_3_VERSION);
  SSL_CTX_set_options(context_, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
  SSL_CTX_set_mode(context_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                                 SSL_MODE_ENABLE_PARTIAL_WRITE |
                                 SSL_MODE_RELEASE_BUFFERS);
  SSL_CTX_set_session_cache_mode(context_, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);

  if (SSL_CTX_use_certificate_chain_file(context_,
                                         params.certificatePath.c_str()) != 1) {
    const std::string detail = lastError();
    SSL_CTX_free(context_);
    context_ = nullptr;
    throw std::runtime_error("cannot load the home certificate " +
                             params.certificatePath + ": " + detail);
  }
  if (SSL_CTX_use_PrivateKey_file(context_, params.keyPath.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    const std::string detail = lastError();
    SSL_CTX_free(context_);
    context_ = nullptr;
    throw std::runtime_error("cannot load the home private key " +
                             params.keyPath + ": " + detail);
  }
  if (SSL_CTX_check_private_key(context_) != 1) {
    SSL_CTX_free(context_);
    context_ = nullptr;
    throw std::runtime_error("the certificate and the private key of " +
                             params.certificatePath + " do not match");
  }
}

ClientTlsContext::~ClientTlsContext()
{
  if (context_ != nullptr)
    SSL_CTX_free(context_);
}
}
