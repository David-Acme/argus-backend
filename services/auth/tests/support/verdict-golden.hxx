#pragma once

#include <argus/auth/v1/auth.grpc.pb.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <google/protobuf/util/json_util.h>
#include <grpc/grpc-client-base.hxx>
#include <grpcpp/grpcpp.h>
#include <iterator>
#include <json/json.h>
#include <memory>
#include <session-fixture.hxx>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef ARGUS_AUTH_GOLDEN_FILE
#error "ARGUS_AUTH_GOLDEN_FILE must point at the recorded ValidateToken answers"
#endif

inline constexpr int64_t kGoldenLiveExpiry = 4102444800;
inline constexpr int64_t kGoldenPastExpiry = 946684800;

struct GoldenVerdict
{
  std::string name;
  Json::Value response;
};

[[nodiscard]] inline Json::Value
responseJson(const argus::auth::v1::ValidateTokenResponse& response)
{
  std::string text;
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  if (!google::protobuf::util::MessageToJsonString(response, &text, options)
           .ok())
    throw std::runtime_error("a ValidateToken answer did not print as JSON");
  Json::Value parsed;
  std::string errors;
  const std::unique_ptr<Json::CharReader> reader(
      Json::CharReaderBuilder().newCharReader());
  if (!reader->parse(text.data(), text.data() + text.size(), &parsed, &errors))
    throw std::runtime_error("a ValidateToken answer did not parse: " + errors);
  return parsed;
}

struct GoldenRequest
{
  std::string accessToken;
  std::string deviceHash;
};

[[nodiscard]] inline argus::auth::v1::ValidateTokenResponse
validateOver(const std::string& target, const GoldenRequest& golden)
{
  const auto stub =
      argus::auth::v1::AuthService::NewStub(argus::client::makeChannel(target));
  grpc::ClientContext context;
  argus::client::setDeadline(context, 5000);
  argus::auth::v1::ValidateTokenRequest request;
  request.set_access_token(golden.accessToken);
  request.set_device_hash(golden.deviceHash);
  argus::auth::v1::ValidateTokenResponse response;
  const grpc::Status status = stub->ValidateToken(&context, request, &response);
  if (!status.ok())
    throw std::runtime_error("ValidateToken failed: " + status.error_message());
  return response;
}

inline void resetGoldenIdentity(ScriptedIdentityClient& identity)
{
  identity.reachable = true;
  identity.isActive = true;
  identity.name = "Ada";
  identity.lastName = "Rico";
  identity.lang = "es";
  identity.role = "owner";
}

struct GoldenSeed
{
  int64_t expiresAt{0};
  std::string sessionId;
};

[[nodiscard]] inline std::string seedGoldenSession(const GoldenSeed& seed)
{
  const std::string token = seedSession({.expiresAt = seed.expiresAt});
  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET session_id = ? WHERE access_token = ?",
      seed.sessionId, argus::hash::sha256Hex(token));
  return token;
}

[[nodiscard]] inline std::vector<GoldenVerdict>
askGoldenVerdicts(const std::string& target, ScriptedIdentityClient& identity)
{
  resetGoldenIdentity(identity);
  const std::string live = seedGoldenSession(
      {.expiresAt = kGoldenLiveExpiry, .sessionId = "golden-live-session"});
  const std::string expired = seedGoldenSession(
      {.expiresAt = kGoldenPastExpiry, .sessionId = "golden-expired-session"});
  const std::string revoked = seedGoldenSession(
      {.expiresAt = kGoldenLiveExpiry, .sessionId = "golden-revoked-session"});
  DbService::client()->execSqlSync(
      "UPDATE refresh_token SET is_valid = 0 WHERE access_token = ?",
      argus::hash::sha256Hex(revoked));
  const std::string unknown = issueToken(kUserId);

  std::vector<GoldenVerdict> verdicts;
  const auto answer = [&](std::string name, const GoldenRequest& request) {
    verdicts.push_back(
        {.name = std::move(name),
         .response = responseJson(validateOver(target, request))});
  };
  answer("valid-session", {.accessToken = live, .deviceHash = kDeviceHash});
  answer("expired-session",
         {.accessToken = expired, .deviceHash = kDeviceHash});
  answer("revoked-session",
         {.accessToken = revoked, .deviceHash = kDeviceHash});
  answer("wrong-device", {.accessToken = live, .deviceHash = kOtherDeviceHash});
  answer("unknown-token", {.accessToken = unknown, .deviceHash = kDeviceHash});
  identity.isActive = false;
  answer("deactivated-account",
         {.accessToken = live, .deviceHash = kDeviceHash});
  resetGoldenIdentity(identity);
  return verdicts;
}

[[nodiscard]] inline Json::Value goldenFile()
{
  std::ifstream file(ARGUS_AUTH_GOLDEN_FILE);
  if (!file)
    throw std::runtime_error("cannot read " +
                             std::string(ARGUS_AUTH_GOLDEN_FILE));
  Json::Value golden;
  std::string errors;
  if (!Json::parseFromStream(Json::CharReaderBuilder(), file, &golden, &errors))
    throw std::runtime_error("the golden file does not parse: " + errors);
  return golden;
}

[[nodiscard]] inline bool recordingGolden()
{
  const char* value = std::getenv("ARGUS_AUTH_GOLDEN_RECORD");
  return value != nullptr && std::string(value) == "1";
}

inline void recordGolden(const std::vector<GoldenVerdict>& verdicts)
{
  Json::Value golden(Json::objectValue);
  for (const auto& verdict : verdicts)
    golden[verdict.name] = verdict.response;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  std::istringstream lines(Json::writeString(writer, golden));
  std::ofstream file(ARGUS_AUTH_GOLDEN_FILE, std::ios::trunc);
  for (std::string line; std::getline(lines, line);) {
    line.erase(line.find_last_not_of(' ') + 1);
    file << line << '\n';
  }
}
