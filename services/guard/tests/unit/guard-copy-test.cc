#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/guard/guard-copy.hxx>

#include <string>

namespace
{
GuardNotice episode()
{
  return {.kind = NoticeKind::Episode,
          .subject = NoticeSubject::Stranger,
          .people = 1,
          .cameraId = 7,
          .cameraName = "Puerta",
          .environmentName = {},
          .role = CameraRole::Entrance,
          .outdoor = true,
          .zoneName = {},
          .reasons = {GuardReason::AfterHours, GuardReason::Night,
                      GuardReason::AlertZone},
          .dwellS = 18,
          .danger = GuardDanger::High,
          .action = NoticeAction::Speaker,
          .tamperStatus = {},
          .held = {},
          .routine = {},
          .notified = 0,
          .afterQuiet = false};
}
}

TEST_CASE("an episode reads who, where, why and what Argus did, in Spanish")
{
  const NoticeText text = guard_copy::render(episode(), "es");
  CHECK(text.title == "Persona desconocida · Puerta");
  CHECK(text.body == "En la entrada, fuera de horario, de noche, desde hace "
                     "18 s. Argus le está avisando por el altavoz.");
}

TEST_CASE("the same episode in English")
{
  const NoticeText text = guard_copy::render(episode(), "en-GB");
  CHECK(text.title == "Unknown person · Puerta");
  CHECK(text.body == "At the entrance, after hours, at night, for 18s. Argus "
                     "is warning them over the speaker.");
}

TEST_CASE("at most two reasons, a weapon leads, minutes for long visits")
{
  GuardNotice notice = episode();
  notice.reasons = {GuardReason::Weapon, GuardReason::NobodyHome,
                    GuardReason::Night, GuardReason::AlertZone};
  notice.dwellS = 200;
  notice.action = NoticeAction::SilentWeapon;
  const NoticeText text = guard_copy::render(notice, "es");
  CHECK(text.body == "Posible arma a la vista. En la entrada, sin nadie dentro, "
                     "de noche, desde hace 3 min. Argus se mantiene en silencio "
                     "para no poner a nadie en riesgo.");
}

TEST_CASE("zone names replace the generic zone wording")
{
  GuardNotice notice = episode();
  notice.zoneName = "Puerta trasera";
  notice.reasons = {GuardReason::AlertZone};
  CHECK(guard_copy::render(notice, "es").body ==
        "En la entrada, en «Puerta trasera», desde hace 18 s. Argus "
        "le está avisando por el altavoz.");
  notice.reasons = {};
  notice.zoneName = "Porche";
  CHECK(guard_copy::render(notice, "en").body ==
        "At the entrance, in “Porche”, for 18s. Argus is warning "
        "them over the speaker.");
}

TEST_CASE("an escalation updates the same story instead of starting a new one")
{
  GuardNotice notice = episode();
  notice.kind = NoticeKind::Escalation;
  notice.danger = GuardDanger::Critical;
  notice.action = NoticeAction::Alarm;
  const NoticeText text = guard_copy::render(notice, "es");
  CHECK(text.title == "Sigue en Puerta · riesgo crítico");
  CHECK(text.body.ends_with("La alarma de la cámara está sonando."));
  CHECK(guard_copy::render(notice, "en").title ==
        "Still at Puerta · critical risk");
}

TEST_CASE("subjects and unnamed cameras")
{
  GuardNotice notice = episode();
  notice.cameraName = {};
  notice.subject = NoticeSubject::Several;
  notice.people = 3;
  CHECK(guard_copy::render(notice, "es").title ==
        "3 personas desconocidas · Cámara 7");
  notice.subject = NoticeSubject::Unobserved;
  notice.reasons = {GuardReason::FaceHidden};
  CHECK(guard_copy::render(notice, "en").title ==
        "Someone unidentified · Camera 7");
  CHECK(guard_copy::render(notice, "en").body.find("face") == std::string::npos);
}

TEST_CASE("tamper notices ask to check the camera")
{
  GuardNotice notice = episode();
  notice.kind = NoticeKind::Tamper;
  notice.tamperStatus = "covered";
  notice.dwellS = 360;
  const NoticeText text = guard_copy::render(notice, "es");
  CHECK(text.title == "Revisa la cámara Puerta");
  CHECK(text.body ==
        "La imagen está tapada desde hace 6 min. Argus no ve esa zona.");
}

TEST_CASE("digests summarize held alerts and routine activity")
{
  GuardNotice notice{.kind = NoticeKind::Digest,
                     .subject = NoticeSubject::Stranger,
                     .people = 0,
                     .cameraId = 0,
                     .cameraName = {},
                     .environmentName = {},
                     .role = CameraRole::Other,
                     .outdoor = false,
                     .zoneName = {},
                     .reasons = {},
                     .dwellS = 0,
                     .danger = GuardDanger::Low,
                     .action = NoticeAction::Watching,
                     .tamperStatus = {},
                     .held = {},
                     .routine = {{.cameraId = 1, .cameraName = "Cocina", .count = 23},
                                 {.cameraId = 2, .cameraName = "Calle", .count = 9},
                                 {.cameraId = 3, .cameraName = "Entrada", .count = 2},
                                 {.cameraId = 4, .cameraName = {}, .count = 1}},
                     .notified = 0,
                     .afterQuiet = false};
  NoticeText text = guard_copy::render(notice, "es");
  CHECK(text.title == "Resumen de vigilancia");
  CHECK(text.body == "Nada requirió tu atención. Actividad normal: Cocina 23, "
                     "Calle 9, Entrada 2 y 1 más.");
  notice.afterQuiet = true;
  notice.held = {{.cameraId = 3, .cameraName = "Entrada", .count = 2}};
  notice.routine = {};
  text = guard_copy::render(notice, "en");
  CHECK(text.title == "While you were resting");
  CHECK(text.body == "Argus held 2 alerts: Entrada 2.");
}

TEST_CASE("urgency follows danger, digests are passive")
{
  GuardNotice notice = episode();
  CHECK(guard_copy::urgency(notice) == "time_sensitive");
  notice.danger = GuardDanger::Critical;
  CHECK(guard_copy::urgency(notice) == "critical");
  notice.danger = GuardDanger::Medium;
  CHECK(guard_copy::urgency(notice) == "active");
  notice.kind = NoticeKind::Digest;
  CHECK(guard_copy::urgency(notice) == "passive");
}

TEST_CASE("languages normalize to es or en with a fallback")
{
  CHECK(guard_copy::normalizeLang({.requested = "EN-us", .fallback = "es"}) ==
        "en");
  CHECK(guard_copy::normalizeLang({.requested = "es_ES", .fallback = "en"}) ==
        "es");
  CHECK(guard_copy::normalizeLang({.requested = "fr", .fallback = "en"}) ==
        "en");
  CHECK(guard_copy::normalizeLang({.requested = "", .fallback = "pt"}) == "es");
}

TEST_CASE("with several environments the notice names the place")
{
  GuardNotice notice = episode();
  notice.environmentName = "Trattoria";
  CHECK(guard_copy::render(notice, "es").title ==
        "Persona desconocida · Puerta (Trattoria)");
  notice.kind = NoticeKind::Escalation;
  notice.danger = GuardDanger::Critical;
  CHECK(guard_copy::render(notice, "en").title ==
        "Still at Puerta (Trattoria) · critical risk");
  notice.kind = NoticeKind::Tamper;
  notice.tamperStatus = "covered";
  CHECK(guard_copy::render(notice, "es").title ==
        "Revisa la cámara Puerta (Trattoria)");
  notice.kind = NoticeKind::Digest;
  notice.routine = {{.cameraId = 1, .cameraName = "Cocina", .count = 4}};
  CHECK(guard_copy::render(notice, "es").title ==
        "Resumen de vigilancia · Trattoria");
  notice.afterQuiet = true;
  CHECK(guard_copy::render(notice, "en").title ==
        "While you were resting · Trattoria");
}

TEST_CASE("a seeded environment is named after its kind in the owner's language")
{
  CHECK(guard_copy::environmentDefaultName(EnvironmentKind::Home, "es") == "Casa");
  CHECK(guard_copy::environmentDefaultName(EnvironmentKind::Commercial, "es") ==
        "Local");
  CHECK(guard_copy::environmentDefaultName(EnvironmentKind::Restaurant, "en-US") ==
        "Restaurant");
  CHECK(guard_copy::environmentDefaultName(EnvironmentKind::Warehouse, "fr") ==
        "Almacén");
}
