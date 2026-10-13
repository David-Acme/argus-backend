#include "tls-health.hxx"

#include <memory>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tls_health
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

using Bio = std::unique_ptr<BIO, BioDeleter>;
using Certificate = std::unique_ptr<X509, X509Deleter>;

Bio readOnly(std::string_view pem)
{
  return Bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
}

std::vector<Certificate> certificatesIn(std::string_view pem)
{
  std::vector<Certificate> certificates;
  const Bio input = readOnly(pem);
  if (!input)
    return certificates;
  while (true) {
    Certificate certificate(
        PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr));
    if (!certificate)
      break;
    certificates.push_back(std::move(certificate));
  }
  ERR_clear_error();
  return certificates;
}

std::string subjectOf(const X509* certificate)
{
  const Bio output(BIO_new(BIO_s_mem()));
  if (!output ||
      X509_NAME_print_ex(output.get(), X509_get_subject_name(certificate), 0,
                         XN_FLAG_RFC2253) < 0)
    return {};
  char* data = nullptr;
  const long size = BIO_get_mem_data(output.get(), &data);
  return size > 0 ? std::string(data, static_cast<std::size_t>(size))
                  : std::string{};
}

struct Expiry
{
  int days{0};
  bool expired{true};
};

Expiry expiryOf(const X509* certificate)
{
  int days = 0;
  int seconds = 0;
  if (ASN1_TIME_diff(&days, &seconds, nullptr,
                     X509_get0_notAfter(certificate)) != 1)
    return {};
  return {.days = days, .expired = days < 0 || seconds < 0};
}

Json::Value describe(const X509* certificate)
{
  const Expiry expiry = expiryOf(certificate);
  Json::Value entry(Json::objectValue);
  entry["subject"] = subjectOf(certificate);
  entry["expiresInDays"] = expiry.days;
  entry["expired"] = expiry.expired;
  return entry;
}

std::string expiryWords(const Expiry& expiry)
{
  if (expiry.expired)
    return "has expired";
  return "expires in " + std::to_string(expiry.days) + " days";
}

const X509* issuerIn(const std::vector<Certificate>& bundle, X509* certificate)
{
  for (const auto& candidate : bundle) {
    if (X509_check_issued(candidate.get(), certificate) == X509_V_OK)
      return candidate.get();
  }
  return nullptr;
}

}

Json::Value report(const LoadedTls& loaded)
{
  Json::Value health(Json::objectValue);
  health["loaded"] = !loaded.certificatePem.empty();
  if (loaded.certificatePem.empty())
    return health;

  Json::Value warnings(Json::arrayValue);
  const auto bundle = certificatesIn(loaded.trustBundlePem);
  Json::Value trustBundle(Json::arrayValue);
  for (const auto& authority : bundle)
    trustBundle.append(describe(authority.get()));
  health["trustBundleCas"] = static_cast<Json::UInt>(bundle.size());
  health["trustBundle"] = std::move(trustBundle);

  auto leaf = certificatesIn(loaded.certificatePem);
  if (leaf.empty()) {
    warnings.append("the loaded internal certificate cannot be read");
    health["warnings"] = std::move(warnings);
    return health;
  }
  X509* certificate = leaf.front().get();
  health["certificate"] = describe(certificate);
  const Expiry certificateExpiry = expiryOf(certificate);
  if (certificateExpiry.expired || certificateExpiry.days < kWarningDays)
    warnings.append("the internal certificate " + subjectOf(certificate) + " " +
                    expiryWords(certificateExpiry) +
                    ": re-run provisioning and restart the service");

  const X509* issuer = issuerIn(bundle, certificate);
  if (issuer == nullptr) {
    warnings.append("the issuer of the internal certificate " +
                    subjectOf(certificate) +
                    " is not in the loaded trust bundle");
  }
  else {
    health["issuingCa"] = describe(issuer);
    const Expiry issuerExpiry = expiryOf(issuer);
    if (issuerExpiry.expired || issuerExpiry.days < kWarningDays)
      warnings.append("the internal CA " + subjectOf(issuer) + " " +
                      expiryWords(issuerExpiry) +
                      ": re-run provisioning, which rotates it, and restart "
                      "the service");
  }
  health["warnings"] = std::move(warnings);
  return health;
}

}
