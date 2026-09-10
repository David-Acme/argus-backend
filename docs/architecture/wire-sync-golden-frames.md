# Golden sync frames

JSON files recorded from the backend `/sync` stream (one file per scenario),
stored in `packages/argus-sync/tests/fixtures/sync/`. Frozen: they are the
reference for backend and frontend sync decoders. Any change to sync code
that would alter these frames is a contract break.