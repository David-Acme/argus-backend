#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class StreamHub
{
public:
  class ISink
  {
  public:
    virtual ~ISink() = default;
    virtual bool sendBinary(const uint8_t* data, size_t len) = 0;
    virtual void onClosed(const std::string& reason) = 0;
  };

  struct SubscribeInput
  {
    std::shared_ptr<ISink> sink;
    int64_t cameraId{0};
    std::string quality;
  };

  StreamHub() = delete;
  ~StreamHub() = delete;

  static void init();
  static void shutdown();

  static uint16_t subscribe(const SubscribeInput& input, std::string& error);
  static void ack(uint16_t subId, int64_t bytes);
  static void unsubscribe(uint16_t subId);
  static void closeAll(const ISink* sink);
  static int activeUpstreams();
  static int activeSubscribers();

private:
  struct Subscriber
  {
    uint16_t subId{0};
    std::shared_ptr<ISink> sink;
    int64_t bytesInFlight{0};
    bool sentInit{false};
    bool skipUntilKeyframe{true};
    uint32_t seq{0};
  };

  struct Upstream
  {
    std::string name;
    std::mutex mtx;
    std::vector<std::shared_ptr<Subscriber>> subs;
    std::string init;
    bool hasInit{false};
    std::atomic<int> fd{-1};
    std::atomic<bool> stopping{false};
    std::thread reader;
  };

  static std::shared_ptr<Upstream> getOrOpen(const SubscribeInput& input,
                                             std::string& error);
  static void sendFramed(const std::shared_ptr<Subscriber>& sub, uint8_t type,
                         bool keyframe, const uint8_t* data, size_t len);
  static void sendBox(const std::shared_ptr<Subscriber>& sub,
                      const std::string& box, bool keyframe);
  static void dispatchBox(Upstream& up, std::string box, bool keyframe);
  static void runUpstream(std::shared_ptr<Upstream> up);

  static std::mutex hubMutex_;
  static std::unordered_map<std::string, std::shared_ptr<Upstream>> upstreams_;
  static std::unordered_map<uint16_t, std::shared_ptr<Upstream>> subToUpstream_;
  static uint16_t nextSubId_;
  static uint32_t nextSeq_;
  static int64_t windowBytes_;
  static size_t chunkBytes_;
  static int64_t graceMs_;
};
