#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <http/certificate-reload.hxx>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string>
#include <unistd.h>

namespace
{

struct KeyDeleter
{
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

struct X509Deleter
{
  void operator()(X509* certificate) const { X509_free(certificate); }
};

struct FileCloser
{
  void operator()(std::FILE* file) const { std::fclose(file); }
};

using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;

Key newKey()
{
  return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
}

std::string tempPath(const std::string& stem)
{
  return (std::filesystem::temp_directory_path() /
          (stem + "-" + std::to_string(::getpid()) + ".pem"))
      .string();
}

void writeKey(const std::string& path, EVP_PKEY* key)
{
  const std::unique_ptr<std::FILE, FileCloser> file(std::fopen(path.c_str(), "wb"));
  REQUIRE(file);
  REQUIRE(PEM_write_PrivateKey(file.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
}

void writeCertificate(const std::string& path, EVP_PKEY* key)
{
  const std::unique_ptr<X509, X509Deleter> certificate(X509_new());
  REQUIRE(certificate);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
  X509_set_pubkey(certificate.get(), key);
  X509_NAME* name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("argus.local"), -1, -1, 0);
  X509_set_issuer_name(certificate.get(), name);
  REQUIRE(X509_sign(certificate.get(), key, EVP_sha256()) > 0);
  const std::unique_ptr<std::FILE, FileCloser> file(std::fopen(path.c_str(), "wb"));
  REQUIRE(file);
  REQUIRE(PEM_write_X509(file.get(), certificate.get()) == 1);
}

}

TEST_CASE("a rotated certificate is reloaded only when it pairs with its key")
{
  const std::string certPath = tempPath("certificate-reload-cert");
  const std::string keyPath = tempPath("certificate-reload-key");
  const std::string otherKeyPath = tempPath("certificate-reload-other");
  const auto key = newKey();
  const auto otherKey = newKey();
  REQUIRE(key);
  REQUIRE(otherKey);
  writeKey(keyPath, key.get());
  writeKey(otherKeyPath, otherKey.get());
  writeCertificate(certPath, key.get());

  CHECK(certificate_reload::usablePair({.certPath = certPath, .keyPath = keyPath}));
  CHECK_FALSE(certificate_reload::usablePair({.certPath = certPath, .keyPath = otherKeyPath}));
  CHECK_FALSE(certificate_reload::usablePair({.certPath = certPath, .keyPath = "/nonexistent/key.pem"}));

  std::ofstream(certPath, std::ios::trunc) << "not a certificate";
  CHECK_FALSE(certificate_reload::usablePair({.certPath = certPath, .keyPath = keyPath}));

  std::filesystem::remove(certPath);
  std::filesystem::remove(keyPath);
  std::filesystem::remove(otherKeyPath);
}
