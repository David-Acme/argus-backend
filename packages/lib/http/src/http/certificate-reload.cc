#include "certificate-reload.hxx"

#include <chrono>
#include <drogon/drogon.h>
#include <filesystem>
#include <memory>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <optional>
#include <utility>

namespace certificate_reload
{
namespace
{

constexpr double kCheckSeconds = 600.0;

struct BioDeleter
{
  void operator()(BIO* bio) const { BIO_free(bio); }
};

struct X509Deleter
{
  void operator()(X509* certificate) const { X509_free(certificate); }
};

struct KeyDeleter
{
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

using File = std::unique_ptr<BIO, BioDeleter>;

File openRead(const std::string& path)
{
  return File(BIO_new_file(path.c_str(), "rb"));
}

using Stamp = std::pair<std::filesystem::file_time_type, std::filesystem::file_time_type>;

std::optional<Stamp> stampOf(const CertificatePair& pair)
{
  std::error_code certError;
  std::error_code keyError;
  const auto cert = std::filesystem::last_write_time(pair.certPath, certError);
  const auto key = std::filesystem::last_write_time(pair.keyPath, keyError);
  if (certError || keyError)
    return std::nullopt;
  return Stamp{cert, key};
}

}

bool usablePair(const CertificatePair& pair)
{
  const File certFile = openRead(pair.certPath);
  const File keyFile = openRead(pair.keyPath);
  if (!certFile || !keyFile)
    return false;
  const std::unique_ptr<X509, X509Deleter> certificate(
      PEM_read_bio_X509(certFile.get(), nullptr, nullptr, nullptr));
  const std::unique_ptr<EVP_PKEY, KeyDeleter> key(
      PEM_read_bio_PrivateKey(keyFile.get(), nullptr, nullptr, nullptr));
  if (!certificate || !key)
    return false;
  return X509_check_private_key(certificate.get(), key.get()) == 1 &&
         X509_cmp_current_time(X509_get0_notAfter(certificate.get())) > 0;
}

void watch(const ListenerConfig& listener)
{
  if (!listener.tls || listener.certPath.empty() || listener.keyPath.empty())
    return;
  struct WatchState
  {
    CertificatePair pair;
    std::optional<Stamp> stamp;
  };
  auto state = std::make_shared<WatchState>();
  state->pair = {.certPath = listener.certPath, .keyPath = listener.keyPath};
  state->stamp = stampOf(state->pair);
  drogon::app().getLoop()->runEvery(kCheckSeconds, [state] {
    const auto stamp = stampOf(state->pair);
    if (!stamp || stamp == state->stamp)
      return;
    if (!usablePair(state->pair)) {
      LOG_WARN << "TLS certificate at " << state->pair.certPath
               << " changed but does not pair with its key yet; keeping the "
                  "loaded one";
      return;
    }
    state->stamp = stamp;
    drogon::app().reloadSSLFiles();
    LOG_INFO << "TLS certificate reloaded from " << state->pair.certPath;
  });
}

}
