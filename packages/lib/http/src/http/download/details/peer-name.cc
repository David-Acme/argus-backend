#include "peer-name.hxx"

#include <arpa/inet.h>
#include <array>
#include <memory>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace file_download::details
{
namespace
{

struct BioDeleter
{
  void operator()(BIO* bio) const { BIO_free(bio); }
};

struct X509Deleter
{
  void operator()(X509* certificate) const { X509_free(certificate); }
};

bool isIpLiteral(const std::string& host)
{
  std::array<unsigned char, sizeof(in6_addr)> address{};
  return ::inet_pton(AF_INET, host.c_str(), address.data()) == 1 ||
         ::inet_pton(AF_INET6, host.c_str(), address.data()) == 1;
}

}

std::string hostOfOrigin(std::string_view origin)
{
  const auto scheme = origin.find("://");
  auto authority = scheme == std::string_view::npos ? origin : origin.substr(scheme + 3);
  if (authority.starts_with('['))
    return std::string(authority.substr(1, authority.find(']') - 1));
  return std::string(authority.substr(0, authority.rfind(':')));
}

bool certificateNamesHost(const PeerNameInput& input)
{
  if (input.certificatePem.empty())
    return false;
  const std::unique_ptr<BIO, BioDeleter> bio(
      BIO_new_mem_buf(input.certificatePem.data(), static_cast<int>(input.certificatePem.size())));
  if (!bio)
    return false;
  const std::unique_ptr<X509, X509Deleter> certificate(
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!certificate)
    return false;
  const auto host = hostOfOrigin(input.origin);
  if (host.empty())
    return false;
  if (isIpLiteral(host))
    return X509_check_ip_asc(certificate.get(), host.c_str(), 0) == 1;
  return X509_check_host(certificate.get(), host.data(), host.size(),
                         X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
}

}
