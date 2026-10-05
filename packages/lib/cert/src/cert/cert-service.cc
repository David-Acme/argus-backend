#include "cert-service.hxx"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <drogon/drogon.h>
#include <fstream>
#include <memory>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <config/config-service.hxx>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>

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
  std::string pairingCode;
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

struct AtomicWriteInput
{
  const std::string& path;
  const std::string& data;
  mode_t mode{0600};
};

bool writeFileAtomic(const AtomicWriteInput& input)
{
  const std::string tmp = input.path + ".tmp";
  ::unlink(tmp.c_str());
  const int fd =
      ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, input.mode);
  if (fd < 0)
    return false;
  size_t written = 0;
  bool ok = true;
  while (ok && written < input.data.size()) {
    const ssize_t chunk = ::write(fd, input.data.data() + written,
                                  input.data.size() - written);
    if (chunk < 0 && errno == EINTR)
      continue;
    ok = chunk > 0;
    if (ok)
      written += static_cast<size_t>(chunk);
  }
  ok = ok && ::fchmod(fd, input.mode) == 0 && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), input.path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::mutex& fingerprintMutex()
{
  static std::mutex mutex;
  return mutex;
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

std::string loadPairingCode(const std::string& path)
{
  std::ifstream in(path);
  std::string code;
  if (!(in >> code))
    return {};
  code = toUpper(code);
  const bool hex = std::ranges::all_of(code, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
  });
  const bool base32 = std::ranges::all_of(code, [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7');
  });
  if (base32 && code.size() >= 26 && code.size() <= 32)
    return code;
  if (!hex || code.size() < 8 || code.size() > 12)
    return {};
  return code;
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

long certDaysRemaining(X509* cert)
{
  const ASN1_TIME* notAfter = X509_get0_notAfter(cert);
  if (!notAfter)
    return 0;
  struct tm tm{};
  if (ASN1_TIME_to_tm(notAfter, &tm) != 1)
    return 0;
  const time_t expiry = timegm(&tm);
  const double secs = difftime(expiry, time(nullptr));
  return static_cast<long>(secs / 86400.0);
}

bool isValidHostname(const std::string& host)
{
  if (host.find_first_not_of(" \t") == std::string::npos)
    return false;
  if (host.front() == '.' || host.back() == '.')
    return false;
  return host.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
             ".-") == std::string::npos;
}

bool isPrivateAddress(const std::string& text)
{
  std::array<unsigned char, 16> bytes{};
  if (inet_pton(AF_INET, text.c_str(), bytes.data()) == 1) {
    const unsigned first = bytes[0];
    const unsigned second = bytes[1];
    return first == 10 || (first == 172 && (second & 0xF0U) == 16) ||
           (first == 192 && second == 168) || (first == 169 && second == 254) ||
           (first == 100 && (second & 0xC0U) == 64);
  }
  if (inet_pton(AF_INET6, text.c_str(), bytes.data()) == 1)
    return (bytes[0] & 0xFEU) == 0xFC || (bytes[0] == 0xFE && (bytes[1] & 0xC0U) == 0x80);
  return false;
}

std::vector<std::string> instanceSans()
{
  std::vector<std::string> names{"argus.local", "localhost", "127.0.0.1",
                                 "::1"};
  const std::string mdnsName = ConfigService::getString("mdns.name");
  if (!mdnsName.empty())
    names.push_back(mdnsName);
  std::array<char, 256> hostname{};
  if (gethostname(hostname.data(), hostname.size()) == 0)
    names.emplace_back(hostname.data());
  const std::string lanAddress = ConfigService::getString("mdns.address");
  if (isPrivateAddress(lanAddress) &&
      std::ranges::find(names, lanAddress) == names.end())
    names.push_back(lanAddress);
  const std::string remoteHost = ConfigService::getString("remote.hostname");
  if (remoteHost.empty())
    return names;
  if (!isValidHostname(remoteHost)) {
    LOG_WARN << "remote.hostname '" << remoteHost
             << "' is not a valid DNS hostname, ignoring it";
    return names;
  }
  names.push_back(remoteHost);
  return names;
}

std::string buildSanString(const std::vector<std::string>& sans)
{
  std::string out;
  std::array<unsigned char, 16> addr{};
  for (const auto& value : sans) {
    if (!out.empty())
      out.push_back(',');
    if (inet_pton(AF_INET, value.c_str(), addr.data()) == 1 ||
        inet_pton(AF_INET6, value.c_str(), addr.data()) == 1)
      out += "IP:" + value;
    else
      out += "DNS:" + value;
  }
  return out;
}

bool permittedByCa(NAME_CONSTRAINTS* constraints, const std::string& san)
{
  X509Ptr probe(X509_new(), X509_free);
  if (!probe)
    return false;
  std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)> ext(
      X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                          buildSanString({san}).c_str()),
      &X509_EXTENSION_free);
  if (!ext || X509_add_ext(probe.get(), ext.get(), -1) != 1)
    return false;
  static_cast<void>(X509_check_purpose(probe.get(), -1, 0));
  return NAME_CONSTRAINTS_check(probe.get(), constraints) == X509_V_OK;
}

std::vector<std::string> permittedSans(X509* ca, std::vector<std::string> sans)
{
  std::unique_ptr<NAME_CONSTRAINTS, void (*)(NAME_CONSTRAINTS*)> constraints(
      static_cast<NAME_CONSTRAINTS*>(
          X509_get_ext_d2i(ca, NID_name_constraints, nullptr, nullptr)),
      &NAME_CONSTRAINTS_free);
  if (!constraints)
    return sans;
  std::erase_if(sans, [&constraints](const std::string& san) {
    if (permittedByCa(constraints.get(), san))
      return false;
    LOG_WARN << "cert rotation: '" << san
             << "' is outside the instance CA's name constraints; left out "
                "of the leaf";
    return true;
  });
  return sans;
}

struct CertLeafInput
{
  EVP_PKEY* leafKey;
  X509* caCert;
  EVP_PKEY* caKey;
  const std::vector<std::string>& sans;
  int ttlDays;
};

X509Ptr buildLeaf(const CertLeafInput& input)
{
  EVP_PKEY* leafKey = input.leafKey;
  X509* caCert = input.caCert;
  EVP_PKEY* caKey = input.caKey;
  const std::vector<std::string>& sans = input.sans;
  const int ttlDays = input.ttlDays;

  X509Ptr cert(X509_new(), X509_free);
  if (!cert)
    return {nullptr, X509_free};
  if (X509_set_version(cert.get(), 2) != 1)
    return {nullptr, X509_free};

  std::unique_ptr<BIGNUM, void (*)(BIGNUM*)> bn(BN_new(), &BN_free);
  if (bn) {
    if (BN_rand(bn.get(), 160, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1)
      BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(cert.get()));
  }

  if (X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr)
    return {nullptr, X509_free};
  if (X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                      static_cast<long>(ttlDays) * 86400L) == nullptr)
    return {nullptr, X509_free};

  X509_set_issuer_name(cert.get(), X509_get_subject_name(caCert));
  std::unique_ptr<X509_NAME, void (*)(X509_NAME*)> subject(X509_NAME_new(),
                                                           &X509_NAME_free);
  if (!subject)
    return {nullptr, X509_free};
  X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(
                                 "Argus.local"),
                             -1, -1, 0);
  X509_set_subject_name(cert.get(), subject.get());

  if (X509_set_pubkey(cert.get(), leafKey) != 1)
    return {nullptr, X509_free};

  for (const auto& [nid, value] :
       {std::pair{NID_basic_constraints, "critical,CA:FALSE"},
        std::pair{NID_key_usage, "critical,digitalSignature"},
        std::pair{NID_ext_key_usage, "serverAuth"}}) {
    std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)> ext(
        X509V3_EXT_conf_nid(nullptr, nullptr, nid, value), &X509_EXTENSION_free);
    if (!ext || X509_add_ext(cert.get(), ext.get(), -1) != 1)
      return {nullptr, X509_free};
  }

  const std::string sanStr = buildSanString(sans);
  if (!sanStr.empty()) {
    std::unique_ptr<X509_EXTENSION, void (*)(X509_EXTENSION*)>
        ext(X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                                sanStr.c_str()),
            &X509_EXTENSION_free);
    if (ext) {
      X509_add_ext(cert.get(), ext.get(), -1);
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

}

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
  p.pairingCode = ConfigService::getString("cert.pairing_code");
  if (p.caCert.empty())
    p.caCert = p.dir + "/ca.pem";
  if (p.caKey.empty())
    p.caKey = p.dir + "/ca.key";
  if (p.serverCert.empty())
    p.serverCert = p.dir + "/server.pem";
  if (p.serverKey.empty())
    p.serverKey = p.dir + "/server.key";
  if (p.pairingCode.empty())
    p.pairingCode = p.dir + "/pairing.code";
  if (const int v = ConfigService::getInt("cert.leaf_ttl_days"); v > 0)
    p.leafTtlDays = v;
  if (const int v = ConfigService::getInt("cert.rotation_threshold_days");
      v > 0)
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
  gState.pairingCode = loadPairingCode(p.pairingCode);
  if (gState.pairingCode.empty())
    LOG_WARN << "No pairing code in " << p.pairingCode
             << " - run scripts/setup.sh; pairing is disabled";
  gState.loaded.store(true);

  if (certDaysRemaining(server.get()) <= p.rotationThresholdDays) {
    LOG_INFO << "PKI leaf near expiry, rotating on startup";
    rotateServerCertificate();
  }

  gState.running.store(true, std::memory_order_relaxed);
  gState.rotationThread = std::thread(rotationLoop);

  LOG_INFO << "PKI loaded (instance " << gState.caFingerprint
           << ")";
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
  const std::scoped_lock lock(fingerprintMutex());
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

namespace
{
std::string pairingHmacHex(const std::string& key, const std::string& message)
{
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(message.data()), message.size(),
           digest.data(), &length) == nullptr)
    return {};
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string hex;
  hex.reserve(static_cast<size_t>(length) * 2);
  for (unsigned int i = 0; i < length; ++i) {
    hex.push_back(kHex[digest[i] >> 4]);
    hex.push_back(kHex[digest[i] & 0x0F]);
  }
  return hex;
}
}

bool CertService::verifyPairingProof(const PairingProofInput& input)
{
  if (!gState.loaded.load() || gState.pairingCode.empty() || input.nonce.empty())
    return false;
  const std::string expected =
      pairingHmacHex(gState.pairingCode, "argus-pair-client|" + input.nonce);
  const std::string candidate = toUpper(input.proof);
  return !expected.empty() && candidate.size() == expected.size() &&
         CRYPTO_memcmp(candidate.data(), expected.data(), expected.size()) == 0;
}

std::string CertService::pairingServerProof(const std::string& nonce)
{
  if (!gState.loaded.load() || gState.pairingCode.empty())
    return {};
  return pairingHmacHex(gState.pairingCode, "argus-pair-server|" + nonce + "|" +
                                                caFingerprint());
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

  const std::vector<std::string> sans = permittedSans(ca.get(), instanceSans());
  X509Ptr leaf = buildLeaf({.leafKey = leafKey.get(),
                            .caCert = ca.get(),
                            .caKey = caKey.get(),
                            .sans = sans,
                            .ttlDays = gState.paths.leafTtlDays});
  if (!leaf) {
    LOG_ERROR << "cert rotation: failed to build leaf";
    return false;
  }

  std::string chain = pemOf(leaf.get());
  chain += pemOf(ca.get());
  const std::string keyData = keyPem(leafKey.get());
  if (!writeFileAtomic(
          {.path = gState.paths.serverKey, .data = keyData, .mode = 0600}) ||
      !writeFileAtomic(
          {.path = gState.paths.serverCert, .data = chain, .mode = 0644})) {
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

  const std::string rotated = sha256Hex(reloaded.get());
  {
    const std::scoped_lock lock(fingerprintMutex());
    gState.serverFingerprint = rotated;
  }
  if (drogon::app().isRunning())
    drogon::app().reloadSSLFiles();
  LOG_INFO << "server certificate rotated (fp " << rotated << ")";
  return true;
}

Json::Value CertService::health()
{
  Json::Value value(Json::objectValue);
  value["loaded"] = gState.loaded.load();
  value["instanceId"] = gState.caFingerprint;
  value["caFingerprint"] = gState.caFingerprint;
  value["serverFingerprint"] = serverFingerprint();
  value["pairingCodeSet"] = !gState.pairingCode.empty();
  X509Ptr server = loadX509(gState.paths.serverCert);
  value["serverExpiryDays"] = server ? certDaysRemaining(server.get()) : 0;
  return value;
}
