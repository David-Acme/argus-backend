#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using SslCtxPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using SslPtr = std::unique_ptr<SSL, decltype(&SSL_free)>;

constexpr const char* kRemoteHost = "argus.example.com";
constexpr const char* kScratchDir = "cert-san-test-certs";

std::string readFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

PKeyPtr generateEcKey()
{
  EVP_PKEY_CTX* raw = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (!raw)
    return {nullptr, EVP_PKEY_free};
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>
      ctx(raw, EVP_PKEY_CTX_free);
  if (EVP_PKEY_keygen_init(ctx.get()) <= 0)
    return {nullptr, EVP_PKEY_free};
  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <=
      0)
    return {nullptr, EVP_PKEY_free};
  EVP_PKEY* key = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &key) <= 0)
    return {nullptr, EVP_PKEY_free};
  return PKeyPtr(key, EVP_PKEY_free);
}

struct CaKeys
{
  X509* cert = nullptr;
  EVP_PKEY* key = nullptr;
};

X509Ptr signLeaf(const CaKeys& ca, int days)
{
  X509Ptr leaf(X509_new(), X509_free);
  if (!leaf || X509_set_version(leaf.get(), 2) != 1)
    return {nullptr, X509_free};
  std::unique_ptr<BIGNUM, void (*)(BIGNUM*)> bn(BN_new(), &BN_free);
  if (bn && BN_rand(bn.get(), 160, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1)
    BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(leaf.get()));
  if (X509_set_issuer_name(leaf.get(), X509_get_subject_name(ca.cert)) != 1)
    return {nullptr, X509_free};
  std::unique_ptr<X509_NAME, void (*)(X509_NAME*)> subject(X509_NAME_new(),
                                                           &X509_NAME_free);
  if (!subject ||
      X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>(
                                     "Argus Cert San Test Leaf"),
                                 -1, -1, 0) != 1)
    return {nullptr, X509_free};
  if (X509_set_subject_name(leaf.get(), subject.get()) != 1)
    return {nullptr, X509_free};
  PKeyPtr leafKey = generateEcKey();
  if (!leafKey || X509_set_pubkey(leaf.get(), leafKey.get()) != 1)
    return {nullptr, X509_free};
  X509_gmtime_adj(X509_getm_notBefore(leaf.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(leaf.get()),
                  static_cast<long>(days) * 86400L);
  if (X509_sign(leaf.get(), ca.key, EVP_sha256()) <= 0)
    return {nullptr, X509_free};
  return leaf;
}

bool writeCertPem(const std::string& path, X509* cert)
{
  BioPtr bio(BIO_new_file(path.c_str(), "w"), BIO_free);
  return bio && PEM_write_bio_X509(bio.get(), cert) == 1;
}

bool writeKeyPem(const std::string& path, EVP_PKEY* key)
{
  BioPtr bio(BIO_new_file(path.c_str(), "w"), BIO_free);
  return bio && PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0,
                                         nullptr, nullptr) == 1;
}

X509Ptr firstCertFromPem(const std::string& pem)
{
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
             BIO_free);
  if (!bio)
    return {nullptr, X509_free};
  return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                 X509_free);
}

std::vector<std::string> sansOf(X509* cert)
{
  std::vector<std::string> names;
  auto* stack = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (!stack)
    return names;
  for (int i = 0; i < sk_GENERAL_NAME_num(stack); ++i) {
    const GENERAL_NAME* entry = sk_GENERAL_NAME_value(stack, i);
    if (entry->type == GEN_DNS) {
      const unsigned char* data = ASN1_STRING_get0_data(entry->d.dNSName);
      names.emplace_back(reinterpret_cast<const char*>(data),
                         ASN1_STRING_length(entry->d.dNSName));
    } else if (entry->type == GEN_IPADD) {
      const unsigned char* data = ASN1_STRING_get0_data(entry->d.iPAddress);
      char buf[INET6_ADDRSTRLEN] = {0};
      const int family =
          ASN1_STRING_length(entry->d.iPAddress) == 4 ? AF_INET : AF_INET6;
      if (inet_ntop(family, data, buf, sizeof(buf)) != nullptr)
        names.emplace_back(buf);
    }
  }
  sk_GENERAL_NAME_pop_free(stack, GENERAL_NAME_free);
  return names;
}

std::string fingerprintOf(X509* cert)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &len) != 1)
    return {};
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned int i = 0; i < len; ++i) {
    out.push_back(kHex[(md[i] >> 4) & 0xF]);
    out.push_back(kHex[md[i] & 0xF]);
  }
  return out;
}

// The SAN list cert-service produces with remote.hostname unset.
std::vector<std::string> baseSans()
{
  std::vector<std::string> names{"argus.local", "localhost", "127.0.0.1",
                                 "::1"};
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0)
    names.emplace_back(hostname);
  return names;
}

struct PeerInfo
{
  std::string fingerprint;
  std::vector<std::string> sans;
};

PeerInfo servedPeer(uint16_t port)
{
  SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
  REQUIRE(ctx);
  SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
  PeerInfo info;
  for (int attempt = 0; attempt < 40; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
      ::close(fd);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    SslPtr ssl(SSL_new(ctx.get()), SSL_free);
    REQUIRE(ssl);
    SSL_set_fd(ssl.get(), fd);
    if (SSL_connect(ssl.get()) == 1) {
      X509Ptr peer(SSL_get1_peer_certificate(ssl.get()), X509_free);
      REQUIRE(peer);
      info.fingerprint = fingerprintOf(peer.get());
      info.sans = sansOf(peer.get());
      SSL_shutdown(ssl.get());
      ::close(fd);
      return info;
    }
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  FAIL("TLS listener never accepted a handshake");
  return info;
}

uint16_t freePort()
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) ==
          0);
  socklen_t len = sizeof(addr);
  REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  ::close(fd);
  return ntohs(addr.sin_port);
}

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

} // namespace

TEST_CASE("remote.hostname drives the leaf SAN list and the hot reload")
{
  const std::filesystem::path dir{kScratchDir};
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  PKeyPtr caKey = generateEcKey();
  REQUIRE(caKey);
  X509Ptr ca(X509_new(), X509_free);
  REQUIRE(ca);
  REQUIRE(X509_set_version(ca.get(), 2) == 1);
  std::unique_ptr<BIGNUM, void (*)(BIGNUM*)> caBn(BN_new(), &BN_free);
  REQUIRE(caBn);
  REQUIRE(BN_rand(caBn.get(), 160, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1);
  BN_to_ASN1_INTEGER(caBn.get(), X509_get_serialNumber(ca.get()));
  std::unique_ptr<X509_NAME, void (*)(X509_NAME*)> caSubject(X509_NAME_new(),
                                                             &X509_NAME_free);
  REQUIRE(caSubject);
  REQUIRE(X509_NAME_add_entry_by_txt(caSubject.get(), "CN", MBSTRING_ASC,
                                     reinterpret_cast<const unsigned char*>(
                                         "Argus Cert San Test CA"),
                                     -1, -1, 0) == 1);
  REQUIRE(X509_set_subject_name(ca.get(), caSubject.get()) == 1);
  REQUIRE(X509_set_issuer_name(ca.get(), caSubject.get()) == 1);
  REQUIRE(X509_set_pubkey(ca.get(), caKey.get()) == 1);
  X509_gmtime_adj(X509_getm_notBefore(ca.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(ca.get()), 3650L * 86400L);
  std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)> caExt(
      X509V3_EXT_conf_nid(nullptr, nullptr, NID_basic_constraints,
                          "critical,CA:TRUE"),
      &X509_EXTENSION_free);
  REQUIRE(caExt);
  REQUIRE(X509_add_ext(ca.get(), caExt.get(), -1) == 1);
  REQUIRE(X509_sign(ca.get(), caKey.get(), EVP_sha256()) > 0);

  const CaKeys caKeys{ca.get(), caKey.get()};
  X509Ptr seed = signLeaf(caKeys, 90);
  REQUIRE(seed);
  PKeyPtr seedKey = generateEcKey();
  REQUIRE(seedKey);
  REQUIRE(writeCertPem(dir / "ca.pem", ca.get()));
  REQUIRE(writeKeyPem(dir / "ca.key", caKey.get()));
  REQUIRE(writeCertPem(dir / "server.pem", seed.get()));
  REQUIRE(writeKeyPem(dir / "server.key", seedKey.get()));

  ConfigService::setRuntimeString("cert.dir", dir.string());
  ConfigService::setRuntimeString("cert.rotation_threshold_days", "3650");
  ConfigService::setRuntimeString("mdns.name", "");

  REQUIRE(CertService::init());
  REQUIRE(CertService::isLoaded());

  REQUIRE(CertService::rotateServerCertificate());
  const X509Ptr absentLeaf = firstCertFromPem(readFile(dir / "server.pem"));
  REQUIRE(absentLeaf);
  CHECK(sansOf(absentLeaf.get()) == baseSans());
  std::filesystem::copy_file(dir / "server.pem", dir / "leaf-no-hostname.pem",
                             std::filesystem::copy_options::overwrite_existing);
  const std::string absentFingerprint = fingerprintOf(absentLeaf.get());

  for (const char* bad : {" ", "bad_host", ".leading.dot", "trailing.dot.",
                          "argus.example.com, IP:10.0.0.1", "white space"}) {
    ConfigService::setRuntimeString("remote.hostname", bad);
    REQUIRE(CertService::rotateServerCertificate());
    const X509Ptr rejectedLeaf = firstCertFromPem(readFile(dir / "server.pem"));
    REQUIRE(rejectedLeaf);
    CHECK(sansOf(rejectedLeaf.get()) == baseSans());
  }

  ConfigService::setRuntimeString("remote.hostname", kRemoteHost);
  REQUIRE(CertService::rotateServerCertificate());
  const X509Ptr leaf = firstCertFromPem(readFile(dir / "server.pem"));
  REQUIRE(leaf);
  const std::vector<std::string> sans = sansOf(leaf.get());
  CHECK(sans.size() == baseSans().size() + 1);
  CHECK(sans.back() == kRemoteHost);
  std::filesystem::copy_file(
      dir / "server.pem", dir / "leaf-with-hostname.pem",
      std::filesystem::copy_options::overwrite_existing);

  const uint16_t port = freePort();
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().setUploadPath("/tmp/argus-cert-san-test-upload");
  drogon::app().setSSLFiles((dir / "server.pem").string(),
                            (dir / "server.key").string());
  drogon::app().addListener("127.0.0.1", port, true);
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(10)));

  const PeerInfo before = servedPeer(port);
  CHECK(before.fingerprint == fingerprintOf(leaf.get()));
  CHECK_FALSE(before.sans.empty());
  CHECK(before.sans.back() == kRemoteHost);

  REQUIRE(CertService::rotateServerCertificate());
  const PeerInfo after = servedPeer(port);
  CHECK_FALSE(after.fingerprint.empty());
  CHECK(after.fingerprint != before.fingerprint);
  CHECK(after.fingerprint != absentFingerprint);
  CHECK(after.sans.back() == kRemoteHost);

  drogon::app().quit();
  runner.join();
  CertService::shutdown();
}
