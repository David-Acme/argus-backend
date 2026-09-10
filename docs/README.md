# Argus Backend Documentation

Argus is a local-first C++20 platform for home security and assistance: every
AI capability (face auth, LLM, vision, speech) runs on the user's hardware.
This directory is the documentation map; start here and follow the reading
order below.

## Reading order

1. [Architecture overview](architecture/system-overview.md) — what Argus is
   and how the processes fit together.
2. [Services and packages](architecture/services-and-packages.md) — who is a
   process, who is a library, and who owns each database.
3. [Data storage](architecture/data-storage.md) — per-owner SQLite databases,
   schemas and migrations.
4. [Sync engine](architecture/sync-engine.md) — the `/sync` WebSocket contract
   and the audit cursors.
5. [Events and contracts](architecture/events-and-contracts.md) — NATS
   subjects and typed gRPC/protobuf contracts.
6. [Build model](architecture/build-model.md) — the 19 standalone projects,
   their Conan graphs and the third-party dependency map.
7. [Build and test](operations/build-and-test.md)
8. [Provisioning and models](operations/provisioning-and-models.md)
9. [Configuration](operations/configuration.md)
10. [Docker deployment](operations/deployment-docker.md)
11. [Hardware tiers](operations/hardware-tiers.md)

## Contract reference

- [Contracts overview](architecture/contracts-overview.md) — the protobuf
  package and its versioning policy.
- [Sync wire](architecture/wire-sync-tables.md) — frozen `SyncOperation`,
  `TableName` and `SYNC_LIMIT`.
- [Device identity wire](architecture/wire-device-identity.md) — the
  `ip`/`credential` modes and the device-credential header.
- [Golden sync frames](architecture/wire-sync-golden-frames.md) — the frozen
  `/sync` fixtures and where they live.
- [NATS subjects](architecture/wire-nats-subjects.md) — the frozen subject
  contract.
- [Tool-calling evaluation set](operations/tool-calling-eval-set.md) — the
  labelled utterances behind the intent-router gates.

## History and traceability

- [Timeline](history/timeline.md) — every arc from the first build to today,
  with dates, milestones and links.
- [Project log](history/project-log.md) — the full decision record.
- [Plans](history/plans/) — the migration and tool-calling plans as executed.
- [Reports](history/reports/) — focused task reports.

## Repository rules

The binding engineering rules for every change live in the root
[`AGENTS.md`](../AGENTS.md). Each project folder carries its own `AGENTS.md`
(folder-specific rules) and `CONTEXT.md` (local decisions).
