#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <http/download/details/download-url.hxx>
#include <http/download/details/peer-name.hxx>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <string>

using file_download::details::certificateNamesHost;
using file_download::details::ContentRange;
using file_download::details::UrlParts;
using file_download::details::hostOfOrigin;
using file_download::details::parseContentRange;
using file_download::details::resolveLocation;
using file_download::details::splitUrl;

TEST_CASE("splitUrl separates the origin from the request target")
{
  CHECK(splitUrl("https://huggingface.co:443/org/repo/resolve/abc/model.gguf?download=true#x") ==
        UrlParts{.origin = "https://huggingface.co:443",
                 .target = "/org/repo/resolve/abc/model.gguf?download=true"});
  CHECK(splitUrl("http://127.0.0.1:8080") == UrlParts{.origin = "http://127.0.0.1:8080", .target = "/"});
  CHECK(splitUrl("http://host?x=1") == UrlParts{.origin = "http://host", .target = "/?x=1"});
}

TEST_CASE("splitUrl refuses what is not http or https")
{
  CHECK_FALSE(splitUrl("ftp://host/file").has_value());
  CHECK_FALSE(splitUrl("file:///etc/passwd").has_value());
  CHECK_FALSE(splitUrl("https:///path").has_value());
  CHECK_FALSE(splitUrl("https://user@host/path").has_value());
  CHECK_FALSE(splitUrl("").has_value());
}

TEST_CASE("resolveLocation follows absolute, scheme-relative, rooted and relative locations")
{
  constexpr std::string_view base = "https://huggingface.co/org/repo/resolve/main/model.gguf?x=1";
  CHECK(resolveLocation({.base = base, .location = "https://cdn.example/obj?sig=a%2Fb"}) ==
        "https://cdn.example/obj?sig=a%2Fb");
  CHECK(resolveLocation({.base = base, .location = "//cdn.example/obj"}) == "https://cdn.example/obj");
  CHECK(resolveLocation({.base = base, .location = "/api/resolve-cache/models/x"}) ==
        "https://huggingface.co/api/resolve-cache/models/x");
  CHECK(resolveLocation({.base = base, .location = "other.gguf"}) ==
        "https://huggingface.co/org/repo/resolve/main/other.gguf");
  CHECK_FALSE(resolveLocation({.base = base, .location = ""}).has_value());
  CHECK_FALSE(resolveLocation({.base = base, .location = "ftp://elsewhere/x"}).has_value());
}

TEST_CASE("parseContentRange reads a byte range and its total")
{
  CHECK(parseContentRange("bytes 100-199/1000") == ContentRange{.first = 100, .last = 199, .total = 1000});
  CHECK(parseContentRange("bytes 0-9/*") == ContentRange{.first = 0, .last = 9, .total = std::nullopt});
}

TEST_CASE("parseContentRange refuses malformed and impossible ranges")
{
  CHECK_FALSE(parseContentRange("").has_value());
  CHECK_FALSE(parseContentRange("bytes */1000").has_value());
  CHECK_FALSE(parseContentRange("items 0-9/10").has_value());
  CHECK_FALSE(parseContentRange("bytes 9-0/10").has_value());
  CHECK_FALSE(parseContentRange("bytes 0-10/10").has_value());
  CHECK_FALSE(parseContentRange("bytes a-9/10").has_value());
  CHECK_FALSE(parseContentRange("bytes 0-9/").has_value());
}

namespace
{

struct KeyDeleter
{
  void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

struct CertificateDeleter
{
  void operator()(X509* certificate) const { X509_free(certificate); }
};

struct BioDeleter
{
  void operator()(BIO* bio) const { BIO_free(bio); }
};

std::string certificatePem(const std::string& subjectAltName)
{
  const std::unique_ptr<EVP_PKEY, KeyDeleter> key(EVP_EC_gen("P-256"));
  const std::unique_ptr<X509, CertificateDeleter> certificate(X509_new());
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
  X509_set_pubkey(certificate.get(), key.get());
  auto* name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("unrelated.test"), -1, -1, 0);
  X509_set_issuer_name(certificate.get(), name);
  X509V3_CTX context{};
  X509V3_set_ctx(&context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
  auto* extension = X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name, subjectAltName.c_str());
  X509_add_ext(certificate.get(), extension, -1);
  X509_EXTENSION_free(extension);
  X509_sign(certificate.get(), key.get(), EVP_sha256());
  const std::unique_ptr<BIO, BioDeleter> bio(BIO_new(BIO_s_mem()));
  PEM_write_bio_X509(bio.get(), certificate.get());
  char* data = nullptr;
  const auto length = BIO_get_mem_data(bio.get(), &data);
  return {data, static_cast<std::size_t>(length)};
}

}

TEST_CASE("hostOfOrigin strips the scheme, the port and IPv6 brackets")
{
  CHECK(hostOfOrigin("https://cdn.example:8443") == "cdn.example");
  CHECK(hostOfOrigin("https://cdn.example") == "cdn.example");
  CHECK(hostOfOrigin("https://[::1]:8443") == "::1");
}

TEST_CASE("certificateNamesHost accepts only a certificate that names the host")
{
  const auto pem = certificatePem("DNS:*.hf.co,DNS:huggingface.co,IP:127.0.0.1");
  CHECK(certificateNamesHost({.certificatePem = pem, .origin = "https://huggingface.co"}));
  CHECK(certificateNamesHost({.certificatePem = pem, .origin = "https://cas-bridge.hf.co:443"}));
  CHECK(certificateNamesHost({.certificatePem = pem, .origin = "https://127.0.0.1:8443"}));
  CHECK_FALSE(certificateNamesHost({.certificatePem = pem, .origin = "https://evil.example"}));
  CHECK_FALSE(certificateNamesHost({.certificatePem = pem, .origin = "https://a.b.hf.co"}));
  CHECK_FALSE(certificateNamesHost({.certificatePem = pem, .origin = "https://unrelated.test"}));
  CHECK_FALSE(certificateNamesHost({.certificatePem = pem, .origin = "https://127.0.0.2"}));
  CHECK_FALSE(certificateNamesHost({.certificatePem = "", .origin = "https://huggingface.co"}));
  CHECK_FALSE(certificateNamesHost({.certificatePem = "garbage", .origin = "https://huggingface.co"}));
}
