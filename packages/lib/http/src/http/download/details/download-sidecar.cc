#include "download-sidecar.hxx"

#include <fstream>
#include <json/json.h>
#include <system_error>

namespace file_download::details
{

std::optional<Sidecar> readSidecar(const std::filesystem::path& path)
{
  std::ifstream input(path);
  if (!input)
    return std::nullopt;
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  if (!Json::parseFromStream(builder, input, &root, &errors) || !root.isObject())
    return std::nullopt;
  const auto text = [&root](const char* key)
  { return root[key].isString() ? root[key].asString() : std::string{}; };
  const auto number = [&root](const char* key) -> std::optional<std::uint64_t>
  {
    if (!root[key].isUInt64())
      return std::nullopt;
    return root[key].asUInt64();
  };
  const auto size = number("size");
  const auto bytes = number("bytes");
  if (!size || !bytes)
    return std::nullopt;
  return Sidecar{.url = text("url"),
                 .finalUrl = text("finalUrl"),
                 .size = *size,
                 .sha256 = text("sha256"),
                 .etag = text("etag"),
                 .bytes = *bytes};
}

bool writeSidecar(const std::filesystem::path& path, const Sidecar& sidecar)
{
  Json::Value root(Json::objectValue);
  root["url"] = sidecar.url;
  root["finalUrl"] = sidecar.finalUrl;
  root["size"] = Json::UInt64{sidecar.size};
  root["sha256"] = sidecar.sha256;
  root["etag"] = sidecar.etag;
  root["bytes"] = Json::UInt64{sidecar.bytes};
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    output << Json::writeString(builder, root);
    if (!output.flush())
      return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
}

}
