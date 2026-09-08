#include <iostream>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/reaction/reaction-engine.hxx>
#include <string>
#include <utility>
#include <vector>

namespace
{

int fails = 0;
ReactionEngine gEngine;

void check(bool ok, const std::string& what)
{
  std::cout << (ok ? "[ok] " : "[FAIL] ") << what << "\n";
  if (!ok)
    ++fails;
}

struct Case
{
  ReactionSignals signals;
  ReactionKind want;
  const char* because;
};

int reactionTest()
{
  ConfigService::load("config.toml");
  gEngine.init();
  check(gEngine.isLoaded(), "engine loaded its vocabulary");

  const std::vector<Case> cases = {
      // Priority: the first matching rule wins, top to bottom.
      {{.text = "se metio alguien en casa",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = 2,
        .cameraIntent = true,
        .sttFailed = false,
        .systemAlert = true},
       ReactionKind::Alarmed,
       "system_alert"},
      {{.text = "",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = true,
        .systemAlert = false},
       ReactionKind::Confused,
       "stt_failed"},
      {{.text = "no, olvidalo",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = 3,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Acknowledging,
       "retraction"},
      {{.text = "cuando viene mi hermana",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = 2,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Recognizing,
       "recall_hit"},
      {{.text = "recuerda que a Pedro no le gusta el pescado",
        .lang = "es",
        .captureStored = true,
        .captureQueued = false,
        .recallHits = 0,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Attentive,
       "capture_stored"},
      {{.text = "mi perro se llama Toby",
        .lang = "es",
        .captureStored = false,
        .captureQueued = true,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Attentive,
       "capture_queued"},
      {{.text = "que ves en la camara",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = true,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Curious,
       "camera_intent"},
      {{.text = "que me gusta tomar",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = 0,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Uncertain,
       "recall_empty"},
      {{.text = "cuanto es dos por dos",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Thinking,
       "question"},
      {{.text = "hola argus",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Warm,
       "small_talk"},
      {{.text = "el paquete llego esta manana sin novedad",
        .lang = "es",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Idle,
       "no_signal"},
      {{.text = "never mind, forget it",
        .lang = "en",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Acknowledging,
       "retraction"},
      {{.text = "what time is it",
        .lang = "en",
        .captureStored = false,
        .captureQueued = false,
        .recallHits = -1,
        .cameraIntent = false,
        .sttFailed = false,
        .systemAlert = false},
       ReactionKind::Thinking,
       "question"},
  };

  for (const auto& c : cases) {
    const Reaction got = gEngine.react(c.signals);
    const bool ok =
        got.kind == c.want && got.because == std::string(c.because);
    check(ok, std::string(c.because) + " -> " + reactionKindToString(c.want) +
                  " (\"" + c.signals.text + "\")");
    if (!ok)
      std::cout << "      got " << reactionKindToString(got.kind) << " because="
                << got.because << "\n";
  }

  // recallHits < 0 means recall never ran: that must not read as "empty".
  const ReactionSignals notConsulted{.text = "que me gusta tomar",
                                     .lang = "es",
                                     .captureStored = false,
                                     .captureQueued = false,
                                     .recallHits = -1,
                                     .cameraIntent = false,
                                     .sttFailed = false,
                                     .systemAlert = false};
  check(gEngine.react(notConsulted).kind == ReactionKind::Thinking,
        "recall not consulted is not the same as recall empty");

  // Intensity has to move with the evidence, not be a constant per kind.
  ReactionSignals oneHit{.text = "cuando viene mi hermana",
                         .lang = "es",
                         .captureStored = false,
                         .captureQueued = false,
                         .recallHits = 1,
                         .cameraIntent = false,
                         .sttFailed = false,
                         .systemAlert = false};
  ReactionSignals manyHits = oneHit;
  manyHits.recallHits = 3;
  const float weak = gEngine.react(oneHit).intensity;
  const float strong = gEngine.react(manyHits).intensity;
  check(strong > weak && strong <= 1.0F && weak > 0.0F,
        "recall intensity grows with hits (" + std::to_string(weak) + " -> " +
            std::to_string(strong) + ")");

  const Reaction alarmed =
      gEngine.react({.text = "alguien entro",
                     .lang = "es",
                     .captureStored = false,
                     .captureQueued = false,
                     .recallHits = -1,
                     .cameraIntent = false,
                     .sttFailed = false,
                     .systemAlert = true});
  check(alarmed.intensity == 1.0F, "an alarm is full intensity");

  check(!ReactionEngine::toneNote(alarmed, "es").empty(),
        "alarmed carries a tone note");
  check(ReactionEngine::toneNote({.kind = ReactionKind::Idle,
                                  .intensity = 0.0F,
                                  .because = {}},
                                 "es")
            .empty(),
        "idle carries no tone note");
  check(ReactionEngine::toneNote({.kind = ReactionKind::Thinking,
                                  .intensity = 0.45F,
                                  .because = {}},
                                 "es")
            .empty(),
        "thinking carries no tone note (it is the common case)");

  const std::pair<std::string, std::string> uncertainByLang[] = {
      {"es", "que me gusta tomar"}, {"en", "what do i like to drink"}};
  for (const auto& [lang, text] : uncertainByLang) {
    const Reaction r = gEngine.react({.text = text,
                                      .lang = lang,
                                      .captureStored = false,
                                      .captureQueued = false,
                                      .recallHits = 0,
                                      .cameraIntent = false,
                                      .sttFailed = false,
                                      .systemAlert = false});
    check(r.kind == ReactionKind::Uncertain &&
              !ReactionEngine::toneNote(r, lang).empty(),
          "uncertain has a tone note in " + lang);
  }

  check(reactionKindFromString(reactionKindToString(ReactionKind::Recognizing)) ==
            ReactionKind::Recognizing,
        "wire name round-trips");

  return fails == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
  const std::string mode = argc > 1 ? argv[1] : "--reaction-test";
  if (mode == "--reaction-test")
    return reactionTest();
  std::cout << "argus-reaction-probe\n  --reaction-test\n";
  return 1;
}
