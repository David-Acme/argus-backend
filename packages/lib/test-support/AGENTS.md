# argus-test-support

What the test binaries of several services would otherwise each carry a copy of:
the part that starts Drogon's application on a thread and stops it again.

## What this is

A PACKAGE, header-only (INTERFACE), tier 1: it depends on Drogon alone. A test
links `argus::lib::test-support`; no service links it.

`test_support::AppRunner` runs `drogon::app().run()` on its own thread and stops
it in its destructor: wait for the main loop, `quit()`, `join()`.

## The one rule behind it

The constructor returns only once the main loop is running. By then every
singleton Drogon builds in `run()` (the controllers router, the static file
router) exists, so any runner that lives in static storage is finished
constructing after them and is destroyed before them, which is the order
`quit()` needs: its loop callback resets those singletons. A runner that returned
earlier crashed at exit in about one run in ten whenever it sat in a
function-local static, because the singletons then finished construction after
it and were destroyed first.

A test that needs the app for a single test case keeps the runner a local
variable of that case.

## The gate

`tests/check-runner-lifetime.py` is the ctest `test-runner-lifetime-test`: it
scans every test source under `services/*/tests` and `packages/*/*/tests` and
fails on a runner a file defines itself (`class AppRunner`) and then keeps in
static storage: a `static` AppRunner, a `static` initializer that builds one, or a
`static` object of a class that holds one. A file that takes the runner from
`test-support/app-runner.hxx` may keep it anywhere. `test-runner-lifetime-selftest`
runs the same rules over `tests/fixtures`, whose `bad-*` samples must be flagged and
whose `good-*` samples must not.
