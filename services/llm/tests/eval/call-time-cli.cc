#include <feature/memory/services/extract/call-time.hxx>
#include <text/iso-time.hxx>

#include <json/json.h>

#include <iostream>
#include <memory>
#include <string>

namespace
{

Json::Value answerFor(const Json::Value& request)
{
  Json::Value answer(Json::objectValue);
  answer["seq"] = request["seq"];
  answer["at"] = Json::nullValue;
  const auto now = iso_time::parse(request["now"].asString());
  if (!now)
    return answer;
  const auto found = call_time::resolve(
      {.text = request["text"].asString(), .lang = request["lang"].asString(), .now = *now});
  if (!found)
    return answer;
  answer["at"] = iso_time::format(found->fireAt);
  answer["begin"] = static_cast<Json::UInt64>(found->phraseBegin);
  answer["end"] = static_cast<Json::UInt64>(found->phraseEnd);
  return answer;
}

}

int main()
{
  std::ios::sync_with_stdio(false);
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  writer["emitUTF8"] = true;
  const std::unique_ptr<Json::CharReader> reader(Json::CharReaderBuilder().newCharReader());
  std::string line;
  while (std::getline(std::cin, line)) {
    Json::Value request;
    std::string errors;
    if (!reader->parse(line.data(), line.data() + line.size(), &request, &errors)) {
      std::cerr << "call-time-cli: " << errors << '\n';
      return 1;
    }
    std::cout << Json::writeString(writer, answerFor(request)) << std::endl;
  }
  return 0;
}
