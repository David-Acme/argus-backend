#include "delivery-page.hxx"

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
  }
  return page;
}
