#include "mdns-service.hxx"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <drogon/drogon.h>
#include <ifaddrs.h>
#include <mdns.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <shared/services/config-service/config-service.hxx>
#include <string>
#include <string_view>
#include <strings.h>
#include <thread>
#include <utility>
#include <vector>

namespace
{

struct MdnsConfig
{
  bool enabled = false;
  std::string name = "Argus";
  std::string serviceType = "_argus._tcp";
  uint16_t port = 7024;
  std::vector<std::pair<std::string, std::string>> txt;
};

struct MdnsSocketDeleter
{
  void operator()(int* fd) const
  {
    if (fd && *fd >= 0)
      mdns_socket_close(*fd);
    delete fd;
  }
};

using SocketPtr = std::unique_ptr<int, MdnsSocketDeleter>;

struct AlignedBufferDeleter
{
  void operator()(std::byte* ptr) const { std::free(ptr); }
};

using BufferPtr = std::unique_ptr<std::byte, AlignedBufferDeleter>;

constexpr size_t kPacketCapacity = 2048;

std::string normalizeServiceType(const std::string& raw)
{
  std::string type = raw.empty() ? "_argus._tcp" : raw;
  if (type.front() != '_')
    type.insert(type.begin(), '_');
  static constexpr std::string_view kLocalSuffix = ".local";
  if (type.size() >= kLocalSuffix.size() &&
      type.compare(type.size() - kLocalSuffix.size(), kLocalSuffix.size(),
                   kLocalSuffix) == 0)
    type.resize(type.size() - kLocalSuffix.size());
  if (type.back() != '.')
    type.push_back('.');
  type += "local.";
  return type;
}

std::string sanitizeInstanceName(const std::string& raw)
{
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw)
    out.push_back(c == '.' ? '-' : c);
  return out.empty() ? "Argus" : out;
}

MdnsConfig loadMdnsConfig()
{
  MdnsConfig config;
  config.enabled = ConfigService::getBool("mdns.enabled");
  const std::string name = ConfigService::getString("mdns.name");
  if (!name.empty())
    config.name = name;
  const std::string type = ConfigService::getString("mdns.service_type");
  if (!type.empty())
    config.serviceType = type;
  const int port = ConfigService::getInt("mdns.port");
  if (port > 0 && port <= 65535)
    config.port = static_cast<uint16_t>(port);
  config.txt = ConfigService::getStringPairs("mdns.txt");
  return config;
}

bool isUsableInterface(const struct ifaddrs* ifa)
{
  if (!ifa || !ifa->ifa_addr)
    return false;
  if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_MULTICAST) == 0)
    return false;
  if ((ifa->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT)) != 0)
    return false;
  return true;
}

bool nameEquals(const mdns_string_t& name, const char* expected,
                size_t expectedLength)
{
  return name.length == expectedLength &&
         strncasecmp(name.str, expected, name.length) == 0;
}

} // namespace

struct MdnsService::Impl
{
  MdnsConfig config;
  std::atomic<bool> running{false};
  std::thread thread;
  std::vector<SocketPtr> sockets;
  BufferPtr buffer;
  size_t bufferCapacity = 0;

  std::string service;
  std::string serviceInstance;
  std::string hostname;
  std::string hostnameQualified;

  struct sockaddr_in addressIpv4{};
  struct sockaddr_in6 addressIpv6{};
  bool hasIpv4 = false;
  bool hasIpv6 = false;

  mdns_record_t recordPtr{};
  mdns_record_t recordSrv{};
  mdns_record_t recordA{};
  mdns_record_t recordAaaa{};
  std::vector<mdns_record_t> txtRecords;

  bool resolveAddresses();
  void buildRecords();
  std::vector<mdns_record_t> buildAdditionalRecords() const;

  void runLoop();

  void sendAnswer(int sock, const struct sockaddr* from, size_t addrlen,
                  uint16_t query_id, uint16_t rtype, uint16_t rclass,
                  const mdns_string_t& queryName, const mdns_record_t& answer,
                  const std::vector<mdns_record_t>& additional) const;

  int handleQuestion(int sock, const struct sockaddr* from, size_t addrlen,
                     uint16_t query_id, uint16_t rtype, uint16_t rclass,
                     const void* data, size_t size, size_t name_offset,
                     size_t name_length) const;

  void announce();
  void goodbye();

  static int callbackBridge(int sock, const struct sockaddr* from,
                            size_t addrlen, mdns_entry_type_t entry,
                            uint16_t query_id, uint16_t rtype, uint16_t rclass,
                            uint32_t ttl, const void* data, size_t size,
                            size_t name_offset, size_t name_length,
                            size_t record_offset, size_t record_length,
                            void* user_data);
};

bool MdnsService::Impl::resolveAddresses()
{
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) < 0)
    return false;

  for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!isUsableInterface(ifa))
      continue;
    if (ifa->ifa_addr->sa_family == AF_INET && !hasIpv4) {
      auto* addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
      if (addr->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        addressIpv4 = *addr;
        hasIpv4 = true;
      }
    }
    else if (ifa->ifa_addr->sa_family == AF_INET6 && !hasIpv6) {
      auto* addr = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
      if (addr->sin6_scope_id == 0 && !IN6_IS_ADDR_LOOPBACK(&addr->sin6_addr) &&
          !IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) {
        addressIpv6 = *addr;
        hasIpv6 = true;
      }
    }
  }

  freeifaddrs(ifaddr);
  return hasIpv4 || hasIpv6;
}

void MdnsService::Impl::buildRecords()
{
  hostname = sanitizeInstanceName(config.name);
  service = normalizeServiceType(config.serviceType);
  serviceInstance = hostname + "." + service;
  hostnameQualified = hostname + ".local.";

  const mdns_string_t serviceStr{service.data(), service.size()};
  const mdns_string_t serviceInstanceStr{serviceInstance.data(),
                                         serviceInstance.size()};
  const mdns_string_t hostnameQualifiedStr{hostnameQualified.data(),
                                           hostnameQualified.size()};

  recordPtr = {};
  recordPtr.name = serviceStr;
  recordPtr.type = MDNS_RECORDTYPE_PTR;
  recordPtr.data.ptr.name = serviceInstanceStr;

  recordSrv = {};
  recordSrv.name = serviceInstanceStr;
  recordSrv.type = MDNS_RECORDTYPE_SRV;
  recordSrv.data.srv.name = hostnameQualifiedStr;
  recordSrv.data.srv.port = config.port;
  recordSrv.data.srv.priority = 0;
  recordSrv.data.srv.weight = 0;

  if (hasIpv4) {
    recordA = {};
    recordA.name = hostnameQualifiedStr;
    recordA.type = MDNS_RECORDTYPE_A;
    recordA.data.a.addr = addressIpv4;
    recordA.data.a.addr.sin_port = 0;
  }

  if (hasIpv6) {
    recordAaaa = {};
    recordAaaa.name = hostnameQualifiedStr;
    recordAaaa.type = MDNS_RECORDTYPE_AAAA;
    recordAaaa.data.aaaa.addr = addressIpv6;
    recordAaaa.data.aaaa.addr.sin6_port = 0;
  }

  txtRecords.clear();
  txtRecords.reserve(config.txt.size());
  for (const auto& [key, value] : config.txt) {
    mdns_record_t txt{};
    txt.name = serviceInstanceStr;
    txt.type = MDNS_RECORDTYPE_TXT;
    txt.data.txt.key = mdns_string_t{key.data(), key.size()};
    txt.data.txt.value = mdns_string_t{value.data(), value.size()};
    txtRecords.push_back(txt);
  }
}

std::vector<mdns_record_t> MdnsService::Impl::buildAdditionalRecords() const
{
  std::vector<mdns_record_t> additional;
  additional.reserve(txtRecords.size() + 3);
  additional.push_back(recordSrv);
  if (hasIpv4)
    additional.push_back(recordA);
  if (hasIpv6)
    additional.push_back(recordAaaa);
  for (const auto& txt : txtRecords)
    additional.push_back(txt);
  return additional;
}

void MdnsService::Impl::sendAnswer(
    int sock, const struct sockaddr* from, size_t addrlen, uint16_t query_id,
    uint16_t rtype, uint16_t rclass, const mdns_string_t& queryName,
    const mdns_record_t& answer,
    const std::vector<mdns_record_t>& additional) const
{
  if ((rclass & MDNS_UNICAST_RESPONSE) != 0) {
    mdns_query_answer_unicast(sock, from, addrlen, buffer.get(), bufferCapacity,
                              query_id, static_cast<mdns_record_type_t>(rtype),
                              queryName.str, queryName.length, answer, 0, 0,
                              additional.data(), additional.size());
  }
  else {
    mdns_query_answer_multicast(sock, buffer.get(), bufferCapacity, answer, 0,
                                0, additional.data(), additional.size());
  }
}

int MdnsService::Impl::handleQuestion(int sock, const struct sockaddr* from,
                                      size_t addrlen, uint16_t query_id,
                                      uint16_t rtype, uint16_t rclass,
                                      const void* data, size_t size,
                                      size_t name_offset,
                                      size_t name_length) const
{
  static constexpr char kDnsSd[] = "_services._dns-sd._udp.local.";

  (void)name_length;

  char nameBuffer[256];
  size_t offset = name_offset;
  const mdns_string_t name =
      mdns_string_extract(data, size, &offset, nameBuffer, sizeof(nameBuffer));
  if (name.length == 0)
    return 0;

  if (nameEquals(name, kDnsSd, sizeof(kDnsSd) - 1)) {
    if (rtype != MDNS_RECORDTYPE_PTR && rtype != MDNS_RECORDTYPE_ANY)
      return 0;
    mdns_record_t answer = recordPtr;
    answer.name = name;
    sendAnswer(sock, from, addrlen, query_id, rtype, rclass, name, answer, {});
    return 0;
  }

  if (nameEquals(name, service.data(), service.size())) {
    if (rtype != MDNS_RECORDTYPE_PTR && rtype != MDNS_RECORDTYPE_ANY)
      return 0;
    const std::vector<mdns_record_t> additional = buildAdditionalRecords();
    sendAnswer(sock, from, addrlen, query_id, rtype, rclass, name, recordPtr,
               additional);
    return 0;
  }

  if (nameEquals(name, serviceInstance.data(), serviceInstance.size())) {
    if (rtype != MDNS_RECORDTYPE_SRV && rtype != MDNS_RECORDTYPE_ANY)
      return 0;
    std::vector<mdns_record_t> additional;
    additional.reserve(txtRecords.size() + 2);
    if (hasIpv4)
      additional.push_back(recordA);
    if (hasIpv6)
      additional.push_back(recordAaaa);
    for (const auto& txt : txtRecords)
      additional.push_back(txt);
    sendAnswer(sock, from, addrlen, query_id, rtype, rclass, name, recordSrv,
               additional);
    return 0;
  }

  if (nameEquals(name, hostnameQualified.data(), hostnameQualified.size())) {
    const bool answerA =
        (rtype == MDNS_RECORDTYPE_A || rtype == MDNS_RECORDTYPE_ANY) && hasIpv4;
    const bool answerAaaa =
        (rtype == MDNS_RECORDTYPE_AAAA || rtype == MDNS_RECORDTYPE_ANY) &&
        hasIpv6;
    if (!answerA && !answerAaaa)
      return 0;

    if (answerA) {
      std::vector<mdns_record_t> additional;
      additional.reserve(txtRecords.size() + 1);
      if (answerAaaa)
        additional.push_back(recordAaaa);
      for (const auto& txt : txtRecords)
        additional.push_back(txt);
      sendAnswer(sock, from, addrlen, query_id, rtype, rclass, name, recordA,
                 additional);
    }

    if (answerAaaa) {
      std::vector<mdns_record_t> additional;
      additional.reserve(txtRecords.size() + 1);
      if (answerA)
        additional.push_back(recordA);
      for (const auto& txt : txtRecords)
        additional.push_back(txt);
      sendAnswer(sock, from, addrlen, query_id, rtype, rclass, name, recordAaaa,
                 additional);
    }
    return 0;
  }

  return 0;
}

void MdnsService::Impl::runLoop()
{
  while (running.load(std::memory_order_relaxed)) {
    std::vector<struct pollfd> fds;
    fds.reserve(sockets.size());
    for (const auto& sock : sockets)
      fds.push_back({*sock, POLLIN, 0});

    const int ready = ::poll(fds.data(), fds.size(), 500);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ready == 0)
      continue;

    for (size_t i = 0; i < sockets.size(); ++i) {
      if ((fds[i].revents & (POLLIN | POLLERR)) != 0)
        mdns_socket_listen(*sockets[i], buffer.get(), bufferCapacity,
                           callbackBridge, this);
    }
  }
}

int MdnsService::Impl::callbackBridge(int sock, const struct sockaddr* from,
                                      size_t addrlen, mdns_entry_type_t entry,
                                      uint16_t query_id, uint16_t rtype,
                                      uint16_t rclass, uint32_t ttl,
                                      const void* data, size_t size,
                                      size_t name_offset, size_t name_length,
                                      size_t record_offset,
                                      size_t record_length, void* user_data)
{
  (void)ttl;
  (void)record_offset;
  (void)record_length;
  if (entry != MDNS_ENTRYTYPE_QUESTION)
    return 0;

  auto* impl = static_cast<MdnsService::Impl*>(user_data);
  return impl->handleQuestion(sock, from, addrlen, query_id, rtype, rclass,
                              data, size, name_offset, name_length);
}

void MdnsService::Impl::announce()
{
  const std::vector<mdns_record_t> additional = buildAdditionalRecords();
  for (const auto& sock : sockets)
    mdns_announce_multicast(*sock, buffer.get(), bufferCapacity, recordPtr, 0,
                            0, additional.data(), additional.size());
}

void MdnsService::Impl::goodbye()
{
  const std::vector<mdns_record_t> additional = buildAdditionalRecords();
  for (const auto& sock : sockets)
    mdns_goodbye_multicast(*sock, buffer.get(), bufferCapacity, recordPtr, 0, 0,
                           additional.data(), additional.size());
}

MdnsService::MdnsService() : impl_(std::make_unique<Impl>())
{
  impl_->config = loadMdnsConfig();
}

MdnsService::~MdnsService()
{
  shutdown();
}

bool MdnsService::initialize()
{
  if (!impl_->config.enabled) {
    LOG_INFO << "mDNS advertising disabled (mdns.enabled=false)";
    return true;
  }

  if (!impl_->resolveAddresses())
    LOG_WARN << "mDNS: no usable network interface found";

  impl_->buildRecords();

  struct sockaddr_in v4{};
  v4.sin_family = AF_INET;
  v4.sin_addr.s_addr = INADDR_ANY;
  v4.sin_port = htons(MDNS_PORT);
  const int fd4 = mdns_socket_open_ipv4(&v4);
  if (fd4 >= 0) {
    impl_->sockets.push_back(SocketPtr(new int(fd4)));
  }
  else {
    LOG_WARN << "mDNS: failed to open IPv4 socket (errno=" << errno << ")";
  }

  struct sockaddr_in6 v6{};
  v6.sin6_family = AF_INET6;
  v6.sin6_addr = in6addr_any;
  v6.sin6_port = htons(MDNS_PORT);
  const int fd6 = mdns_socket_open_ipv6(&v6);
  if (fd6 >= 0) {
    impl_->sockets.push_back(SocketPtr(new int(fd6)));
  }
  else {
    LOG_WARN << "mDNS: failed to open IPv6 socket (errno=" << errno << ")";
  }

  if (impl_->sockets.empty()) {
    LOG_WARN << "mDNS: no socket available, advertising disabled";
    return true;
  }

  impl_->buffer = BufferPtr(
      static_cast<std::byte*>(std::aligned_alloc(64, kPacketCapacity)));
  if (!impl_->buffer) {
    LOG_ERROR << "mDNS: failed to allocate packet buffer";
    impl_->sockets.clear();
    return true;
  }
  impl_->bufferCapacity = kPacketCapacity;

  impl_->announce();
  impl_->running.store(true, std::memory_order_relaxed);
  impl_->thread = std::thread(&Impl::runLoop, impl_.get());

  LOG_INFO << "mDNS advertising \"" << impl_->hostname << "\" as "
           << impl_->serviceInstance << " port " << impl_->config.port;
  return true;
}

bool MdnsService::isAdvertising() const
{
  return impl_ && impl_->running.load(std::memory_order_relaxed) &&
         !impl_->sockets.empty();
}

void MdnsService::shutdown()
{
  if (!impl_)
    return;

  impl_->running.store(false, std::memory_order_relaxed);
  if (impl_->thread.joinable())
    impl_->thread.join();

  if (!impl_->sockets.empty()) {
    impl_->goodbye();
    impl_->sockets.clear();
  }
  impl_->buffer.reset();
  impl_->bufferCapacity = 0;
}

Json::Value MdnsService::health() const
{
  Json::Value value(Json::objectValue);
  value["advertising"] = isAdvertising();
  value["name"] = impl_->config.name;
  value["serviceType"] = impl_->config.serviceType;
  value["port"] = impl_->config.port;
  return value;
}
