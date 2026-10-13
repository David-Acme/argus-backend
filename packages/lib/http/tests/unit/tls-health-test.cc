#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <http/tls-health.hxx>
#include <memory>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

constexpr long kDay = 86400;

struct KeyDeleter
{
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

struct X509Deleter
{
  void operator()(X509* certificate) const { X509_free(certificate); }
};

struct BioDeleter
{
  void operator()(BIO* bio) const { BIO_free(bio); }
};

using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
using Certificate = std::unique_ptr<X509, X509Deleter>;

Key newKey()
{
  Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
  if (!key)
    throw std::runtime_error("no test key");
  return key;
}

struct Signer
{
  X509* certificate;
  EVP_PKEY* key;
};

struct IssueInput
{
  std::string commonName;
  long startSeconds;
  long endSeconds;
  bool authority;
  EVP_PKEY* subjectKey;
  Signer signer;
};

void addExtension(X509* certificate, X509* issuer, int nid, const char* value)
{
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension =
      X509V3_EXT_conf_nid(nullptr, &context, nid, value);
  if (extension == nullptr)
    throw std::runtime_error("no test extension");
  X509_add_ext(certificate, extension, -1);
  X509_EXTENSION_free(extension);
}

Certificate issue(const IssueInput& input)
{
  Certificate certificate(X509_new());
  if (!certificate)
    throw std::runtime_error("no test certificate");
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 7);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), input.startSeconds);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), input.endSeconds);
  X509_set_pubkey(certificate.get(), input.subjectKey);
  X509_NAME* name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(
                                 input.commonName.c_str()),
                             -1, -1, 0);
  X509* issuer = input.signer.certificate == nullptr ? certificate.get()
                                                     : input.signer.certificate;
  X509_set_issuer_name(certificate.get(), X509_get_subject_name(issuer));
  addExtension(certificate.get(), issuer, NID_basic_constraints,
               input.authority ? "critical,CA:TRUE" : "critical,CA:FALSE");
  addExtension(certificate.get(), issuer, NID_key_usage,
               input.authority ? "critical,keyCertSign,cRLSign"
                               : "critical,digitalSignature");
  if (X509_sign(certificate.get(), input.signer.key, EVP_sha256()) <= 0)
    throw std::runtime_error("the test certificate was not signed");
  return certificate;
}

std::string pemOf(X509* certificate)
{
  const std::unique_ptr<BIO, BioDeleter> output(BIO_new(BIO_s_mem()));
  if (!output || PEM_write_bio_X509(output.get(), certificate) != 1)
    throw std::runtime_error("no test PEM");
  char* data = nullptr;
  const long size = BIO_get_mem_data(output.get(), &data);
  return {data, static_cast<std::size_t>(size)};
}

struct Authority
{
  Key key;
  Certificate certificate;
};

Authority authority(const std::string& name, long endSeconds)
{
  Key key = newKey();
  Certificate certificate =
      issue({.commonName = name,
             .startSeconds = 0,
             .endSeconds = endSeconds,
             .authority = true,
             .subjectKey = key.get(),
             .signer = {.certificate = nullptr, .key = key.get()}});
  return {.key = std::move(key), .certificate = std::move(certificate)};
}

std::string leafOf(const Authority& issuer, long endSeconds)
{
  const Key key = newKey();
  const Certificate certificate =
      issue({.commonName = "argus-auth",
             .startSeconds = -kDay,
             .endSeconds = endSeconds,
             .authority = false,
             .subjectKey = key.get(),
             .signer = {.certificate = issuer.certificate.get(),
                        .key = issuer.key.get()}});
  return pemOf(certificate.get());
}

std::string bundleOf(const Authority& first)
{
  return pemOf(first.certificate.get());
}

std::string bundleOf(const Authority& first, const Authority& second)
{
  return pemOf(first.certificate.get()) + pemOf(second.certificate.get());
}

int warningCount(const Json::Value& health)
{
  return static_cast<int>(health["warnings"].size());
}

bool warns(const Json::Value& health, const std::string& words)
{
  for (const auto& warning : health["warnings"]) {
    if (warning.asString().find(words) != std::string::npos)
      return true;
  }
  return false;
}

}

TEST_CASE("a service that loaded nothing reports so and nothing else")
{
  const auto health =
      tls_health::report({.certificatePem = "", .trustBundlePem = ""});
  CHECK_FALSE(health["loaded"].asBool());
  CHECK_FALSE(health.isMember("trustBundleCas"));
  CHECK_FALSE(health.isMember("warnings"));
}

TEST_CASE("a fresh certificate under a fresh CA reports one CA and no warning")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto health =
      tls_health::report({.certificatePem = leafOf(current, 730 * kDay),
                          .trustBundlePem = bundleOf(current)});
  CHECK(health["loaded"].asBool());
  CHECK(health["trustBundleCas"].asUInt() == 1);
  CHECK(health["certificate"]["subject"].asString() == "CN=argus-auth");
  CHECK(health["certificate"]["expiresInDays"].asInt() >= 729);
  CHECK(health["certificate"]["expiresInDays"].asInt() <= 730);
  CHECK_FALSE(health["certificate"]["expired"].asBool());
  CHECK(health["issuingCa"]["subject"].asString() ==
        "CN=Argus internal CA current");
  CHECK(health["issuingCa"]["expiresInDays"].asInt() >= 3649);
  CHECK(warningCount(health) == 0);
}

TEST_CASE("the trust bundle reports every CA it holds")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto next = authority("Argus internal CA next", 3650 * kDay);
  const auto health =
      tls_health::report({.certificatePem = leafOf(current, 730 * kDay),
                          .trustBundlePem = bundleOf(current, next)});
  CHECK(health["trustBundleCas"].asUInt() == 2);
  CHECK(health["trustBundle"].size() == 2);
  CHECK(health["trustBundle"][1]["subject"].asString() ==
        "CN=Argus internal CA next");
  CHECK(health["issuingCa"]["subject"].asString() ==
        "CN=Argus internal CA current");
  CHECK(warningCount(health) == 0);
}

TEST_CASE("a certificate warns under 30 days left and not at 31")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto soon =
      tls_health::report({.certificatePem = leafOf(current, 29 * kDay + 3600),
                          .trustBundlePem = bundleOf(current)});
  CHECK(soon["certificate"]["expiresInDays"].asInt() == 29);
  CHECK(warningCount(soon) == 1);
  CHECK(
      warns(soon, "the internal certificate CN=argus-auth expires in 29 days"));

  const auto later =
      tls_health::report({.certificatePem = leafOf(current, 31 * kDay + 3600),
                          .trustBundlePem = bundleOf(current)});
  CHECK(later["certificate"]["expiresInDays"].asInt() == 31);
  CHECK(warningCount(later) == 0);
}

TEST_CASE("an expired certificate warns that it has expired")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto health =
      tls_health::report({.certificatePem = leafOf(current, -3 * kDay),
                          .trustBundlePem = bundleOf(current)});
  CHECK(health["certificate"]["expired"].asBool());
  CHECK(warns(health, "the internal certificate CN=argus-auth has expired"));
}

TEST_CASE("the issuing CA warns under 30 days left like a certificate")
{
  const auto ending = authority("Argus internal CA ending", 28 * kDay + 3600);
  const auto health =
      tls_health::report({.certificatePem = leafOf(ending, 19 * kDay + 3600),
                          .trustBundlePem = bundleOf(ending)});
  CHECK(health["issuingCa"]["expiresInDays"].asInt() == 28);
  CHECK(
      warns(health,
            "the internal CA CN=Argus internal CA ending expires in 28 days"));
  CHECK(warns(health,
              "the internal certificate CN=argus-auth expires in 19 days"));

  const auto lasting = authority("Argus internal CA lasting", 32 * kDay + 3600);
  const auto calm =
      tls_health::report({.certificatePem = leafOf(lasting, 31 * kDay + 3600),
                          .trustBundlePem = bundleOf(lasting)});
  CHECK(warningCount(calm) == 0);
}

TEST_CASE(
    "a previous CA near its end stays quiet when it issued nothing loaded")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto previous =
      authority("Argus internal CA previous", 9 * kDay + 3600);
  const auto health =
      tls_health::report({.certificatePem = leafOf(current, 730 * kDay),
                          .trustBundlePem = bundleOf(current, previous)});
  CHECK(health["trustBundleCas"].asUInt() == 2);
  CHECK(health["trustBundle"][1]["expiresInDays"].asInt() == 9);
  CHECK(warningCount(health) == 0);
}

TEST_CASE("a certificate whose issuer is not in the bundle warns")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto stranger = authority("Argus internal CA stranger", 3650 * kDay);
  const auto health =
      tls_health::report({.certificatePem = leafOf(current, 730 * kDay),
                          .trustBundlePem = bundleOf(stranger)});
  CHECK_FALSE(health.isMember("issuingCa"));
  CHECK(warns(health, "is not in the loaded trust bundle"));
}

TEST_CASE("a certificate that cannot be read warns")
{
  const auto current = authority("Argus internal CA current", 3650 * kDay);
  const auto health = tls_health::report(
      {.certificatePem = "-----BEGIN CERTIFICATE-----\nnot base64\n",
       .trustBundlePem = bundleOf(current)});
  CHECK(health["loaded"].asBool());
  CHECK(health["trustBundleCas"].asUInt() == 1);
  CHECK(warns(health, "cannot be read"));
}
