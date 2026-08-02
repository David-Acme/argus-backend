#include "json-diff.hxx"

#include <shared/utils/json-util/json-util.hxx>
#include <string_view>

bool JsonDiff::isIndex(const std::string& s)
{
  if (s.empty())
    return false;
  for (char c : s)
    if (c < '0' || c > '9')
      return false;
  return true;
}

std::vector<std::string> JsonDiff::splitPath(const std::string& path)
{
  std::vector<std::string> out;
  if (path.empty())
    return out;
  size_t start = 0;
  while (true) {
    const size_t pos = path.find('.', start);
    if (pos == std::string::npos) {
      out.push_back(path.substr(start));
      break;
    }
    out.push_back(path.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

bool JsonDiff::valuesEqual(const Json::Value& a, const Json::Value& b)
{
  if (a.type() != b.type()) {
    if ((a.isNull() || a.isObject()) && (b.isNull() || b.isObject())) {
      const bool aEmpty = a.isNull() || (a.isObject() && a.empty());
      const bool bEmpty = b.isNull() || (b.isObject() && b.empty());
      if (aEmpty && bEmpty)
        return true;
    }
    return false;
  }
  return a == b;
}

Json::Value JsonDiff::getByPath(const Json::Value& root,
                                const std::vector<std::string>& segments)
{
  const Json::Value* cur = &root;
  for (const auto& seg : segments) {
    if (!cur || cur->isNull())
      return Json::Value();
    if (isIndex(seg)) {
      if (!cur->isArray())
        return Json::Value();
      const unsigned idx = static_cast<unsigned>(std::stoul(seg));
      if (idx >= cur->size())
        return Json::Value();
      cur = &(*cur)[idx];
    }
    else {
      if (!cur->isObject())
        return Json::Value();
      if (!cur->isMember(seg))
        return Json::Value();
      cur = &(*cur)[seg];
    }
  }
  return *cur;
}

void JsonDiff::setByPath(Json::Value& root,
                         const std::vector<std::string>& segments,
                         const Json::Value& value)
{
  if (segments.empty())
    return;
  Json::Value* cur = &root;
  for (size_t i = 0; i < segments.size(); ++i) {
    const std::string& seg = segments[i];
    const bool last = (i + 1 == segments.size());
    const bool segIsIdx = isIndex(seg);
    if (last) {
      if (segIsIdx) {
        if (!cur->isArray())
          *cur = Json::Value(Json::arrayValue);
        (*cur)[static_cast<unsigned>(std::stoul(seg))] = value;
      }
      else {
        if (!cur->isObject())
          *cur = Json::Value(Json::objectValue);
        (*cur)[seg] = value;
      }
      return;
    }
    const std::string& next = segments[i + 1];
    const bool nextIsIdx = isIndex(next);
    if (segIsIdx) {
      const unsigned idx = static_cast<unsigned>(std::stoul(seg));
      if (!cur->isArray())
        *cur = Json::Value(Json::arrayValue);
      if ((*cur)[idx].isNull())
        (*cur)[idx] = nextIsIdx ? Json::Value(Json::arrayValue)
                                : Json::Value(Json::objectValue);
      cur = &(*cur)[idx];
    }
    else {
      if (!cur->isObject())
        *cur = Json::Value(Json::objectValue);
      if (!cur->isMember(seg) || (*cur)[seg].isNull())
        (*cur)[seg] = nextIsIdx ? Json::Value(Json::arrayValue)
                                : Json::Value(Json::objectValue);
      cur = &(*cur)[seg];
    }
  }
}

void JsonDiff::deepMerge(Json::Value& target, const Json::Value& source)
{
  if (!source.isObject()) {
    target = source;
    return;
  }
  if (!target.isObject())
    target = Json::Value(Json::objectValue);
  for (const auto& k : source.getMemberNames()) {
    if (source[k].isObject()) {
      if (!target.isMember(k) || !target[k].isObject())
        target[k] = Json::Value(Json::objectValue);
      deepMerge(target[k], source[k]);
    }
    else {
      target[k] = source[k];
    }
  }
}

void JsonDiff::diffRecursive(const Json::Value& orig, const Json::Value& upd,
                             std::string& path, ChangesDiff& out)
{
  if (orig.isArray() || upd.isArray()) {
    if (!valuesEqual(orig, upd))
      out[path] = Change{orig, upd};
    return;
  }

  if (orig.isObject() && upd.isObject()) {
    const auto origKeys = orig.getMemberNames();
    const auto updKeys = upd.getMemberNames();

    std::unordered_map<std::string, int> keySet;
    keySet.reserve(origKeys.size() + updKeys.size());
    for (const auto& k : origKeys)
      keySet[k] |= 1;
    for (const auto& k : updKeys)
      keySet[k] |= 2;

    const size_t baseLen = path.size();

    for (const auto& [k, flags] : keySet) {
      if (baseLen > 0)
        path += '.';
      path += k;

      if (flags == 1) {
        out[path] = Change{orig[k], Json::Value()};
      }
      else if (flags == 2) {
        out[path] = Change{Json::Value(), upd[k]};
      }
      else {
        diffRecursive(orig[k], upd[k], path, out);
      }

      path.resize(baseLen);
    }
    return;
  }

  if (!valuesEqual(orig, upd)) {
    const Json::Value prev = orig.isNull() ? Json::Value() : orig;
    const Json::Value cur = upd.isNull() ? Json::Value() : upd;
    out[path] = Change{prev, cur};
  }
}

ChangesDiff JsonDiff::createFlatDiff(const Json::Value& original,
                                     const Json::Value& updated)
{
  ChangesDiff out;
  std::string path;
  path.reserve(128);
  diffRecursive(original, updated, path, out);
  return out;
}

ChangesComparisonResult
JsonDiff::compareChanges(const ChangesDiff& prevChanges,
                         const ChangesDiff& startChanges)
{
  ChangesDiff combined;
  combined.reserve(prevChanges.size() + startChanges.size());

  for (const auto& [key, prev] : prevChanges) {
    const auto it = startChanges.find(key);
    if (it != startChanges.end()) {
      const auto& next = it->second;
      if (valuesEqual(next.current, prev.previous))
        combined[key] = Change{prev.current, prev.previous};
      else
        combined[key] = Change{prev.previous, next.current};
    }
    else {
      combined[key] = prev;
    }
  }

  for (const auto& [key, next] : startChanges) {
    if (prevChanges.find(key) == prevChanges.end())
      combined[key] = next;
  }

  if (combined.empty())
    return ChangesComparisonResult{"DELETE", {}};

  return ChangesComparisonResult{"SNAPSHOT", std::move(combined)};
}

Json::Value JsonDiff::compareObjects(const Json::Value& left,
                                     const Json::Value& right)
{
  const ChangesDiff flat = createFlatDiff(left, right);
  if (flat.empty())
    return Json::Value();

  Json::Value changes(Json::objectValue);
  for (const auto& [key, change] : flat) {
    const auto segments = splitPath(key);
    Json::Value acc = change.current;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
      if (isIndex(*it)) {
        Json::Value arr(Json::arrayValue);
        arr[static_cast<unsigned>(std::stoul(*it))] = acc;
        acc = std::move(arr);
      }
      else {
        Json::Value obj(Json::objectValue);
        obj[*it] = std::move(acc);
        acc = std::move(obj);
      }
    }
    deepMerge(changes, acc);
  }
  return changes;
}

Json::Value JsonDiff::applyChanges(const ChangesDiff& changes,
                                   Json::Value target)
{
  for (const auto& [key, change] : changes) {
    const auto segments = splitPath(key);
    setByPath(target, segments, change.current);
  }
  return target;
}

ChangesDiff JsonDiff::fromJsonString(const std::string& jsonStr)
{
  ChangesDiff out;
  if (jsonStr.empty())
    return out;

  const Json::Value root = json_util::fromString(jsonStr);
  if (!root.isObject())
    return out;

  out.reserve(root.size());
  for (const auto& key : root.getMemberNames()) {
    const Json::Value& node = root[key];
    Change c;
    if (node.isObject()) {
      if (node.isMember("previous"))
        c.previous = node["previous"];
      if (node.isMember("current"))
        c.current = node["current"];
    }
    else if (node.isArray()) {
      if (node.size() > 0)
        c.previous = node[0u];
      if (node.size() > 1)
        c.current = node[1u];
    }
    else {
      c.current = node;
    }
    out.emplace(key, std::move(c));
  }
  return out;
}

Json::Value JsonDiff::toJson(const ChangesDiff& changes)
{
  Json::Value out(Json::objectValue);
  for (const auto& [key, change] : changes) {
    Json::Value node(Json::objectValue);
    if (!change.previous.isNull())
      node["previous"] = change.previous;
    if (!change.current.isNull())
      node["current"] = change.current;
    out[key] = std::move(node);
  }
  return out;
}
