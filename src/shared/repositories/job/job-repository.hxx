#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared/repositories/job/job-query.hxx>
#include <sqlite3.h>
#include <string>
#include <vector>

class JobRepository
{
public:
  JobRepository() = default;
  ~JobRepository() = default;

  JobRepository(const JobRepository&) = delete;
  JobRepository& operator=(const JobRepository&) = delete;

  bool open(const std::string& dbPath);
  void close();

  std::optional<int64_t> insert(const job_query::JobInsertInput& input);
  std::optional<int64_t> findIdByDedupe(const std::string& queue,
                                        const std::string& dedupeKey);
  std::vector<job_query::JobRow> loadDue(const std::string& queue, int64_t now);
  bool markActive(int64_t id, int64_t now);
  bool markDelayed(int64_t id, int attempts, const std::string& error,
                   int64_t nextRunAt, int64_t now);
  bool markFailed(int64_t id, int attempts, const std::string& error,
                  int64_t now);
  bool complete(int64_t id);
  int resetActive(int64_t now);
  int countByState(const std::string& queue, const std::string& state);

private:
  std::unique_ptr<sqlite3, int (*)(sqlite3*)> db_{nullptr, &sqlite3_close};
  std::mutex mutex_;
};
