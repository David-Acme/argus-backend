#pragma once

#include <drogon/drogon.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace
{

class AppLoop
{
public:
  AppLoop()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    const auto uploads =
        std::filesystem::temp_directory_path() / "argus-vlm-rpc-test";
    std::error_code ignored;
    std::filesystem::create_directories(uploads, ignored);
    drogon::app().setUploadPath(uploads.string());
    drogon::app().addListener("127.0.0.1", 0);
    runner_ = std::thread([] { drogon::app().run(); });
    for (int tries = 0; tries < 1000 && !drogon::app().isRunning(); ++tries)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ~AppLoop()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppLoop(const AppLoop&) = delete;
  AppLoop& operator=(const AppLoop&) = delete;

  bool ready() const { return drogon::app().isRunning(); }

private:
  std::thread runner_;
};

AppLoop& appLoop()
{
  static AppLoop loop;
  return loop;
}
}
