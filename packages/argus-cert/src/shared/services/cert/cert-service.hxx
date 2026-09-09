#pragma once

#include <json/value.h>
#include <string>

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

  static bool rotateServerCertificate();
  static Json::Value health();
};
