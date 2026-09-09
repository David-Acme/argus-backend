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

  // Read path of the sync tables that live outside the host's own database.
  static drogon::orm::DbClientPtr readOnlyClient();

  // Installs the named read-only client; once at boot, before app().run().
  static void setReadOnlyClient(drogon::orm::DbClientPtr client);

  // Client of the identity database for the auth reads.
  static drogon::orm::DbClientPtr identityClient();

  // Installs the named identity client; once at boot, before app().run().
  static void setIdentityClient(drogon::orm::DbClientPtr client);

  // Client of the camera domain (camera, camera_stream, zone).
  static drogon::orm::DbClientPtr cameraClient();

  // Installs the named camera client; once at boot, before app().run().
  static void setCameraClient(drogon::orm::DbClientPtr client);

  // Client of the productivity domain (reminder, project, calendar tables).
  static drogon::orm::DbClientPtr productivityClient();

  // Installs the named productivity client; once at boot, before app().run().
  static void setProductivityClient(drogon::orm::DbClientPtr client);

  // Client of the notification domain (notification, notification_token).
  static drogon::orm::DbClientPtr notificationClient();

  // Installs the named notification client; once at boot, before app().run().
  static void setNotificationClient(drogon::orm::DbClientPtr client);

  // Enables SQLite URI filenames process-wide, before the first sqlite3_open.
  static void enableUriFilenames();

  static bool runScriptFile(const std::string& path);
  static void applyPragmas();
  static void installExtensions();
};
