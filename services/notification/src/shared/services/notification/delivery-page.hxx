#pragma once

#include <notification/notification-delivery-sink.hxx>
#include <nats/push-intent-sink.hxx>
#include <shared/repositories/notification/notification-query.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace delivery_page
{
inline constexpr int kPageSize = 200;
inline constexpr std::chrono::seconds kPublishMaxWait{2};
inline constexpr std::chrono::seconds kPublishBudget{20};
inline constexpr int64_t kClaimLeaseS = 30;

static_assert(kPublishBudget + kPublishMaxWait + std::chrono::seconds(5) <=
                  std::chrono::seconds(kClaimLeaseS),
              "a page must settle before its claims can be taken again");

struct PageInput
{
  std::vector<NotificationDeliveryRow> pending;
  std::shared_ptr<const NotificationDeliverySink> sink;
  std::shared_ptr<const push_intent::PushIntentSink> pushSink;
  std::chrono::steady_clock::time_point deadline;
  std::function<std::chrono::steady_clock::time_point()> clock;
};

struct PublishedPage
{
  std::vector<int64_t> sent;
  std::vector<int64_t> unsent;
};

PublishedPage publish(const PageInput& input);
}
