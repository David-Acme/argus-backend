#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>

#include <cstdint>
#include <string>
#include <vector>

struct NotificationDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct NotificationIdentityConfig
{
  std::string target;
  std::string credential;
  std::string rpcSecret;
};

struct NotificationSyncControlConfig
{
  std::string target;
  std::string credential;
  std::string secret;
};

struct NotificationVoiceConfig
{
  std::string target;
  std::string credential;
};

struct CallEngineConfig
{
  bool enabled{true};
  int64_t callGapS{300};
  int maxCallsPerHour{4};
  int64_t arrivalAbsenceS{10800};
  int64_t scheduledLateS{900};
  int64_t scheduleHorizonS{2592000};
  int64_t maxPendingScheduled{20};
  int64_t answeredStaleS{7200};
  int64_t retentionDays{30};
  std::string fallbackLang{"es"};
};

struct NotificationCallCallers
{
  std::vector<argus::client::CallerCredential> answer;
  std::vector<argus::client::CallerCredential> schedule;
  std::vector<argus::client::CallerCredential> agenda;
};

class NotificationConfig
{
public:
  static NotificationDbConfig resolveDb();
  static ListenerConfig resolveListener();
  static GrpcListenerConfig resolveRpcListener();
  static NotificationIdentityConfig resolveIdentity();
  static std::vector<argus::client::CallerCredential> resolveSettingsCallers();
  static int64_t resolveAckWindowS();
  static int64_t resolveSelfTestIntervalS();
  static NotificationSyncControlConfig resolveSyncControl();
  static NotificationVoiceConfig resolveVoice();
  static CallEngineConfig resolveCalls();
  static NotificationCallCallers resolveCallCallers();
};
