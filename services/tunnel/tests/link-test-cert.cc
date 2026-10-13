#include "link-test-cert.hxx"

#include <chrono>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace tunnel::test
{
namespace
{
std::string writeTo(const std::filesystem::path& path, std::string_view text)
{
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!stream.good())
    throw std::runtime_error("cannot write " + path.string());
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.close();
  return path.string();
}

std::string pemOf(X509* certificate)
{
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr)
    throw std::runtime_error("BIO_new failed");
  PEM_write_bio_X509(bio, certificate);
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio, &data);
  std::string pem{data, static_cast<size_t>(size)};
  BIO_free(bio);
  return pem;
}

std::string pemOf(EVP_PKEY* key)
{
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr)
    throw std::runtime_error("BIO_new failed");
  PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio, &data);
  std::string pem{data, static_cast<size_t>(size)};
  BIO_free(bio);
  return pem;
}

std::string publicPemOf(EVP_PKEY* key)
{
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr)
    throw std::runtime_error("BIO_new failed");
  PEM_write_bio_PUBKEY(bio, key);
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio, &data);
  std::string pem{data, static_cast<size_t>(size)};
  BIO_free(bio);
  return pem;
}
}

std::filesystem::path makeTempDirectory(const std::string& prefix)
{
  std::random_device device;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      (prefix + "-" + std::to_string(stamp) + "-" + std::to_string(device()));
  std::filesystem::create_directories(directory);
  return directory;
}

TestIdentity mintIdentity(const IdentityInput& input)
{
  EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
  if (key == nullptr)
    throw std::runtime_error("cannot generate an Ed25519 key");
  X509* certificate = X509_new();
  if (certificate == nullptr) {
    EVP_PKEY_free(key);
    throw std::runtime_error("X509_new failed");
  }
  X509_set_version(certificate, 2);
  std::random_device device;
  ASN1_INTEGER_set(X509_get_serialNumber(certificate),
                   static_cast<long>(device() & 0x7FFFFFFFU));
  X509_gmtime_adj(X509_getm_notBefore(certificate), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
  X509_set_pubkey(certificate, key);
  X509_NAME* subject = X509_get_subject_name(certificate);
  X509_NAME_add_entry_by_txt(
      subject, "CN", MBSTRING_ASC,
      reinterpret_cast<const unsigned char*>(input.commonName.c_str()), -1, -1,
      0);
  X509_set_issuer_name(certificate, subject);
  if (X509_sign(certificate, key, nullptr) == 0) {
    X509_free(certificate);
    EVP_PKEY_free(key);
    throw std::runtime_error("X509_sign failed");
  }

  TestIdentity identity;
  identity.commonName = input.commonName;
  identity.certificatePath =
      writeTo(input.directory / (input.name + ".crt"), pemOf(certificate));
  identity.keyPath =
      writeTo(input.directory / (input.name + ".key"), pemOf(key));
  identity.publicKeyPem = publicPemOf(key);
  X509_free(certificate);
  EVP_PKEY_free(key);
  return identity;
}
}
