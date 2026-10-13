#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <nats/nats-permissions.hxx>
#include <nats/nats-subject.hxx>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using nats_permissions::ConsumerKind;
using nats_permissions::Grants;
using nats_permissions::ServicePermissions;
using nats_permissions::StreamUse;

constexpr std::array<std::string_view, 1> kRuledWildcards{
    "argus.recorder.v1.>"};

std::filesystem::path deployDir()
{
  return ARGUS_DEPLOY_DIR;
}

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::vector<std::string_view> tokensOf(std::string_view subject)
{
  std::vector<std::string_view> tokens;
  size_t start = 0;
  while (true) {
    const size_t dot = subject.find('.', start);
    tokens.push_back(subject.substr(start, dot == std::string_view::npos
                                               ? std::string_view::npos
                                               : dot - start));
    if (dot == std::string_view::npos)
      return tokens;
    start = dot + 1;
  }
}

bool printableToken(std::string_view token)
{
  return !token.empty() && std::ranges::all_of(token, [](char c) {
    return c > ' ' && c < 0x7f && c != '"';
  });
}

bool literalSubject(std::string_view subject)
{
  return std::ranges::all_of(tokensOf(subject), [](std::string_view token) {
    return printableToken(token) && token != "*" && token != ">";
  });
}

bool wildcardTree(std::string_view subject)
{
  const auto tokens = tokensOf(subject);
  return tokens.size() > 1 && tokens.back() == ">" &&
         std::all_of(tokens.begin(), tokens.end() - 1, [](std::string_view t) {
           return printableToken(t) && t != "*" && t != ">";
         });
}

bool validName(std::string_view name)
{
  return printableToken(name) &&
         name.find_first_of(".*>/\\") == std::string_view::npos;
}

bool covers(std::string_view grant, std::string_view subject)
{
  if (grant == subject)
    return true;
  if (!wildcardTree(grant))
    return false;
  const std::string_view stem = grant.substr(0, grant.size() - 1);
  return subject.size() > stem.size() && subject.starts_with(stem);
}

std::vector<std::string_view> namedSubjects(const ServicePermissions& row)
{
  std::vector<std::string_view> subjects(row.publishes.begin(),
                                         row.publishes.end());
  subjects.insert(subjects.end(), row.stores.begin(), row.stores.end());
  subjects.insert(subjects.end(), row.subscribes.begin(), row.subscribes.end());
  for (const auto& consumer : row.consumers)
    subjects.insert(subjects.end(), consumer.subjects.begin(),
                    consumer.subjects.end());
  return subjects;
}

const ServicePermissions& rowOf(std::string_view service)
{
  const auto& table = nats_permissions::table();
  const auto it =
      std::ranges::find(table, service, &ServicePermissions::service);
  REQUIRE_MESSAGE(it != table.end(), "no row for " << service);
  return *it;
}

struct ParsedDirection
{
  std::vector<std::string> allow;
  std::vector<std::string> deny;
};

struct ParsedBlock
{
  ParsedDirection publish;
  ParsedDirection subscribe;
};

std::string unquote(std::string_view line)
{
  const size_t open = line.find('"');
  const size_t close = line.rfind('"');
  if (open == std::string_view::npos || close <= open)
    return {};
  return std::string(line.substr(open + 1, close - open - 1));
}

std::map<std::string, ParsedBlock> parseConfig(const std::string& text)
{
  std::map<std::string, ParsedBlock> blocks;
  std::istringstream in(text);
  std::string line;
  ParsedBlock* block = nullptr;
  ParsedDirection* direction = nullptr;
  std::vector<std::string>* list = nullptr;
  while (std::getline(in, line)) {
    if (line.ends_with(": {") && !line.starts_with(' ')) {
      block = &blocks[line.substr(0, line.size() - 3)];
      direction = nullptr;
      list = nullptr;
    }
    else if (block != nullptr && line == "  publish: {")
      direction = &block->publish;
    else if (block != nullptr && line == "  subscribe: {")
      direction = &block->subscribe;
    else if (direction != nullptr && line == "    allow: [")
      list = &direction->allow;
    else if (direction != nullptr && line == "    deny: [")
      list = &direction->deny;
    else if (line.find_first_not_of(' ') != std::string::npos &&
             line[line.find_first_not_of(' ')] == ']')
      list = nullptr;
    else if (list != nullptr && line.find('"') != std::string::npos)
      list->push_back(unquote(line));
  }
  return blocks;
}

std::vector<std::string> sorted(std::vector<std::string> values)
{
  std::ranges::sort(values);
  return values;
}

struct DirectionCheck
{
  const ParsedDirection& file;
  const std::vector<std::string>& table;
  std::string where;
};

void checkDirection(const DirectionCheck& check)
{
  const auto& [file, table, where] = check;
  if (table.empty()) {
    CHECK_MESSAGE(file.allow.empty(),
                  where << " allows what the table does not grant");
    CHECK_MESSAGE(file.deny == std::vector<std::string>{">"},
                  where
                      << " must deny everything when the table grants nothing");
    return;
  }
  CHECK_MESSAGE(file.deny.empty(), where << " carries a deny list");
  for (const auto& subject : file.allow)
    CHECK_MESSAGE(std::ranges::binary_search(table, subject),
                  where << " grants " << subject
                        << ", which the table does not name");
  for (const auto& subject : table)
    CHECK_MESSAGE(std::ranges::find(file.allow, subject) != file.allow.end(),
                  where << " misses " << subject << ", which the table grants");
}
}

TEST_CASE("the checked-in permission config is the table's rendering")
{
  const std::string expected = nats_permissions::render();
  const std::string actual = readFile(deployDir() / "nats-permissions.conf");
  if (actual != expected) {
    std::ofstream out(ARGUS_NATS_RENDER_OUT,
                      std::ios::binary | std::ios::trunc);
    out << expected;
  }
  CHECK_MESSAGE(
      actual == expected,
      "argus-deploy/nats-permissions.conf has drifted from the table; "
      "the table's rendering is in " ARGUS_NATS_RENDER_OUT);
}

TEST_CASE("every grant in the checked-in config is one the table names")
{
  const auto blocks =
      parseConfig(readFile(deployDir() / "nats-permissions.conf"));
  const auto& table = nats_permissions::table();
  CHECK(blocks.size() == table.size());
  for (const auto& row : table) {
    const std::string variable = nats_permissions::variableName(row.service);
    const auto block = blocks.find(variable);
    REQUIRE_MESSAGE(block != blocks.end(), variable << " is missing");
    const Grants grants = nats_permissions::grantsFor(row);
    checkDirection({.file = block->second.publish,
                    .table = grants.publish,
                    .where = variable + " publish"});
    checkDirection({.file = block->second.subscribe,
                    .table = grants.subscribe,
                    .where = variable + " subscribe"});
  }
  for (const auto& [variable, block] : blocks)
    CHECK_MESSAGE(std::ranges::any_of(table,
                                      [&](const ServicePermissions& row) {
                                        return nats_permissions::variableName(
                                                   row.service) == variable;
                                      }),
                  variable << " has no row in the table");
}

TEST_CASE("every deployed NATS client has its own row")
{
  std::vector<std::string> deployed;
  for (const auto& entry : std::filesystem::directory_iterator(deployDir())) {
    const std::string file = entry.path().filename().string();
    if (!file.starts_with("config.") || !file.ends_with(".toml.example"))
      continue;
    std::istringstream in(readFile(entry.path()));
    std::string line;
    bool nats = false;
    while (std::getline(in, line))
      nats = nats || line == "[nats]";
    if (nats)
      deployed.push_back(file.substr(7, file.size() - 7 - 13));
  }
  CHECK(deployed.size() >= 10);
  for (const auto& service : deployed)
    CHECK_MESSAGE(std::ranges::any_of(nats_permissions::table(),
                                      [&](const ServicePermissions& row) {
                                        return row.service == service;
                                      }),
                  "config." << service << ".toml.example connects to NATS but "
                            << service << " has no row");
}

TEST_CASE("a service has one row, and its inbox is its own")
{
  std::set<std::string_view> services;
  for (const auto& row : nats_permissions::table()) {
    CHECK_MESSAGE(services.insert(row.service).second,
                  row.service << " has two rows");
    CHECK(validName(row.service));
    const Grants grants = nats_permissions::grantsFor(row);
    const std::string own = nats_permissions::inboxPrefix(row.service) + ".>";
    for (const auto& subject : grants.subscribe) {
      if (subject.starts_with(nats_permissions::kInboxRoot))
        CHECK_MESSAGE(subject == own,
                      row.service << " subscribes outside its own inbox: "
                                  << subject);
    }
    const bool inbox =
        std::ranges::find(grants.subscribe, own) != grants.subscribe.end();
    CHECK(inbox == nats_permissions::needsInbox(row));
  }
  CHECK(nats_permissions::inboxPrefix("sync") == "_INBOX.sync");
}

TEST_CASE(
    "a row names literal subjects, and only the ruled trees are wildcards")
{
  std::set<std::string, std::less<>> wildcards;
  for (const auto& row : nats_permissions::table()) {
    for (const auto subject : namedSubjects(row)) {
      if (wildcardTree(subject)) {
        wildcards.emplace(subject);
        continue;
      }
      CHECK_MESSAGE(literalSubject(subject),
                    row.service << " names a malformed subject: " << subject);
    }
    for (const auto& grant : row.streams)
      CHECK_MESSAGE(validName(grant.stream),
                    row.service << " names stream " << grant.stream);
    for (const auto& consumer : row.consumers) {
      CHECK_MESSAGE(validName(consumer.stream),
                    row.service << " names stream " << consumer.stream);
      CHECK_MESSAGE(validName(consumer.name),
                    row.service << " names consumer " << consumer.name);
      CHECK_MESSAGE(!consumer.subjects.empty(),
                    consumer.name << " names no subject");
    }
  }
  const std::set<std::string, std::less<>> ruled(kRuledWildcards.begin(),
                                                 kRuledWildcards.end());
  CHECK(wildcards == ruled);
  for (const auto& row : nats_permissions::table())
    for (const auto subject : row.publishes)
      if (wildcardTree(subject))
        CHECK_MESSAGE(subject.starts_with("argus." + std::string(row.service) +
                                          "."),
                      row.service
                          << " publishes a tree outside its own domain");
}

TEST_CASE("sync's row is an explicit list, one durable per feed it drains")
{
  const ServicePermissions& sync = rowOf("sync");
  for (const auto subject : namedSubjects(sync))
    CHECK_MESSAGE(literalSubject(subject),
                  "sync names a wildcard: " << subject);
  CHECK(sync.stores.empty());
  CHECK(sync.streams.empty());
  std::set<std::string_view> drained;
  for (const auto& consumer : sync.consumers) {
    CHECK(consumer.kind == ConsumerKind::Push);
    CHECK(consumer.subjects.size() == 1);
    drained.insert(consumer.subjects.front());
  }
  for (const auto& row : nats_permissions::table())
    for (const auto subject : row.stores)
      if (subject.ends_with(".change") || subject.ends_with(".user-action") ||
          subject == nats_subject::kAuthSession ||
          subject == nats_subject::kNotificationDelivery ||
          subject == nats_subject::kSettingsModule)
        CHECK_MESSAGE(drained.contains(subject),
                      "sync drains no durable for " << subject);
}

TEST_CASE("a consumer binds to a stream some row creates, under its own name")
{
  std::set<std::string_view> created;
  for (const auto& row : nats_permissions::table())
    for (const auto& grant : row.streams)
      if (grant.use != StreamUse::Inspect)
        created.insert(grant.stream);
  std::set<std::pair<std::string_view, std::string_view>> names;
  for (const auto& row : nats_permissions::table()) {
    for (const auto& consumer : row.consumers) {
      CHECK_MESSAGE(created.contains(consumer.stream),
                    consumer.name << " reads " << consumer.stream
                                  << ", which no row creates");
      CHECK_MESSAGE(names.insert({consumer.stream, consumer.name}).second,
                    consumer.name << " is claimed twice on "
                                  << consumer.stream);
    }
  }
}

TEST_CASE("a core subscription names a subject some row publishes")
{
  std::vector<std::string_view> published;
  for (const auto& row : nats_permissions::table()) {
    published.insert(published.end(), row.publishes.begin(),
                     row.publishes.end());
    published.insert(published.end(), row.stores.begin(), row.stores.end());
  }
  for (const auto& row : nats_permissions::table()) {
    for (const auto subject : row.subscribes) {
      if (subject.starts_with("$JS.EVENT."))
        continue;
      CHECK_MESSAGE(std::ranges::any_of(published,
                                        [subject](std::string_view grant) {
                                          return covers(grant, subject);
                                        }),
                    row.service << " subscribes to " << subject
                                << ", which no row publishes");
    }
  }
}

TEST_CASE("no direction renders an empty allow list, which the broker reads as "
          "allow-all")
{
  const std::string rendered = nats_permissions::render();
  CHECK(rendered.find("allow: [\n    ]") == std::string::npos);
  const Grants relay = nats_permissions::grantsFor(rowOf("relay"));
  CHECK(relay.publish.empty());
  CHECK(relay.subscribe ==
        std::vector<std::string>{nats_subject::kNotificationPushIntent});
}

TEST_CASE("the JetStream grants follow from the stream and consumer use")
{
  const ServicePermissions
      row{.service = "probe",
          .publishes = {},
          .stores = {"argus.probe.v1.change"},
          .subscribes = {},
          .streams = {{.stream = "S_INSPECT", .use = StreamUse::Inspect},
                      {.stream = "S_CREATE", .use = StreamUse::Create},
                      {.stream = "S_ENSURE", .use = StreamUse::Ensure}},
          .consumers = {{.stream = "S_ENSURE",
                         .name = "pushed",
                         .subjects = {"argus.probe.v1.change"},
                         .kind = ConsumerKind::Push,
                         .recreates = true},
                        {.stream = "S_CREATE",
                         .name = "pulled",
                         .subjects = {"argus.probe.v1.change"},
                         .kind = ConsumerKind::Pull,
                         .recreates = false}}};
  const Grants grants = nats_permissions::grantsFor(row);
  CHECK(
      grants.publish ==
      sorted({"$JS.ACK.S_CREATE.pulled.>", "$JS.ACK.S_ENSURE.pushed.>",
              "$JS.API.CONSUMER.CREATE.S_CREATE.pulled",
              "$JS.API.CONSUMER.CREATE.S_CREATE.pulled.>",
              "$JS.API.CONSUMER.DELETE.S_ENSURE.pushed",
              "$JS.API.CONSUMER.DURABLE.CREATE.S_ENSURE.pushed",
              "$JS.API.CONSUMER.INFO.S_CREATE.pulled",
              "$JS.API.CONSUMER.INFO.S_ENSURE.pushed",
              "$JS.API.CONSUMER.MSG.NEXT.S_CREATE.pulled",
              "$JS.API.STREAM.CREATE.S_CREATE",
              "$JS.API.STREAM.CREATE.S_ENSURE", "$JS.API.STREAM.INFO.S_CREATE",
              "$JS.API.STREAM.INFO.S_ENSURE", "$JS.API.STREAM.INFO.S_INSPECT",
              "$JS.API.STREAM.UPDATE.S_ENSURE", "argus.probe.v1.change"}));
  CHECK(grants.subscribe == std::vector<std::string>{"_INBOX.probe.>"});
  CHECK(nats_permissions::variableName("probe") == "ARGUS_NATS_PROBE");
}

TEST_CASE("the stream use and consumer kind spellings round-trip")
{
  for (const StreamUse use :
       {StreamUse::Inspect, StreamUse::Create, StreamUse::Ensure})
    CHECK(nats_permissions::streamUseFromString(
              nats_permissions::streamUseToString(use)) == use);
  for (const ConsumerKind kind : {ConsumerKind::Push, ConsumerKind::Pull})
    CHECK(nats_permissions::consumerKindFromString(
              nats_permissions::consumerKindToString(kind)) == kind);
  CHECK_FALSE(nats_permissions::streamUseFromString("update").has_value());
  CHECK_FALSE(nats_permissions::consumerKindFromString("ordered").has_value());
}
