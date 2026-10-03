#pragma once

#include <json/value.h>
#include <string>

struct PairingProofInput
{
  std::string nonce;
  std::string proof;
};

class CertService
{
public:
  static bool init();
  static bool isLoaded();
  static void shutdown();

  static std::string caPem();
  static std::string instanceId();
  static std::string caFingerprint();
  static std::string serverFingerprint();
  static std::string pairingCode();
  static bool verifyPairingCode(const std::string& code);
  static bool verifyPairingProof(const PairingProofInput& input);
  static std::string pairingServerProof(const std::string& nonce);

  static bool rotateServerCertificate();
  static Json::Value health();
};
