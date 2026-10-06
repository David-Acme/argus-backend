#include "range-server.hxx"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <charconv>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr int kPollMillis = 20;

bool sendAll(int connection, std::string_view bytes)
{
  while (!bytes.empty())
  {
    const auto sent = ::send(connection, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (sent <= 0)
      return false;
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

std::string lowered(std::string_view text)
{
  std::string out(text);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::optional<std::uint64_t> number(std::string_view text)
{
  std::uint64_t value = 0;
  const auto* const begin = std::to_address(text.begin());
  const auto* const end = std::to_address(text.end());
  const auto [ptr, error] = std::from_chars(begin, end, value);
  if (text.empty() || error != std::errc{} || ptr != end)
    return std::nullopt;
  return value;
}

ServedRequest parseRequest(std::string_view head)
{
  ServedRequest request;
  const auto lineEnd = head.find("\r\n");
  const auto requestLine = head.substr(0, lineEnd);
  const auto pathStart = requestLine.find(' ') + 1;
  request.path = std::string(requestLine.substr(pathStart, requestLine.find(' ', pathStart) - pathStart));
  std::size_t position = lineEnd + 2;
  while (position < head.size())
  {
    const auto next = head.find("\r\n", position);
    const auto line = head.substr(position, next - position);
    position = next == std::string_view::npos ? head.size() : next + 2;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || lowered(line.substr(0, colon)) != "range")
      continue;
    auto value = line.substr(colon + 1);
    value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
    constexpr std::string_view kUnit = "bytes=";
    if (!value.starts_with(kUnit))
      continue;
    value.remove_prefix(kUnit.size());
    const auto dash = value.find('-');
    request.rangeFirst = number(value.substr(0, dash));
    request.rangeLast = number(value.substr(dash + 1));
  }
  return request;
}

std::string head(std::string_view status, std::string_view extra, std::size_t length)
{
  return "HTTP/1.1 " + std::string(status) + "\r\nContent-Length: " + std::to_string(length) +
         "\r\nETag: \"v1\"\r\nConnection: keep-alive\r\n" + std::string(extra) + "\r\n";
}

}

RangeServer::RangeServer(RangeServerOptions options) : options_(std::move(options))
{
  listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener_ < 0)
    throw std::runtime_error("range server: socket");
  const int reuse = 1;
  ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listener_, 16) != 0)
    throw std::runtime_error("range server: bind");
  socklen_t length = sizeof(address);
  ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
  port_ = ntohs(address.sin_port);
  acceptor_ = std::thread([this] { acceptLoop(); });
}

RangeServer::~RangeServer()
{
  {
    const std::scoped_lock lock(mutex_);
    stopping_ = true;
    for (const int connection : connections_)
      ::shutdown(connection, SHUT_RDWR);
  }
  acceptor_.join();
  for (auto& worker : workers_)
    worker.join();
  ::close(listener_);
}

std::string RangeServer::url(std::string_view path) const
{
  return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
}

std::vector<ServedRequest> RangeServer::requests() const
{
  const std::scoped_lock lock(mutex_);
  return requests_;
}

std::vector<ServedRequest> RangeServer::fileRequests() const
{
  auto all = requests();
  std::erase_if(all, [](const ServedRequest& request) { return request.path != "/file"; });
  return all;
}

void RangeServer::acceptLoop()
{
  while (true)
  {
    {
      const std::scoped_lock lock(mutex_);
      if (stopping_)
        return;
    }
    pollfd watch{.fd = listener_, .events = POLLIN, .revents = 0};
    if (::poll(&watch, 1, kPollMillis) <= 0)
      continue;
    const int connection = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0)
      continue;
    const std::scoped_lock lock(mutex_);
    if (stopping_)
    {
      ::close(connection);
      return;
    }
    connections_.push_back(connection);
    workers_.emplace_back([this, connection] { serveConnection(connection); });
  }
}

void RangeServer::serveConnection(int connection)
{
  std::string buffer;
  std::array<char, 4096> block{};
  bool open = true;
  while (open)
  {
    const auto end = buffer.find("\r\n\r\n");
    if (end == std::string::npos)
    {
      const auto received = ::recv(connection, block.data(), block.size(), 0);
      if (received <= 0)
        break;
      buffer.append(block.data(), static_cast<std::size_t>(received));
      continue;
    }
    const auto request = parseRequest(std::string_view(buffer).substr(0, end));
    buffer.erase(0, end + 4);
    {
      const std::scoped_lock lock(mutex_);
      requests_.push_back(request);
    }
    open = answer(connection, request);
  }
  ::shutdown(connection, SHUT_RDWR);
  const std::scoped_lock lock(mutex_);
  std::erase(connections_, connection);
  ::close(connection);
}

bool RangeServer::answer(int connection, const ServedRequest& request)
{
  const auto& content = options_.content;
  if (request.path == "/redirect")
    return sendAll(connection, head("302 Found", "Location: /file\r\n", 0));
  if (request.path == "/absolute")
    return sendAll(connection, head("302 Found", "Location: " + url("/redirect") + "\r\n", 0));
  if (request.path == "/loop")
    return sendAll(connection, head("302 Found", "Location: /loop\r\n", 0));
  if (request.path == "/silent")
  {
    std::array<char, 64> sink{};
    while (::recv(connection, sink.data(), sink.size(), 0) > 0)
    {
    }
    return false;
  }
  if (request.path != "/file")
    return sendAll(connection, head("404 Not Found", "", 0));
  int index = 0;
  {
    const std::scoped_lock lock(mutex_);
    index = fileRequests_++;
  }
  const bool ranged = options_.honourRange && request.rangeFirst && request.rangeLast &&
                      *request.rangeFirst < content.size();
  std::string_view body = content;
  std::string response;
  if (ranged)
  {
    const auto last = std::min<std::uint64_t>(*request.rangeLast, content.size() - 1);
    body = body.substr(*request.rangeFirst, last - *request.rangeFirst + 1);
    response = head("206 Partial Content",
                    "Content-Range: bytes " + std::to_string(*request.rangeFirst) + "-" +
                        std::to_string(last) + "/" + std::to_string(content.size()) + "\r\n",
                    body.size());
  }
  else
    response = head("200 OK", "", body.size());
  if (index == options_.dropOnFileRequest)
  {
    sendAll(connection, response);
    sendAll(connection, body.substr(0, body.size() / 2));
    return false;
  }
  return sendAll(connection, response) && sendAll(connection, body);
}
