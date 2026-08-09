#pragma once

#include <mutex>

struct sqlite3;

class VecDb
{
public:
  VecDb() = delete;
  ~VecDb() = delete;

  static std::mutex& mutex();
  static sqlite3* handle();
};
