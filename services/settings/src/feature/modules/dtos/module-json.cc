#include "module-json.hxx"

#include <utility>

namespace
{
Json::Value strings(const std::vector<std::string>& values)
{
  Json::Value list(Json::arrayValue);
  for (const auto& value : values)
    list.append(value);
  return list;
}

Json::Value introLine(const ModuleIntroLine& line)
{
  Json::Value json(Json::objectValue);
  json["what"] = line.what;
  json["examples"] = strings(line.examples);
  return json;
}

Json::Value localized(const ModuleLocalized& text)
{
  Json::Value json(Json::objectValue);
  json["es"] = text.es;
  json["en"] = text.en;
  return json;
}

Json::Value hardware(const HardwareAssessment& assessment)
{
  Json::Value json(Json::objectValue);
  json["verdict"] = std::string(hardwareVerdictToString(assessment.verdict));
  json["reasons"] = strings(assessment.reasons);
  json["minRamMb"] = static_cast<Json::Int64>(assessment.minRamMb);
  json["recommendedRamMb"] = static_cast<Json::Int64>(assessment.recommendedRamMb);
  json["freeDiskMb"] = assessment.freeDiskMb ? Json::Value(static_cast<Json::Int64>(*assessment.freeDiskMb))
                                             : Json::Value(Json::nullValue);
  return json;
}

Json::Value component(const ComponentView& view)
{
  Json::Value json(Json::objectValue);
  json["id"] = view.component->spec.id;
  json["owner"] = view.component->owner;
  json["source"] = std::string(componentSourceToString(view.component->spec.source));
  json["reachable"] = view.reach != OwnerReach::Unreachable;
  json["reported"] = view.reach == OwnerReach::Answered;
  json["state"] = std::string(componentStateToString(view.status.state));
  json["bytesPresent"] = static_cast<Json::Int64>(view.status.bytesPresent);
  json["bytesTotal"] = static_cast<Json::Int64>(view.status.bytesTotal);
  json["ready"] = view.status.ready;
  json["hostCommand"] = view.component->spec.source == ComponentSource::Provisioned
                            ? Json::Value(view.component->spec.hostCommand)
                            : Json::Value(Json::nullValue);
  json["reason"] = view.status.reason.empty() ? Json::Value(Json::nullValue) : Json::Value(view.status.reason);
  return json;
}
}

namespace module_json
{
Json::Value job(const JobView& view)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(view.job.id);
  json["kind"] = std::string(jobKindToString(view.job.kind));
  json["owner"] = view.job.owner.empty() ? Json::Value(Json::nullValue) : Json::Value(view.job.owner);
  json["state"] = std::string(jobStateToString(view.job.state));
  json["progress"] = view.progress;
  json["bytesDone"] = static_cast<Json::Int64>(view.job.bytesDone);
  json["bytesTotal"] = static_cast<Json::Int64>(view.job.bytesTotal);
  json["bytesPerSecond"] = static_cast<Json::Int64>(view.bytesPerSecond);
  json["etaSeconds"] =
      view.etaSeconds ? Json::Value(static_cast<Json::Int64>(*view.etaSeconds)) : Json::Value(Json::nullValue);
  json["reason"] = view.job.reason.empty() ? Json::Value(Json::nullValue) : Json::Value(view.job.reason);
  return json;
}

Json::Value module(const ModuleView& view, std::string_view lang)
{
  const auto& entry = *view.module;
  Json::Value json(Json::objectValue);
  json["id"] = entry.id;
  json["name"] = entry.name.in(lang);
  json["summary"] = entry.summary.in(lang);
  json["kind"] = std::string(moduleKindToString(entry.kind));
  json["enabled"] = view.enabled;
  json["lifecycle"] = std::string(moduleLifecycleToString(view.lifecycle));
  json["hasData"] = view.hasData;
  json["dataPurgedAt"] =
      view.dataPurgedAt > 0 ? Json::Value(static_cast<Json::Int64>(view.dataPurgedAt)) : Json::Value(Json::nullValue);
  json["requires"] = strings(entry.required);
  json["roles"] = strings(entry.roles);
  json["intro"] = introLine(lang == "en" ? entry.intro.en : entry.intro.es);
  json["sizeBytes"] = static_cast<Json::Int64>(view.sizeBytes);
  json["installedBytes"] = static_cast<Json::Int64>(view.installedBytes);
  json["hardware"] = hardware(view.hardware);
  json["job"] = view.job ? job(*view.job) : Json::Value(Json::nullValue);
  Json::Value steps(Json::arrayValue);
  for (const auto& item : entry.gettingStarted) {
    Json::Value step(Json::objectValue);
    step["id"] = item.id;
    step["title"] = item.title.in(lang);
    step["route"] = item.route;
    steps.append(step);
  }
  json["gettingStarted"] = steps;
  Json::Value components(Json::arrayValue);
  for (const auto& part : view.components)
    components.append(component(part));
  json["components"] = std::move(components);
  return json;
}

Json::Value member(const ModuleView& view, std::string_view lang)
{
  Json::Value json(Json::objectValue);
  json["id"] = view.module->id;
  json["name"] = view.module->name.in(lang);
  json["enabled"] = view.enabled;
  json["lifecycle"] = std::string(moduleLifecycleToString(view.lifecycle));
  json["dataPurgedAt"] =
      view.dataPurgedAt > 0 ? Json::Value(static_cast<Json::Int64>(view.dataPurgedAt)) : Json::Value(Json::nullValue);
  return json;
}

Json::Value enabledModules(const ModuleStatesReply& set)
{
  Json::Value list(Json::arrayValue);
  for (const auto& entry : set.modules) {
    Json::Value item(Json::objectValue);
    item["id"] = entry.id;
    item["enabled"] = entry.enabled;
    item["lifecycle"] = entry.lifecycle;
    item["dataPurgedAt"] =
        entry.dataPurgedAt > 0 ? Json::Value(static_cast<Json::Int64>(entry.dataPurgedAt)) : Json::Value(Json::nullValue);
    item["roles"] = strings(entry.roles);
    item["kind"] = entry.kind;
    item["name"] = localized(entry.name);
    item["summary"] = localized(entry.summary);
    Json::Value intro(Json::objectValue);
    intro["es"] = introLine(entry.intro.es);
    intro["en"] = introLine(entry.intro.en);
    item["intro"] = std::move(intro);
    list.append(item);
  }
  return list;
}
}
