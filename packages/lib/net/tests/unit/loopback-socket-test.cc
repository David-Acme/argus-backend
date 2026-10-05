#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <net/loopback-socket.hxx>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <stop_token>
#include <string>
#include <thread>

using argus::net::Connection;
using argus::net::NetStatus;
using argus::net::Socket;

namespace
{

constexpr auto kTimeout = std::chrono::seconds(5);

class Listener
{
public:
  Listener() : socket_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
  {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool bound =
        ::bind(socket_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
        ::listen(socket_.get(), 4) == 0 &&
        ::getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0;
    port_ = bound ? ntohs(address.sin_port) : 0;
  }

  [[nodiscard]] int port() const { return port_; }

  [[nodiscard]] Socket accept() const
  {
    return Socket(::accept4(socket_.get(), nullptr, nullptr, SOCK_CLOEXEC));
  }

  void close() { socket_ = Socket{}; }

private:
  Socket socket_;
  int port_{0};
};

bool hasFlag(int flags, int flag)
{
  return flags >= 0 && (static_cast<unsigned>(flags) & static_cast<unsigned>(flag)) != 0;
}

Connection connectTo(int port)
{
  return argus::net::connectLoopback(
      {.host = "127.0.0.1", .port = port, .timeout = kTimeout, .cancellation = {}});
}

}

TEST_CASE("an endpoint is read from a base url the way every wire client wrote it")
{
  const auto full = argus::net::parseEndpoint("http://127.0.0.1:7031/llm/v1");
  CHECK(full.host == "127.0.0.1");
  CHECK(full.port == 7031);

  const auto bare = argus::net::parseEndpoint("127.0.0.1");
  CHECK(bare.host == "127.0.0.1");
  CHECK(bare.port == 80);

  const auto schemeOnly = argus::net::parseEndpoint("http://10.0.0.2:9");
  CHECK(schemeOnly.host == "10.0.0.2");
  CHECK(schemeOnly.port == 9);

  CHECK(argus::net::parseEndpoint("http://:8080").host.empty());
  CHECK_THROWS((void)argus::net::parseEndpoint("http://127.0.0.1:port"));
}

TEST_CASE("a connection is blocking, close-on-exec and carries a request and its answer")
{
  Listener listener;
  REQUIRE(listener.port() > 0);
  std::string request;
  std::thread peer([&listener, &request] {
    const Socket accepted = listener.accept();
    while (request.size() < 5 &&
           argus::net::receiveSome({.fd = accepted.get(), .into = request, .cancellation = {}}) ==
               NetStatus::Ok) {
    }
    const std::string answer(200000, 'a');
    CHECK(argus::net::sendAll({.fd = accepted.get(), .data = answer, .cancellation = {}}) ==
          NetStatus::Ok);
  });

  const Connection connection = connectTo(listener.port());
  REQUIRE(connection.connected());
  CHECK(connection.error == 0);
  const int fd = connection.socket.get();
  CHECK(hasFlag(::fcntl(fd, F_GETFD), FD_CLOEXEC));
  CHECK_FALSE(hasFlag(::fcntl(fd, F_GETFL), O_NONBLOCK));

  CHECK(argus::net::sendAll({.fd = fd, .data = "hello", .cancellation = {}}) == NetStatus::Ok);
  const auto read = argus::net::readUntilClosed(
      {.fd = fd, .deadline = std::chrono::steady_clock::now() + kTimeout, .cancellation = {}});
  peer.join();
  CHECK(request == "hello");
  CHECK(read.status == NetStatus::Closed);
  CHECK(read.data == std::string(200000, 'a'));
}

TEST_CASE("a closed port and a host that is not an address fail with their reason")
{
  Listener listener;
  const int port = listener.port();
  REQUIRE(port > 0);
  listener.close();

  const Connection refused = connectTo(port);
  CHECK(refused.status == NetStatus::Failed);
  CHECK(refused.error == ECONNREFUSED);
  CHECK_FALSE(refused.socket.valid());

  const Connection named = argus::net::connectLoopback(
      {.host = "localhost", .port = port, .timeout = kTimeout, .cancellation = {}});
  CHECK(named.status == NetStatus::Failed);
  CHECK(named.error == EINVAL);
}

TEST_CASE("a stop requested before any call answers cancelled without touching the wire")
{
  std::stop_source source;
  source.request_stop();
  CHECK(argus::net::connectLoopback({.host = "127.0.0.1",
                                     .port = 1,
                                     .timeout = kTimeout,
                                     .cancellation = source.get_token()})
            .status == NetStatus::Cancelled);

  Listener listener;
  REQUIRE(listener.port() > 0);
  const Connection connection = connectTo(listener.port());
  REQUIRE(connection.connected());
  const Socket accepted = listener.accept();
  std::string into;
  CHECK(argus::net::sendAll({.fd = connection.socket.get(),
                             .data = "x",
                             .cancellation = source.get_token()}) == NetStatus::Cancelled);
  CHECK(argus::net::receiveSome({.fd = connection.socket.get(),
                                 .into = into,
                                 .cancellation = source.get_token()}) == NetStatus::Cancelled);
  CHECK(into.empty());
}

TEST_CASE("a stop during a blocked receive wakes it as cancelled")
{
  Listener listener;
  REQUIRE(listener.port() > 0);
  const Connection connection = connectTo(listener.port());
  REQUIRE(connection.connected());
  const Socket accepted = listener.accept();

  std::stop_source source;
  const std::stop_callback wake(source.get_token(),
                                [&connection] { connection.socket.shutdown(); });
  std::thread stopper([&source] { source.request_stop(); });
  const auto read = argus::net::readUntilClosed(
      {.fd = connection.socket.get(),
       .deadline = std::chrono::steady_clock::now() + kTimeout,
       .cancellation = source.get_token()});
  stopper.join();
  CHECK(read.status == NetStatus::Cancelled);
  CHECK(read.data.empty());
}

TEST_CASE("a silent peer ends the read at the connection's timeout")
{
  Listener listener;
  REQUIRE(listener.port() > 0);
  const Connection connection = argus::net::connectLoopback({.host = "127.0.0.1",
                                                             .port = listener.port(),
                                                             .timeout = std::chrono::milliseconds(100),
                                                             .cancellation = {}});
  REQUIRE(connection.connected());
  const Socket accepted = listener.accept();

  std::string into;
  CHECK(argus::net::receiveSome({.fd = connection.socket.get(), .into = into, .cancellation = {}}) ==
        NetStatus::Failed);
  CHECK(into.empty());
}

TEST_CASE("a socket closes its descriptor once and moves ownership")
{
  int raw = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  REQUIRE(raw >= 0);
  {
    Socket first(raw);
    Socket second(std::move(first));
    CHECK(second.get() == raw);
    CHECK(::fcntl(raw, F_GETFD) >= 0);
  }
  CHECK(::fcntl(raw, F_GETFD) == -1);
  CHECK(errno == EBADF);
}
