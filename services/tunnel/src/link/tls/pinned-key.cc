#include <link/tls/pinned-key.hxx>

#include <cstdio>
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace tunnel::link
{
namespace
{
std::string derOfKey(EVP_PKEY* key)
{
  unsigned char* encoded = nullptr;
  const int length = i2d_PUBKEY(key, &encoded);
  std::string der;
  if (length > 0 && encoded != nullptr) {
    der.assign(reinterpret_cast<const char*>(encoded),
               static_cast<size_t>(length));
  }
  if (encoded != nullptr)
    OPENSSL_free(encoded);
  return der;
}
}

std::optional<std::string> subjectPublicKeyDerOfPem(std::string_view pem)
{
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr)
    return std::nullopt;
  EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (key == nullptr)
    return std::nullopt;
  std::string der = derOfKey(key);
  EVP_PKEY_free(key);
  if (der.empty())
    return std::nullopt;
  return der;
}

std::optional<std::string> subjectPublicKeyDerOfFile(const std::string& path)
{
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr)
    return std::nullopt;
  std::string contents;
  char chunk[4096];
  while (true) {
    const size_t count = std::fread(chunk, 1, sizeof(chunk), file);
    contents.append(chunk, count);
    if (count < sizeof(chunk))
      break;
  }
  std::fclose(file);
  if (contents.empty())
    return std::nullopt;
  if (const auto der = subjectPublicKeyDerOfPem(contents))
    return der;
  BIO* bio =
      BIO_new_mem_buf(contents.data(), static_cast<int>(contents.size()));
  if (bio == nullptr)
    return std::nullopt;
  X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (certificate == nullptr)
    return std::nullopt;
  EVP_PKEY* key = X509_get_pubkey(certificate);
  X509_free(certificate);
  if (key == nullptr)
    return std::nullopt;
  std::string der = derOfKey(key);
  EVP_PKEY_free(key);
  if (der.empty())
    return std::nullopt;
  return der;
}
}
