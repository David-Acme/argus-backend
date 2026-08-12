#pragma once

#include <cstdint>
#include <functional>
#include <shared/enums.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <string>

struct JobOptions
{
  int priority = 0;
  int maxAttempts = 3;
  int backoffMs = 1000;
  int delayMs = 0;
  bool durable = true;
  std::string dedupeKey;
};

struct Job
{
  int64_t id = 0;
  std::string queue;
  std::string payload;
  int attempts = 0;
  int64_t createdAt = 0;
  CancellationToken cancel;
};

struct JobResult
{
  bool ok = true;
  std::string error;
};

struct QueueConfig
{
  std::string name;
  int workers = 0;
  int maxAttempts = 3;
  bool durable = true;
};

struct QueueStats
{
  size_t waiting = 0;
  size_t active = 0;
  size_t completed = 0;
  size_t failed = 0;
  size_t delayed = 0;
  size_t totalProcessed = 0;
};

using JobHandler = std::function<JobResult(const Job&)>;
