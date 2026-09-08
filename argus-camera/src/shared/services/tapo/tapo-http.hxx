#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct TapoHttpHeader
{
  std::string name;
  std::string value;
};

struct TapoEndpoint
{
  std::string host;
  int port{443};
  bool tls{true};
  int connectTimeoutMs{3000};
  int ioTimeoutMs{5000};
};

struct TapoHttpRequest
{
  TapoEndpoint endpoint;
  std::string method{"POST"};
  std::string path{"/"};
  std::vector<TapoHttpHeader> headers;
  std::string body;
};

struct TapoHttpResponse
{
  bool ok{false};
  int status{0};
  std::vector<TapoHttpHeader> headers;
  std::string body;
  std::string error;

  std::string header(const std::string& name) const;
};

class TapoConnection
{
public:
  TapoConnection();
  ~TapoConnection();

  TapoConnection(const TapoConnection&) = delete;
  TapoConnection& operator=(const TapoConnection&) = delete;

  bool open(const TapoEndpoint& endpoint);
  void close();
  bool isOpen() const;

  bool write(const std::string& data);
  bool readLine(std::string& line);
  bool readExactly(size_t count, std::string& out);
  bool readSome(std::string& out);

  const std::string& error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TapoHttp
{
public:
  static TapoHttpResponse send(const TapoHttpRequest& request);
  static std::string buildRequestHead(const TapoHttpRequest& request);
  static bool readResponseHead(TapoConnection& connection, TapoHttpResponse& response);
  static bool readResponseBody(TapoConnection& connection, TapoHttpResponse& response);
};
