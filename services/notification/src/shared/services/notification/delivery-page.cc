#include "delivery-page.hxx"

#include <shared/services/notification/push-copy.hxx>

namespace
{
void pushIntentFor(const delivery_page::PageInput& input,
                   const NotificationDeliveryRow& delivery)
{
  if (!input.pushSink || delivery.userId <= 0)
    return;
  const PushCopy copy =
      push_copy::render({.lang = push_copy::langOf(delivery.data),
                         .urgency = push_copy::urgencyOf(delivery.data),
                         .call = false});
  input.pushSink->publish(
      PushIntent{.userId = delivery.userId,
                 .notificationId = delivery.notificationId,
                 .type = delivery.type,
                 .title = copy.title,
                 .body = copy.body,
                 .createdAtMs = delivery.createdAt * 1000,
                 .data = push_copy::minimalData(
                     {.notificationId = delivery.notificationId,
                      .data = delivery.data})});
}
}

delivery_page::PublishedPage delivery_page::publish(const PageInput& input)
{
  PublishedPage page;
  page.sent.reserve(input.pending.size());
  bool stopped = false;
  for (const auto& delivery : input.pending) {
    if (!stopped && !page.sent.empty() && input.clock() >= input.deadline)
      stopped = true;
    if (stopped) {
      page.unsent.push_back(delivery.deliveryId);
      continue;
    }
    const NotificationDeliveryEvent event{
        .deliveryId = delivery.deliveryId,
        .notificationId = delivery.notificationId,
        .userId = delivery.userId,
        .type = delivery.type,
        .title = delivery.title,
        .body = delivery.body,
        .data = delivery.data,
        .createdAt = delivery.createdAt};
    if (!input.sink->publish(event)) {
      stopped = true;
      page.unsent.push_back(delivery.deliveryId);
      continue;
    }
    page.sent.push_back(delivery.deliveryId);
    pushIntentFor(input, delivery);
  }
  return page;
}
