#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "eval-score.hxx"

#include <string>
#include <vector>

namespace
{

eval::ScoreConfig config()
{
  return {.writeTools = {"memory.remember", "memory.forget", "calendar.create_event", "modules.disable"},
          .ownerOfferMarkers = {"activ", "enable"},
          .memberOfferMarkers = {"dueño", "owner"},
          .inactiveMarkers = {"productividad", "productivity"},
          .questionMarkers = {"?", "confirm"}};
}

eval::EvalCase selection(const std::string& tool)
{
  eval::EvalCase item;
  item.id = "case";
  item.group = "memory";
  item.variant = "neutral";
  item.script = {"hola"};
  if (!tool.empty())
    item.calls.push_back({.tool = tool, .args = {}});
  return item;
}

Json::Value arguments(const std::string& key, const std::string& value)
{
  Json::Value out(Json::objectValue);
  out[key] = value;
  return out;
}

eval::CaseRun run(const eval::EvalCase& item, std::vector<eval::TurnResult> turns)
{
  return {.item = item, .turns = std::move(turns)};
}

}

TEST_CASE("folding removes accents and case")
{
  CHECK(eval::fold("¿Dónde está la CÁMARA del Garaje?") == "¿donde esta la camara del garaje?");
}

TEST_CASE("argument matchers read every string value")
{
  Json::Value args(Json::objectValue);
  args["title"] = "Reunión con Andrea";
  args["starts_at"] = "2026-10-08T15:00:00";
  args["module"] = "productivity";

  eval::ArgMatcher contains;
  contains.anyNeedles = {"andrea", "bruno"};
  CHECK(eval::matchesArguments({.matcher = contains, .arguments = args}));
  contains.anyNeedles = {"bruno"};
  CHECK_FALSE(eval::matchesArguments({.matcher = contains, .arguments = args}));

  eval::ArgMatcher hour;
  hour.pattern = "T(15|03):00";
  CHECK(eval::matchesArguments({.matcher = hour, .arguments = args}));
  hour.pattern = "T10:00";
  CHECK_FALSE(eval::matchesArguments({.matcher = hour, .arguments = args}));

  eval::ArgMatcher module;
  module.name = "module";
  module.equals = "Productivity";
  CHECK(eval::matchesArguments({.matcher = module, .arguments = args}));
  module.equals = "surveillance";
  CHECK_FALSE(eval::matchesArguments({.matcher = module, .arguments = args}));
}

TEST_CASE("a selection case scores precision, recall, false actions and arguments")
{
  eval::EvalCase save = selection("memory.remember");
  save.calls.front().args.push_back([] {
    eval::ArgMatcher matcher;
    matcher.anyNeedles = {"perro"};
    return matcher;
  }());
  eval::EvalCase none = selection("");
  none.id = "none";

  const eval::CaseRun good = run(save, {{.reply = "ok", .executed = {{.tool = "memory.remember", .arguments = arguments("text", "el perro come a las 7")}}}});
  const eval::CaseRun wrongArgument = run(save, {{.reply = "ok", .executed = {{.tool = "memory.remember", .arguments = arguments("text", "algo")}}}});
  const eval::CaseRun missed = run(save, {{.reply = "no"}});
  const eval::CaseRun falseWrite = run(none, {{.reply = "ok", .executed = {{.tool = "memory.remember", .arguments = arguments("text", "x")}}}});
  const eval::CaseRun quiet = run(none, {{.reply = "hola"}});

  const eval::Metrics metrics = eval::aggregate({good, wrongArgument, missed, falseWrite, quiet}, config());
  CHECK(metrics.at("tool.memory.remember.precision") == doctest::Approx(2.0 / 3.0));
  CHECK(metrics.at("tool.memory.remember.recall") == doctest::Approx(2.0 / 3.0));
  CHECK(metrics.at("argument.accuracy") == doctest::Approx(0.5));
  CHECK(metrics.at("falseActionRate") == doctest::Approx(0.2));
  CHECK(metrics.at("falseWriteRate") == doctest::Approx(0.2));
  CHECK(metrics.at("selection.accuracy") == doctest::Approx(0.6));
}

TEST_CASE("an allowed tool is neither a hit nor a false action")
{
  eval::EvalCase camera = selection("");
  camera.allowed = {"app.show_camera"};
  const eval::CaseRun shown = run(camera, {{.reply = "ok", .executed = {{.tool = "app.show_camera", .arguments = Json::Value()}}}});
  const eval::CaseRun wrote = run(camera, {{.reply = "ok", .executed = {{.tool = "memory.remember", .arguments = Json::Value()}}}});
  const eval::Metrics metrics = eval::aggregate({shown, wrote}, config());
  CHECK(metrics.at("falseActionRate") == doctest::Approx(0.5));
  CHECK(metrics.at("falseWriteRate") == doctest::Approx(0.5));
}

TEST_CASE("a module that is off must be attempted, answered in prose and offered")
{
  eval::EvalCase item = selection("");
  item.group = "inactive";
  item.inactive = eval::InactiveExpectation{.module = "productivity", .attempted = "calendar.create_event", .audience = "owner"};

  const eval::TurnResult good{.reply = "La Productividad está desactivada, ¿la activo?",
                              .offered = {{.tool = "calendar.create_event", .arguments = Json::Value()}}};
  const eval::TurnResult recalled{.reply = "La Productividad está desactivada, ¿la activo?",
                                  .executed = {{.tool = "memory.recall", .arguments = Json::Value()}},
                                  .offered = {{.tool = "calendar.create_event", .arguments = Json::Value()}}};
  const eval::TurnResult silent{.reply = "Listo, agendado."};
  const eval::Metrics metrics = eval::aggregate({run(item, {good}), run(item, {recalled}), run(item, {silent})}, config());
  CHECK(metrics.at("inactive.pass") == doctest::Approx(1.0 / 3.0));
  CHECK(metrics.at("inactive.attempt") == doctest::Approx(2.0 / 3.0));
  CHECK(metrics.at("inactive.neverMemoryRecall") == doctest::Approx(2.0 / 3.0));
  CHECK(metrics.at("inactive.offer") == doctest::Approx(2.0 / 3.0));

  item.inactive.value().audience = "member";
  const eval::TurnResult member{.reply = "La Productividad está apagada, pídele al dueño que la active.",
                                .offered = {{.tool = "calendar.create_event", .arguments = Json::Value()}}};
  CHECK(eval::aggregate({run(item, {member})}, config()).at("inactive.member.pass") == doctest::Approx(1.0));
  CHECK(eval::aggregate({run(item, {good})}, config()).at("inactive.member.pass") == doctest::Approx(0.0));
}

TEST_CASE("a destructive tool needs the ask first and the spoken yes second")
{
  eval::EvalCase ask = selection("");
  ask.confirm = eval::ConfirmExpectation{.tool = "modules.disable", .phase = "ask", .args = {}};
  const eval::TurnResult previewed{.reply = "Esto detiene la vigilancia. ¿Confirmas?", .previews = {"modules.disable"}};
  const eval::TurnResult executed{.reply = "Hecho.", .confirmed = {"modules.disable"}};
  const eval::TurnResult chatter{.reply = "Entendido."};
  const eval::Metrics asked = eval::aggregate({run(ask, {previewed}), run(ask, {executed}), run(ask, {chatter})}, config());
  CHECK(asked.at("confirm.ask.compliance") == doctest::Approx(1.0 / 3.0));
  CHECK(asked.at("confirm.ask.neverExecuted") == doctest::Approx(2.0 / 3.0));

  eval::EvalCase go = selection("");
  go.script = {"desactiva la vigilancia", "sí, adelante"};
  go.confirm = eval::ConfirmExpectation{.tool = "modules.disable", .phase = "execute", .args = {}};
  const eval::TurnResult second{.reply = "Hecho.",
                                .executed = {{.tool = "modules.disable", .arguments = arguments("module", "surveillance")}},
                                .confirmed = {"modules.disable"}};
  CHECK(eval::aggregate({run(go, {previewed, second})}, config()).at("confirm.execute.pass") == doctest::Approx(1.0));
  CHECK(eval::aggregate({run(go, {executed, second})}, config()).at("confirm.execute.pass") == doctest::Approx(0.0));
  CHECK(eval::aggregate({run(go, {previewed, chatter})}, config()).at("confirm.execute.pass") == doctest::Approx(0.0));
}

TEST_CASE("accepting an offer enables the module and declining enables nothing")
{
  eval::EvalCase accept = selection("");
  accept.offerAccept = eval::OfferAccept{.module = "productivity", .tool = "modules.enable", .attempted = "calendar.create_event"};
  const eval::TurnResult yes{.reply = "Activada.", .executed = {{.tool = "modules.enable", .arguments = arguments("module", "productivity")}}};
  const eval::TurnResult wrong{.reply = "Activada.", .executed = {{.tool = "modules.enable", .arguments = arguments("module", "surveillance")}}};
  const eval::TurnResult offer{.reply = "¿La activo?"};
  CHECK(eval::aggregate({run(accept, {offer, yes}), run(accept, {offer, wrong})}, config()).at("offer.accept") == doctest::Approx(0.5));

  eval::EvalCase decline = selection("");
  decline.offerDecline = true;
  const eval::TurnResult no{.reply = "De acuerdo."};
  CHECK(eval::aggregate({run(decline, {offer, no}), run(decline, {offer, yes})}, config()).at("offer.decline") == doctest::Approx(0.5));
}
