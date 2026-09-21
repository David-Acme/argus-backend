#pragma once

#include <json/value.h>
#include <memory>

class MdnsService
{
public:
  MdnsService();
  ~MdnsService();

  MdnsService(const MdnsService&) = delete;
  MdnsService& operator=(const MdnsService&) = delete;

  bool initialize();
  bool isAdvertising() const;
  void shutdown();
  Json::Value health() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
