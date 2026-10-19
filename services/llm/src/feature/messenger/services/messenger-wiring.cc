#include "messenger-wiring.hxx"

#include <feature/messenger/infra/conversation-inbound.hxx>
#include <feature/messenger/infra/identity-household-roster.hxx>
#include <feature/messenger/infra/notification-message-courier.hxx>
#include <feature/messenger/services/conversation-port.hxx>
#include <feature/messenger/services/relay-tool.hxx>

#include <mcp/confirmation.hxx>

MessengerWiring composeMessenger(const MessengerWiringInput& input)
{
  std::shared_ptr<const messenger::ConversationPort> conversation =
      input.conversations ? std::shared_ptr<const messenger::ConversationPort>(
                                std::make_shared<const ConversationInbound>(input.conversations))
                          : std::make_shared<const messenger::NoConversation>();
  auto service = std::make_shared<const RelayService>(RelayServiceDependencies{
      .conversation = std::move(conversation),
      .courier = std::make_shared<const NotificationMessageCourier>(input.notifications),
      .store = input.store,
      .settings = input.settings});
  auto roster = std::make_shared<const IdentityHouseholdRoster>(input.identity);
  return {.tool = relayTool({.service = service,
                             .roster = roster,
                             .ledger = std::make_shared<argus::mcp::ConfirmationLedger>()}),
          .service = service};
}
