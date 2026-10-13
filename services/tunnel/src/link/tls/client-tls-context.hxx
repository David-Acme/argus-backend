#pragma once

#include <string>

#include <openssl/ssl.h>

namespace tunnel::link
{
class ClientTlsContext
{
public:
  struct Params
  {
    std::string certificatePath;
    std::string keyPath;
  };

  explicit ClientTlsContext(const Params& params);
  ~ClientTlsContext();
  ClientTlsContext(const ClientTlsContext&) = delete;
  ClientTlsContext& operator=(const ClientTlsContext&) = delete;

  [[nodiscard]] SSL_CTX* handle() const { return context_; }

private:
  SSL_CTX* context_{nullptr};
};
}
