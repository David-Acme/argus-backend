#pragma once

#include <cstdint>

struct BeliefConfig
{
  int weightDetectorStrong{2};
  int weightDetectorWeak{-2};
  int weightPersistenceMet{2};
  int weightPersistenceShort{-2};
  int weightTrackStable{1};
  int weightTrackJitter{-2};
  int weightIdentityUnrecognized{2};
  int weightIdentityUnobservable{-2};
  int weightIdentityUnavailable{0};
  int weightIdentityKnown{-3};
  int weightCameraHealthDegraded{-2};
  double detectorStrong{0.75};
  double detectorWeak{0.35};
  int minScoreSamples{1};
  int persistenceWindows{2};
  double persistenceDwellFraction{0.5};
  int64_t zoneDwellAlertMs{3000};
  int64_t zoneDwellMonitorMs{12000};
  int64_t trackStableAgeMs{4000};
  int64_t trackJitterDwellMs{1500};
  double areaSpreadRatio{2.0};
  int thresholdCritical{1};
  int thresholdHigh{3};
  int thresholdMedium{5};
  int thresholdLow{7};
};
