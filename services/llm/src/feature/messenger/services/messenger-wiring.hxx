#pragma once

#include <feature/conversation/services/conversation-service.hxx>
#include <feature/messenger/services/messenger-store.hxx>
#include <feature/messenger/services/relay-service.hxx>
#include <feature/messenger/vocabulary/messenger-settings.hxx>
#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>

struct MessengerWiringInput
{
  std::shared_ptr<const MessengerStore> store;
  std::shared_ptr<const NotificationClient> notifications;
  std::shared_ptr<const IdentityClient> identity;
  std::shared_ptr<const ConversationService> conversations;
  MessengerSettings settings;
};

struct MessengerWiring
{
  tools::ToolDescriptor tool;
  std::shared_ptr<const RelayService> service;
};

[[nodiscard]] MessengerWiring composeMessenger(const MessengerWiringInput& input);
