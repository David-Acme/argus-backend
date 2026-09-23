#pragma once

#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <string>

class DbService
{
public:
  static drogon::orm::DbClientPtr client();

  static void freezeClient(const std::string& dbPath);

  static drogon::orm::DbClientPtr readOnlyClient();

  static void setReadOnlyClient(drogon::orm::DbClientPtr client);

  static drogon::orm::DbClientPtr identityClient();

  static void setIdentityClient(drogon::orm::DbClientPtr client);

  static drogon::orm::DbClientPtr cameraClient();

  static void setCameraClient(drogon::orm::DbClientPtr client);

  static drogon::orm::DbClientPtr productivityClient();

  static void setProductivityClient(drogon::orm::DbClientPtr client);

  static drogon::orm::DbClientPtr gatewayClient();

  static void setGatewayClient(drogon::orm::DbClientPtr client);

  static void enableUriFilenames();

  static bool runScriptFile(const std::string& path,
                            drogon::orm::DbClientPtr client = nullptr);
  static void applyPragmas(drogon::orm::DbClientPtr client = nullptr);
  static void installExtensions();
};
