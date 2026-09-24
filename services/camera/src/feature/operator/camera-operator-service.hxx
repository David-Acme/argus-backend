#pragma once

#include <feature/objects/object-detector.hxx>
#include <feature/operator/event-intelligence.hxx>
#include <feature/operator/frame-source.hxx>
#include <feature/operator/object-event-sink.hxx>
#include <feature/operator/operator-config.hxx>
#include <feature/operator/zone-source.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cv
{
class Mat;
}

struct CameraRef
{
  int64_t id{0};
  std::string name;
};

class CameraOperatorService
{
public:
  struct Dependencies
  {
    IObjectDetector* detector{nullptr};
    IFrameSource* source{nullptr};
    IObjectEventSink* sink{nullptr};
    const IKnownPersonMatcher* matcher{nullptr};
    IZoneSource* zones{nullptr};
  };

  struct Inputs
  {
    Dependencies dependencies;
    ObjectsConfig objects;
    OperatorConfig operator_;
  };

  explicit CameraOperatorService(Inputs inputs);

  void start();

  void requestStop();

  [[nodiscard]] bool drained() const;

  [[nodiscard]] bool running() const { return running_.load(); }

  struct ProcessFrameInput
  {
    int64_t cameraId{0};
    const std::string& cameraName;
    CameraFrame& frame;
  };

  void processFrame(const ProcessFrameInput& input);

private:
  struct BoxTrack
  {
    float x{0};
    float y{0};
    float w{0};
    float h{0};
    int frames{0};
  };

  struct TrackWindowSample
  {
    float confidence{0.0F};
    float area{0.0F};
  };

  struct PersonTrack
  {
    int64_t id{0};
    float x{0};
    float y{0};
    float w{0};
    float h{0};
    int64_t firstSeenMs{0};
    int64_t lastSeenMs{0};
    int64_t lastEmitMs{0};
    int staticFrames{0};
    int trackWindows{0};
    int zoneWindows{0};
    std::vector<TrackWindowSample> windowHistory;
    double bestScore{0.0};
    std::string signature;
  };

  struct PersonTrackDwell
  {
    int64_t trackId{0};
    int64_t dwellMs{0};
    float confidence{0.0F};
    bool inAlertZone{false};
    bool due{false};
    bool canEmit{false};
  };

  struct PersonDwell
  {
    bool personPresent{false};
    bool dwelling{false};
    std::vector<PersonTrackDwell> tracks;
  };

  struct CameraState
  {
    std::map<int64_t, ObjectDetectedEvent> pendingPersons;
    std::map<int64_t, int64_t> pendingPersonStartMs;
    std::optional<ObjectDetectedEvent> pendingOther;
    int64_t pendingOtherStartMs{0};
    int64_t nextEventSeq{1};
    int64_t vehicleLastSeenMs{0};
    int presenceStreak{0};
    std::vector<uint8_t> motionGray;
    std::map<std::string, std::vector<BoxTrack>> boxTracks;
    std::map<int64_t, PersonTrack> personTracks;
    int64_t nextPersonTrackId{1};
    int64_t burstUntilMs{0};
  };

  struct PersonTrackInput
  {
    CameraState& state;
    std::vector<DetectedObject>& objects;
    const std::vector<OperatorZone>& zones;
    int64_t stamp{0};
    bool night{false};
    int frameWidth{0};
    int frameHeight{0};
  };

  struct PersonPendingInput
  {
    int64_t cameraId{0};
    const std::string& cameraName;
    CameraState& state;
    const EventIntelligenceOutcome& outcome;
    const std::vector<DetectedObject>& objects;
    int64_t stamp{0};
    int frameWidth{0};
    int frameHeight{0};
    int64_t trackId{0};
    int64_t dwellMs{0};
  };

  struct PublishPersonInput
  {
    int64_t cameraId{0};
    CameraState& state;
    int64_t trackId{0};
    int64_t nowMs{0};
  };

  struct PublishOtherInput
  {
    int64_t cameraId{0};
    CameraState& state;
    int64_t nowMs{0};
  };

  struct MergeObjectInput
  {
    ObjectDetectedEvent& event;
    const EvaluatedObject& evaluated;
    CameraState& state;
    int64_t stamp{0};
  };

  static double trackScoreMedian(const std::vector<TrackWindowSample>& history);
  static double trackAreaSpread(const std::vector<TrackWindowSample>& history);

  drogon::Task<void> runCamera(CameraRef camera,
                               std::shared_ptr<std::atomic<bool>> stop);
  drogon::Task<void> supervise();
  void rescan();

  bool isNightHour(int hour) const;

  bool cameraHasMotion(const cv::Mat& rgb, CameraState& state) const;

  std::vector<bool> staticBoxMask(
      CameraState& state, const std::vector<DetectedObject>& objects) const;

  PersonDwell updatePersonTracks(const PersonTrackInput& input) const;

  double currentInferenceFps(int64_t cameraId);

  void mergePersonPending(const PersonPendingInput& input);

  void mergeObject(const MergeObjectInput& input);

  void publishPersonPending(const PublishPersonInput& input);

  void publishOtherPending(const PublishOtherInput& input);

  Inputs inputs_;
  std::atomic<bool> running_{false};
  std::atomic<int64_t> inFlight_{0};
  std::mutex stateMutex_;
  std::map<int64_t, CameraState> states_;
  std::mutex camerasMutex_;
  std::map<int64_t, std::shared_ptr<std::atomic<bool>>> cameraStop_;
};
