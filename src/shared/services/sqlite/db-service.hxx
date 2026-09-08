#pragma once

#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <string>

class DbService
{
public:
  static drogon::orm::DbClientPtr client()
  {
    return drogon::app().getDbClient();
  }

  // Read path of the sync tables that stay in argus.db.
  static drogon::orm::DbClientPtr readOnlyClient();

  // Installs the named read-only sync client; call once at boot, before any IO thread exists.
  static void setReadOnlyClient(drogon::orm::DbClientPtr client);

  // Client of the identity database used by the auth reads.
  static drogon::orm::DbClientPtr identityClient();

  // Installs the named identity client; call once at boot, before any IO thread exists.
  static void setIdentityClient(drogon::orm::DbClientPtr client);

  // Client of the camera domain (camera, camera_stream, zone).
  static drogon::orm::DbClientPtr cameraClient();

  // Installs the named camera client; call once at boot, before any IO thread exists.
  static void setCameraClient(drogon::orm::DbClientPtr client);

  // Client of the productivity domain (reminder, calendar and project tables).
  static drogon::orm::DbClientPtr productivityClient();

  // Installs the named productivity client; call once at boot, before any IO thread exists.
  static void setProductivityClient(drogon::orm::DbClientPtr client);

  // Client of the notification domain (notification, notification_token); the gateway opens it read-write.
  static drogon::orm::DbClientPtr notificationClient();

  // Installs the named notification client; call once at boot, before any IO thread exists.
  static void setNotificationClient(drogon::orm::DbClientPtr client);

  // Enables SQLite URI filenames (`file:...?mode=ro`) process-wide; before the first sqlite3_open.
  static void enableUriFilenames();

  static bool runScriptFile(const std::string& path);
  static bool migrate(int64_t targetVersion);
  static void applyPragmas();
  static void installExtensions();
};
