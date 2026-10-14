#pragma once

#include <app/rpc/identity-callers.hxx>
#include <app/rpc/identity-rpc-service.hxx>
#include <argus/identity/v1/identity.grpc.pb.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <drogon/drogon.h>
#include <fstream>
#include <google/protobuf/message.h>
#include <google/protobuf/util/json_util.h>
#include <grpc/fleet-caller-gate.hxx>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <json/json.h>
#include <memory>
#include <optional>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <sstream>
#include <stdexcept>
#include <string>
#include <test-support/app-runner.hxx>
#include <thread>
#include <utility>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

#ifndef ARGUS_IDENTITY_GOLDEN_FILE
#error "ARGUS_IDENTITY_GOLDEN_FILE must point at the recorded identity answers"
#endif

inline constexpr const char* kIdentityGoldenDb = "identity-rpc-golden.db";

[[nodiscard]] inline std::string goldenSecretOf(const std::string& caller)
{
  return caller + std::string(48, 'g');
}

[[nodiscard]] inline std::shared_ptr<const argus::client::FleetCallerGate>
goldenGate()
{
  return std::make_shared<const argus::client::FleetCallerGate>(
      argus::client::FleetGateConfig{.expectedCallers =
                                         identity_callers::expected(),
                                     .callerPairs =
                                         {{"auth", goldenSecretOf("auth")},
                                          {"camera", goldenSecretOf("camera")},
                                          {"voice", goldenSecretOf("voice")}},
                                     .legacySecret = {},
                                     .onFirstLegacy = {}});
}

inline void seedGoldenRows()
{
  const auto client = DbService::identityClient();
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (1, 'Ada', 'Owner', 'owner', 'es', 1, 1000)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (7, 'Rita', 'Resident', 'resident', 'en', 1, 1001)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (9, 'Gus', 'Guest', 'guest', 'es', 0, 1003)");
  client->execSqlSync(
      "INSERT INTO user (id, name, last_name, role, lang, is_active, "
      "created_at) VALUES (12, 'Juan', 'Janitor', 'janitor', 'es', 1, 1004)");
}

class IdentityGoldenApp
{
public:
  IdentityGoldenApp()
  {
    for (const char* suffix : {"", "-wal", "-shm"})
      std::remove((std::string(kIdentityGoldenDb) + suffix).c_str());
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                   .filename = kIdentityGoldenDb,
                                   .name = "default",
                                   .timeout = -1});
    runner_ = std::make_unique<test_support::AppRunner>();
    if (!drogon::app().isRunning())
      throw std::runtime_error("the drogon loop did not start");
    if (!DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA))
      throw std::runtime_error("the identity schema did not apply");
    seedGoldenRows();
    FaceService::instance().disable();
  }

private:
  std::unique_ptr<test_support::AppRunner> runner_;
};

struct GoldenAnswer
{
  std::string name;
  Json::Value answer;
};

[[nodiscard]] inline Json::Value
messageJson(const google::protobuf::Message& message)
{
  std::string text;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  if (!google::protobuf::util::MessageToJsonString(message, &text, options)
           .ok())
    throw std::runtime_error("an identity answer did not print as JSON");
  Json::Value parsed;
  std::string errors;
  const std::unique_ptr<Json::CharReader> reader(
      Json::CharReaderBuilder().newCharReader());
  if (!reader->parse(text.data(), text.data() + text.size(), &parsed, &errors))
    throw std::runtime_error("an identity answer did not parse: " + errors);
  return parsed;
}

[[nodiscard]] inline Json::Value
answerOf(const grpc::Status& status, const google::protobuf::Message& response)
{
  Json::Value answer(Json::objectValue);
  answer["status"] = static_cast<int>(status.error_code());
  if (status.ok())
    answer["response"] = messageJson(response);
  else
    answer["message"] = status.error_message();
  return answer;
}

struct IdentifyPersonInput
{
  std::string caller;
  std::string image;
};

class GoldenCaller
{
public:
  explicit GoldenCaller(const std::string& target)
      : stub_(argus::identity::v1::IdentityService::NewStub(
            argus::client::makeChannel(target)))
  {
  }

  [[nodiscard]] Json::Value getUser(const std::optional<std::string>& caller,
                                    int64_t userId) const
  {
    grpc::ClientContext context;
    prepare(context, caller);
    argus::identity::v1::GetUserRequest request;
    request.set_user_id(userId);
    argus::identity::v1::GetUserResponse response;
    return answerOf(stub_->GetUser(&context, request, &response), response);
  }

  [[nodiscard]] Json::Value listUsers(const std::string& caller) const
  {
    grpc::ClientContext context;
    prepare(context, caller);
    const argus::identity::v1::ListUsersRequest request;
    argus::identity::v1::ListUsersResponse response;
    return answerOf(stub_->ListUsers(&context, request, &response), response);
  }

  [[nodiscard]] Json::Value identifyPerson(const IdentifyPersonInput& input) const
  {
    grpc::ClientContext context;
    prepare(context, input.caller);
    argus::identity::v1::IdentifyPersonRequest request;
    request.set_image(input.image);
    argus::identity::v1::IdentifyPersonResponse response;
    return answerOf(stub_->IdentifyPerson(&context, request, &response),
                    response);
  }

private:
  static void prepare(grpc::ClientContext& context,
                      const std::optional<std::string>& caller)
  {
    argus::client::setDeadline(context, 5000);
    if (caller)
      argus::client::addPeerCredential(context,
                                       {.credential = goldenSecretOf(*caller),
                                        .fleetSecret = {}});
  }

  std::unique_ptr<argus::identity::v1::IdentityService::Stub> stub_;
};

[[nodiscard]] inline std::vector<GoldenAnswer>
askIdentityGolden(const std::string& target)
{
  const GoldenCaller ask(target);
  return {
      {.name = "get-user-owner", .answer = ask.getUser("auth", 1)},
      {.name = "get-user-resident", .answer = ask.getUser("auth", 7)},
      {.name = "get-user-inactive-guest", .answer = ask.getUser("auth", 9)},
      {.name = "get-user-unknown-role", .answer = ask.getUser("auth", 12)},
      {.name = "get-user-missing", .answer = ask.getUser("auth", 999)},
      {.name = "get-user-invalid-id", .answer = ask.getUser("auth", 0)},
      {.name = "get-user-without-credential",
       .answer = ask.getUser(std::nullopt, 1)},
      {.name = "list-users", .answer = ask.listUsers("auth")},
      {.name = "identify-person-without-image",
       .answer = ask.identifyPerson({.caller = "auth", .image = ""})},
      {.name = "identify-person-not-an-image",
       .answer = ask.identifyPerson({.caller = "auth", .image = "not an image"})},
      {.name = "identify-person-by-a-caller-it-refuses",
       .answer = ask.identifyPerson({.caller = "voice", .image = "not an image"})},
  };
}

[[nodiscard]] inline Json::Value identityGoldenFile()
{
  std::ifstream file(ARGUS_IDENTITY_GOLDEN_FILE);
  if (!file)
    throw std::runtime_error("cannot read " +
                             std::string(ARGUS_IDENTITY_GOLDEN_FILE));
  Json::Value golden;
  std::string errors;
  if (!Json::parseFromStream(Json::CharReaderBuilder(), file, &golden, &errors))
    throw std::runtime_error("the golden file does not parse: " + errors);
  return golden;
}

[[nodiscard]] inline bool recordingIdentityGolden()
{
  const char* value = std::getenv("ARGUS_IDENTITY_GOLDEN_RECORD");
  return value != nullptr && std::string(value) == "1";
}

inline void recordIdentityGolden(const std::vector<GoldenAnswer>& answers)
{
  Json::Value golden(Json::objectValue);
  for (const auto& answer : answers)
    golden[answer.name] = answer.answer;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  std::istringstream lines(Json::writeString(writer, golden));
  std::ofstream file(ARGUS_IDENTITY_GOLDEN_FILE, std::ios::trunc);
  for (std::string line; std::getline(lines, line);) {
    line.erase(line.find_last_not_of(' ') + 1);
    file << line << '\n';
  }
}
