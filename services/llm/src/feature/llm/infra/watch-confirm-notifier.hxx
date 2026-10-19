#pragma once

#include <feature/llm/services/tools/tool-executor.hxx>

#include <memory>

class NotificationClient;

[[nodiscard]] CameraConfirmSink watchConfirmNotifier(std::shared_ptr<const NotificationClient> client);
