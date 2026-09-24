#pragma once

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>

#include <exception>
#include <functional>
#include <nats/nats-bus.hxx>
#include <string>
#include <trantor/utils/Logger.h>
#include <utility>

namespace ordered_delivery
{
using Runner = std::function<drogon::Task<void>(const std::string& body)>;

inline NatsBus::DurableHandler handler(std::string label, Runner runner)
{
  return [label = std::move(label),
          runner = std::move(runner)](const NatsBus::DurableMessage& message,
                                      NatsBus::DurableSettlement settlement) {
    std::string body(message.payload);
    drogon::app().getIOLoop(0)->runInLoop(
        [label, runner, body = std::move(body),
         settlement = std::move(settlement)]() mutable {
          drogon::async_run([label, runner, body = std::move(body),
                             settlement = std::move(settlement)]() mutable
                            -> drogon::Task<void> {
            bool applied = true;
            try {
              co_await runner(body);
            }
            catch (const std::exception& error) {
              applied = false;
              LOG_WARN << label << ": redelivering (" << error.what() << ")";
            }
            catch (...) {
              applied = false;
              LOG_WARN << label << ": redelivering (a failure that is not a "
                                  "std::exception)";
            }
            if (applied) {
              if (settlement.ack)
                settlement.ack();
            }
            else if (settlement.nak) {
              settlement.nak();
            }
          });
        });
  };
}
}
