#include "livekit-room-client.hxx"

#include <feature/rtc/services/livekit-token.hxx>

#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <text/json-util.hxx>

#include <chrono>
#include <exception>
#include <utility>

LiveKitRoomClient::LiveKitRoomClient(LiveKitAdminConfig config) : config_(std::move(config)) {}

drogon::Task<std::optional<Json::Value>> LiveKitRoomClient::call(TwirpCall twirp) const
{
  const std::string token = mintLiveKitToken({.apiKey = config_.apiKey,
                                              .apiSecret = config_.apiSecret,
                                              .identity = {},
                                              .kind = {},
                                              .grant = {.room = twirp.room,
                                                        .roomJoin = false,
                                                        .roomAdmin = !twirp.room.empty(),
                                                        .roomList = twirp.list,
                                                        .canPublish = false,
                                                        .publishSources = {},
                                                        .canSubscribe = false,
                                                        .canPublishData = false,
                                                        .canUpdateOwnMetadata = false},
                                              .ttl = std::chrono::seconds(60),
                                              .now = std::chrono::system_clock::now()});
  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Post);
  request->setPath("/twirp/livekit.RoomService/" + twirp.method);
  request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  request->addHeader("Authorization", "Bearer " + token);
  request->setBody(json_util::toString(twirp.body));
  try {
    const auto client = drogon::HttpClient::newHttpClient(config_.serverUrl);
    const auto response = co_await client->sendRequestCoro(request, kRequestTimeoutSeconds);
    if (response->statusCode() != drogon::k200OK) {
      if (response->statusCode() != drogon::k404NotFound)
        LOG_WARN << "LiveKit " << twirp.method << " answered " << static_cast<int>(response->statusCode());
      co_return std::nullopt;
    }
    co_return json_util::fromString(std::string(response->body()));
  }
  catch (const std::exception& error) {
    LOG_WARN << "LiveKit " << twirp.method << " failed: " << error.what();
    co_return std::nullopt;
  }
}

drogon::Task<std::optional<std::vector<std::string>>> LiveKitRoomClient::listRooms() const
{
  const auto reply = co_await call({.method = "ListRooms",
                                    .body = Json::Value(Json::objectValue),
                                    .room = {},
                                    .list = true});
  if (!reply)
    co_return std::nullopt;
  std::vector<std::string> rooms;
  for (const auto& room : (*reply)["rooms"])
    if (room["name"].isString())
      rooms.push_back(room["name"].asString());
  co_return rooms;
}

drogon::Task<bool> LiveKitRoomClient::removeParticipant(LiveKitParticipantRef participant) const
{
  Json::Value body(Json::objectValue);
  body["room"] = participant.room;
  body["identity"] = participant.identity;
  co_return (co_await call({.method = "RemoveParticipant",
                            .body = std::move(body),
                            .room = participant.room,
                            .list = false}))
      .has_value();
}

drogon::Task<bool> LiveKitRoomClient::deleteRoom(std::string room) const
{
  Json::Value body(Json::objectValue);
  body["room"] = room;
  co_return (co_await call({.method = "DeleteRoom", .body = std::move(body), .room = room, .list = false}))
      .has_value();
}
