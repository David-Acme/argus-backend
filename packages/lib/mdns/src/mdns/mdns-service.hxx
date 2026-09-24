#pragma once

#include <cstdint>
#include <json/value.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct MdnsInstance
{
  std::string serviceType;
  std::string path;
  uint16_t port{0};
  std::vector<std::pair<std::string, std::string>> txt;
};

class MdnsService
{
public:
  explicit MdnsService(std::vector<MdnsInstance> instances);
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
