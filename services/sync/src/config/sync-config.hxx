#pragma once

#include <http/listener-config.hxx>
#include <string>

// The database this service owns: identity's file until row 3c-2 splits the
// tables it holds into sync.db.
struct SyncDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

// The control plane the producers call: unary gRPC, gated by the fleet secret.
struct SyncControlConfig
{
  GrpcListenerConfig listener;
  std::string secret;

  // A non-loopback listener can inject frames into any user's room: must be authed.
  [[nodiscard]] bool reachableBeyondLoopback() const;
};

// The domain services whose tables /sync pages.
struct SyncUpstreams
{
  std::string camera;
  std::string productivity;
  std::string notification;
};

class SyncConfig
{
public:
  static SyncDbConfig resolveDb();

  // The TLS listener /sync terminates on: the gateway's own block re-keyed to [sync].
  static ListenerConfig resolveListener();

  static SyncControlConfig resolveControl();
  static SyncUpstreams resolveUpstreams();
};
