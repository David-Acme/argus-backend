# Legal and privacy obligations of an installation

Argus is self-hosted. Whoever installs and operates it (a household, or the
business that runs a restaurant or office environment) is the controller of
the data it processes and is responsible for using it lawfully where it is
installed. The author follows the rules, ships privacy-protective defaults and
the controls below, and documents the obligations; how each installation uses
them is outside the author's control. Argus is pre-beta software provided "as
is", without warranties, and is not a certified security or alarm system.

This document is not legal advice. Before any commercial release a lawyer of
the target country reviews the wording; consumer law may limit what a waiver
can exclude.

## Where the installer accepts it

- `scripts/setup.sh` and `scripts/provision-host.sh` print the host notice
  (`scripts/privacy/notice.{es,en}.md`, version 2) and refuse to configure
  anything until it is accepted (`--accept-privacy-notice` for a
  non-interactive run). The acceptance is recorded with the notice's SHA-256 in
  `<data>/privacy/host-consent.json` (0600) and appended to
  `host-consent.log`.
- Two optional acknowledgements follow, both off unless accepted:
  - recurring visitors (`--accept-visitor-notice`,
    `scripts/privacy/visitors.*.md`);
  - the watchman through the cameras (`--accept-watchman-notice`,
    `scripts/privacy/watchman.*.md`).
- The app asks every person for their own choices against the same notice
  version (`kPrivacyNoticeVersion`, identity); a new version asks again.

## Peru (the jurisdiction the defaults are written for)

The country references live in `scripts/privacy/jurisdictions.tsv`; adding a
row adds a jurisdiction to the rendered notices.

- **Domestic processing.** Ley N° 29733 and its reglamento (D.S. N°
  016-2024-JUS) exempt purely domestic processing by individuals.
- **Video surveillance.** The ANPD directive N° 01-2020-JUS/DGTAIPD:
  - counts audio as camera data;
  - keeps the domestic exemption unless recording is constant, reachable from
    the internet and covers common or public areas;
  - asks for signage at each access and a retention of 30 to 60 days;
  - forbids sound capture in a domestic worker's rest areas.
- **Biometric data of third parties.** Faces and voices are sensitive data.
  Recognising people outside the household (visitors, workers, passers-by)
  needs their consent, in writing for biometrics.

## The controls Argus ships

| Control | Default | Where |
|---|---|---|
| Per-person privacy choices (presence, face on cameras, voice learning, camera audio, location) | asked on first use; location is an opt-out | app, `PATCH /privacy/me` |
| Household ceilings for each choice | on | Owner, `PATCH /privacy/household` |
| Recurring-visitor recognition | off; needs the acknowledgement; unnamed visitors expire after 1–60 days | Owner, `PATCH /privacy/household {visitorRecognition, acknowledge}` |
| The watchman (Argus listens through cameras) | off; needs the acknowledgement | Owner, `PATCH /privacy/household {watchman, acknowledge}` |
| Per-camera watchman flag | off | `PATCH /camera/{id} {watchman}` |
| Per-camera "no audio" (for rest areas) | off | `PATCH /camera/{id} {noAudio}` |
| Lens privacy mask | per camera | `PATCH /camera/{id}/privacy` |

A camera listens only when the household watchman is on, the household and
the person allow camera audio, the camera's watchman flag is on, "no audio" is
off and the lens is not masked. No audio is kept before a confirmed wake, only
the conversation's own turns are kept (with the voice retention), an earcon
marks the start of every conversation, and everything runs on the installer's
hardware. A camera conversation never authorises a sensitive action: it holds
only the camera capability set (`kCameraWatchCapabilities`, `lib/auth`), and a
stranger only the visitor set (`kCameraVisitorCapabilities`).
