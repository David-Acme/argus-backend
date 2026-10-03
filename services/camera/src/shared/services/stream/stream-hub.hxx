#pragma once

#include <shared/services/stream/gop-cache.hxx>

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
  struct StreamClosedInput
  {
    uint16_t subId{0};
    std::string reason;
  };

  class ISink
  {
  public:
    virtual ~ISink() = default;
    virtual bool sendBinary(const uint8_t* data, size_t len) = 0;
    virtual bool tryReserve(size_t bytes) = 0;
    virtual void release(int64_t bytes) = 0;
    virtual void onClosed(const StreamClosedInput& input) = 0;
  };

  static constexpr int kDefaultViewersPerCamera = 4;
  static constexpr int kDefaultTotalViewers = 8;

  struct ViewerLimits
  {
    int perCamera{kDefaultViewersPerCamera};
    int total{kDefaultTotalViewers};
  };

  struct SubscribeInput
  {
    std::shared_ptr<ISink> sink;
    int64_t cameraId{0};
    std::string quality;
    bool fastStart{false};
  };

  static constexpr size_t kDefaultGopCacheBytes = size_t{2} * 1024 * 1024;

  StreamHub() = default;
  ~StreamHub();

  StreamHub(const StreamHub&) = delete;
  StreamHub& operator=(const StreamHub&) = delete;

  static StreamHub& instance();

  void init();
  void shutdown();
  void refreshViewerLimits();
  [[nodiscard]] ViewerLimits viewerLimits();

  uint16_t subscribe(const SubscribeInput& input, std::string& error);
  void ack(uint16_t subId, int64_t bytes);
  void unsubscribe(uint16_t subId, const ISink* owner);
  int subscriptionsOf(const ISink* sink);
  void closeAll(const ISink* sink);
  int activeUpstreams();
  int activeSubscribers();

private:
  struct Subscriber
  {
    uint16_t subId{0};
    std::shared_ptr<ISink> sink;
    bool sentInit{false};
    bool skipUntilKeyframe{true};
    uint32_t seq{0};
  };

  struct Upstream
  {
    explicit Upstream(size_t gopCacheBytes) : gop(gopCacheBytes) {}

    std::string name;
    int64_t cameraId{0};
    std::mutex mtx;
    std::vector<std::shared_ptr<Subscriber>> subs;
    std::string init;
    bool hasInit{false};
    GopCache gop;
    std::atomic<int> fd{-1};
    std::atomic<bool> stopping{false};
    std::atomic<bool> dead{false};
    std::thread reader;
  };

  struct SendFramedInput
  {
    const std::shared_ptr<Subscriber>& sub;
    uint8_t type{0};
    bool keyframe{false};
    const uint8_t* data{nullptr};
    size_t len{0};
  };

  struct DeliverInput
  {
    Upstream& up;
    const std::shared_ptr<Subscriber>& sub;
    const CachedFragment& fragment;
    bool reserved{false};
  };

  std::shared_ptr<Upstream> getOrOpen(const SubscribeInput& input,
                                      std::string& error);
  void pruneLocked();
  void countViewers(int64_t cameraId, int& perCamera, int& total);
  static void sendFramed(const SendFramedInput& input);
  void deliver(const DeliverInput& input);
  void dispatch(Upstream& up, const CachedFragment& fragment);
  void replayGop(Upstream& up, const std::shared_ptr<Subscriber>& sub);
  void runUpstream(std::shared_ptr<Upstream> up);

  std::mutex hubMutex_;
  std::unordered_map<std::string, std::shared_ptr<Upstream>> upstreams_;
  std::unordered_map<uint16_t, std::shared_ptr<Upstream>> subToUpstream_;
  uint16_t nextSubId_ = 1;
  uint32_t nextSeq_ = 0;
  size_t chunkBytes_ = 16 * 1024;
  int64_t graceMs_ = 2000;
  size_t gopCacheBytes_ = kDefaultGopCacheBytes;
  int maxViewersPerCamera_ = kDefaultViewersPerCamera;
  int maxTotalViewers_ = kDefaultTotalViewers;
};
