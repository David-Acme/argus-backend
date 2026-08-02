#include "cert-service.hxx"

#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <drogon/drogon.h>
#include <fstream>
#include <memory>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <shared/services/config-service/config-service.hxx>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

struct CertPaths
{
  std::string dir = "certs";
  std::string caCert;
  std::string caKey;
  std::string serverCert;
  std::string serverKey;
  int leafTtlDays = 90;
  int rotationThresholdDays = 30;
  int rotateCheckHours = 24;
};

struct CertState
{
  CertPaths paths;
  std::string caPem;
  std::string caFingerprint;
  std::string serverFingerprint;
  std::string pairingCode;
  std::atomic<bool> loaded{false};
  std::atomic<bool> running{false};
  std::thread rotationThread;
};

CertState gState;

bool writeFile(const std::string& path, const std::string& data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out << data;
  return static_cast<bool>(out);
}

bool writeFileAtomic(const std::string& path, const std::string& data)
{
  const std::string tmp = path + ".tmp";
  if (!writeFile(tmp, data))
    return false;
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

std::string pemOf(X509* cert)
{
  BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio)
    return {};
  if (PEM_write_bio_X509(bio.get(), cert) != 1)
    return {};
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return len > 0 && data ? std::string(data, static_cast<size_t>(len))
                         : std::string{};
}

std::string keyPem(EVP_PKEY* key)
{
  BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio)
    return {};
  if (PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr,
                               nullptr) != 1)
    return {};
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return len > 0 && data ? std::string(data, static_cast<size_t>(len))
                         : std::string{};
}

X509Ptr loadX509(const std::string& path)
{
  BioPtr bio(BIO_new_file(path.c_str(), "r"), BIO_free);
  if (!bio)
    return {nullptr, X509_free};
  return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                 X509_free);
}

PKeyPtr loadKey(const std::string& path)
{
  BioPtr bio(BIO_new_file(path.c_str(), "r"), BIO_free);
  if (!bio)
    return {nullptr, EVP_PKEY_free};
  return PKeyPtr(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
                 EVP_PKEY_free);
}

std::string sha256Hex(X509* cert)
{
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &len) != 1)
    return {};
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (unsigned int i = 0; i < len; ++i) {
    out.push_back(kHex[(md[i] >> 4) & 0xF]);
    out.push_back(kHex[md[i] & 0xF]);
  }
  return out;
}

std::string toUpper(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string deriveCode(const std::string& fingerprint)
{
  return fingerprint.size() >= 8 ? fingerprint.substr(0, 8)
                                 : std::string{};
}

PKeyPtr generateEcKey()
{
  EVP_PKEY_CTX* raw = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  if (!raw)
    return {nullptr, EVP_PKEY_free};
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      raw, EVP_PKEY_CTX_free);
  if (EVP_PKEY_keygen_init(ctx.get()) <= 0)
    return {nullptr, EVP_PKEY_free};
  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(),
                                             NID_X9_62_prime256v1) <= 0)
    return {nullptr, EVP_PKEY_free};
  EVP_PKEY* key = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &key) <= 0)
    return {nullptr, EVP_PKEY_free};
  return PKeyPtr(key, EVP_PKEY_free);
}

long certDaysRemaining(X509* cert)
{
  const ASN1_TIME* notAfter = X509_get0_notAfter(cert);
  if (!notAfter)
    return 0;
  struct tm tm {};
  if (ASN1_TIME_to_tm(notAfter, &tm) != 1)
    return 0;
  const time_t expiry = timegm(&tm);
  const double secs = difftime(expiry, time(nullptr));
  return static_cast<long>(secs / 86400.0);
}

std::vector<std::string> instanceSans()
{
  std::vector<std::string> names{"argus.local", "localhost", "127.0.0.1",
                                 "::1"};
  const std::string mdnsName = ConfigService::getString("mdns.name");
  if (!mdnsName.empty())
    names.push_back(mdnsName);
  char hostname[256];
  if (gethostname(hostname, sizeof(hostname)) == 0)
    names.emplace_back(hostname);
  return names;
}

std::string buildSanString(const std::vector<std::string>& sans)
{
  std::string out;
  unsigned char addr[16];
  for (const auto& value : sans) {
    if (!out.empty())
      out.push_back(',');
    if (inet_pton(AF_INET, value.c_str(), addr) == 1 ||
        inet_pton(AF_INET6, value.c_str(), addr) == 1)
      out += "IP:" + value;
    else
      out += "DNS:" + value;
  }
  return out;
}

X509Ptr buildLeaf(EVP_PKEY* leafKey, X509* caCert, EVP_PKEY* caKey,
                  const std::vector<std::string>& sans, int ttlDays)
{
  X509Ptr cert(X509_new(), X509_free);
  if (!cert)
    return {nullptr, X509_free};
  if (X509_set_version(cert.get(), 2) != 1)
    return {nullptr, X509_free};

  BIGNUM* bn = BN_new();
  if (bn) {
    if (BN_rand(bn, 160, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1)
      BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()));
    BN_free(bn);
  }

  if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr)
    return {nullptr, X509_free};
  if (X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                      static_cast<long>(ttlDays) * 86400L) == nullptr)
    return {nullptr, X509_free};

  X509_set_issuer_name(cert.get(), X509_get_subject_name(caCert));
  X509_NAME* subject = X509_NAME_new();
  if (!subject)
    return {nullptr, X509_free};
  X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(
                                 "Argus.local"),
                             -1, -1, 0);
  X509_set_subject_name(cert.get(), subject);
  X509_NAME_free(subject);

  if (X509_set_pubkey(cert.get(), leafKey) != 1)
    return {nullptr, X509_free};

  const std::string sanStr = buildSanString(sans);
  if (!sanStr.empty()) {
    X509_EXTENSION* ext =
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                            sanStr.c_str());
    if (ext) {
      X509_add_ext(cert.get(), ext, -1);
      X509_EXTENSION_free(ext);
    }
  }

  if (X509_sign(cert.get(), caKey, EVP_sha256()) <= 0)
    return {nullptr, X509_free};
  return cert;
}

void rotationLoop()
{
  const int checkHours = std::max(1, gState.paths.rotateCheckHours);
  const auto interval = std::chrono::hours(checkHours);
  auto next = std::chrono::steady_clock::now() + interval;
  while (gState.running.load(std::memory_order_relaxed)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
      next = now + interval;
      if (drogon::app().isRunning())
        CertService::rotateServerCertificate();
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
}

} // namespace

bool CertService::init()
{
  CertPaths& p = gState.paths;
  const std::string dir = ConfigService::getString("cert.dir");
  if (!dir.empty())
    p.dir = dir;
  p.caCert = ConfigService::getString("cert.ca_cert");
  p.caKey = ConfigService::getString("cert.ca_key");
  p.serverCert = ConfigService::getString("cert.server_cert");
  p.serverKey = ConfigService::getString("cert.server_key");
  if (p.caCert.empty())
    p.caCert = p.dir + "/ca.pem";
  if (p.caKey.empty())
    p.caKey = p.dir + "/ca.key";
  if (p.serverCert.empty())
    p.serverCert = p.dir + "/server.pem";
  if (p.serverKey.empty())
    p.serverKey = p.dir + "/server.key";
  if (const int v = ConfigService::getInt("cert.leaf_ttl_days"); v > 0)
    p.leafTtlDays = v;
  if (const int v = ConfigService::getInt("cert.rotation_threshold_days"); v > 0)
    p.rotationThresholdDays = v;
  if (const int v = ConfigService::getInt("cert.rotate_check_hours"); v > 0)
    p.rotateCheckHours = v;

  X509Ptr ca = loadX509(p.caCert);
  X509Ptr server = loadX509(p.serverCert);
  if (!ca || !server) {
    LOG_FATAL << "PKI not found in " << p.dir
              << " — run scripts/setup.sh first (generates certs/)";
    return false;
  }

  gState.caPem = pemOf(ca.get());
  gState.caFingerprint = sha256Hex(ca.get());
  gState.serverFingerprint = sha256Hex(server.get());
  gState.pairingCode = deriveCode(gState.caFingerprint);
  gState.loaded.store(true);

  if (certDaysRemaining(server.get()) <= p.rotationThresholdDays) {
    LOG_INFO << "PKI leaf near expiry, rotating on startup";
    rotateServerCertificate();
  }

  gState.running.store(true, std::memory_order_relaxed);
  gState.rotationThread = std::thread(rotationLoop);

  LOG_INFO << "PKI loaded (instance " << gState.caFingerprint
           << "), pairing code " << gState.pairingCode;
  return true;
}

bool CertService::isLoaded()
{
  return gState.loaded.load();
}

void CertService::shutdown()
{
  gState.running.store(false, std::memory_order_relaxed);
  if (gState.rotationThread.joinable())
    gState.rotationThread.join();
  gState.loaded.store(false);
}

std::string CertService::caPem()
{
  return gState.caPem;
}

std::string CertService::instanceId()
{
  return gState.caFingerprint;
}

std::string CertService::caFingerprint()
{
  return gState.caFingerprint;
}

std::string CertService::serverFingerprint()
{
  return gState.serverFingerprint;
}

std::string CertService::pairingCode()
{
  return gState.pairingCode;
}

bool CertService::verifyPairingCode(const std::string& code)
{
  if (!gState.loaded.load() || gState.pairingCode.empty())
    return false;
  const std::string candidate = toUpper(code);
  if (candidate.size() != gState.pairingCode.size())
    return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < candidate.size(); ++i)
    diff |= static_cast<unsigned char>(candidate[i]) ^
            static_cast<unsigned char>(gState.pairingCode[i]);
  return diff == 0;
}

bool CertService::rotateServerCertificate()
{
  if (!gState.loaded.load())
    return false;

  X509Ptr current = loadX509(gState.paths.serverCert);
  if (current &&
      certDaysRemaining(current.get()) > gState.paths.rotationThresholdDays)
    return true;

  PKeyPtr leafKey = generateEcKey();
  X509Ptr ca = loadX509(gState.paths.caCert);
  PKeyPtr caKey = loadKey(gState.paths.caKey);
  if (!leafKey || !ca || !caKey) {
    LOG_ERROR << "cert rotation: failed to load PKI";
    return false;
  }

  X509Ptr leaf = buildLeaf(leafKey.get(), ca.get(), caKey.get(),
                           instanceSans(), gState.paths.leafTtlDays);
  if (!leaf) {
    LOG_ERROR << "cert rotation: failed to build leaf";
    return false;
  }

  std::string chain = pemOf(leaf.get());
  chain += pemOf(ca.get());
  if (!writeFileAtomic(gState.paths.serverCert, chain) ||
      !writeFileAtomic(gState.paths.serverKey, keyPem(leafKey.get()))) {
    LOG_ERROR << "cert rotation: atomic write failed";
    return false;
  }

  X509Ptr reloaded = loadX509(gState.paths.serverCert);
  PKeyPtr reloadedKey = loadKey(gState.paths.serverKey);
  if (!reloaded || !reloadedKey ||
      X509_check_private_key(reloaded.get(), reloadedKey.get()) != 1) {
    LOG_ERROR << "cert rotation: validation failed after write";
    return false;
  }

  gState.serverFingerprint = sha256Hex(reloaded.get());
  if (drogon::app().isRunning())
    drogon::app().reloadSSLFiles();
  LOG_INFO << "server certificate rotated (fp "
           << gState.serverFingerprint << ")";
  return true;
}

Json::Value CertService::health()
{
  Json::Value value(Json::objectValue);
  value["loaded"] = gState.loaded.load();
  value["instanceId"] = gState.caFingerprint;
  value["caFingerprint"] = gState.caFingerprint;
  value["serverFingerprint"] = gState.serverFingerprint;
  value["pairingCodeSet"] = !gState.pairingCode.empty();
  X509Ptr server = loadX509(gState.paths.serverCert);
  value["serverExpiryDays"] = server ? certDaysRemaining(server.get()) : 0;
  return value;
}
