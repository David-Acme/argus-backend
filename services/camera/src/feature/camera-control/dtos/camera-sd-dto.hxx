#pragma once

#include <drogon/HttpRequest.h>

#include <string>

struct CameraSdDaysDto
{
  std::string from;
  std::string to;

  static CameraSdDaysDto fromRequest(const drogon::HttpRequestPtr& request);
};

struct CameraSdDayDto
{
  std::string date;

  static CameraSdDayDto fromRequest(const drogon::HttpRequestPtr& request);
};
