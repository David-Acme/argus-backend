#!/bin/bash
usage() {
  cat <<'EOF'
usage: measure-guard.sh --build <build-dir> [--config <file>] [--profile dev|prod]
                        [--source <dir>] [--] <command...>

The one gate a measurement runs through. It refuses to start the command when
the tree that built it has uncommitted changes, or when the build predates
HEAD, and it prints the measurement key the report is filed under:

  measure: commit=<sha> build_type=<Debug|Release> profile=<dev|prod> config=<path>

--profile defaults to prod: a dev build is reachable only by passing
--profile dev, and the build directory must then be build/dev.

Exit 2 is a refusal: no measurement was taken.
EOF
}

refuse() {
  printf 'REFUSED: %s\n' "$1" >&2
  exit 2
}

BUILD=""
CONFIG="-"
SOURCE=""
PROFILE="prod"
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help) usage; exit 0 ;;
    --build) [ $# -ge 2 ] || { usage; exit 1; }; BUILD="$2"; shift ;;
    --config) [ $# -ge 2 ] || { usage; exit 1; }; CONFIG="$2"; shift ;;
    --source) [ $# -ge 2 ] || { usage; exit 1; }; SOURCE="$2"; shift ;;
    --profile) [ $# -ge 2 ] || { usage; exit 1; }; PROFILE="$2"; shift ;;
    --) shift; break ;;
    -*) usage; exit 1 ;;
    *) break ;;
  esac
  shift
done

case "$PROFILE" in
  dev | prod) ;;
  *) refuse "unknown profile '$PROFILE'; expected dev or prod" ;;
esac

[ -n "$BUILD" ] || { usage; exit 1; }
[ -d "$BUILD" ] || refuse "no build directory at $BUILD"
[ -f "$BUILD/CMakeCache.txt" ] || refuse "$BUILD has no CMakeCache.txt; it is not a configured build"

if [ -z "$SOURCE" ]; then
  SOURCE="$(git -C "$BUILD" rev-parse --show-toplevel 2>/dev/null)" ||
    refuse "cannot find the source tree above $BUILD; pass --source explicitly"
fi

case "$BUILD" in
  */build/"$PROFILE") ;;
  *)
    refuse "$BUILD is not build/$PROFILE; pass --profile dev explicitly to measure a dev build"
    ;;
esac

DIRTY="$(git -C "$SOURCE" status --porcelain)"
[ -z "$DIRTY" ] ||
  refuse "$SOURCE has uncommitted changes; a measurement refuses to run on a build tree that is not its commit:
$DIRTY"

COMMIT="$(git -C "$SOURCE" rev-parse HEAD)"
BUILD_TYPE="$(awk -F= '/^CMAKE_BUILD_TYPE:/ { print $2; exit }' "$BUILD/CMakeCache.txt")"
[ -n "$BUILD_TYPE" ] || refuse "$BUILD/CMakeCache.txt declares no CMAKE_BUILD_TYPE"

HEAD_TS="$(git -C "$SOURCE" log -1 --format=%ct HEAD)"
if [ -z "$(find "$BUILD" -type f -newermt "@$HEAD_TS" -print -quit 2>/dev/null)" ]; then
  refuse "the build in $BUILD predates HEAD ($COMMIT); rebuild before measuring"
fi

printf 'measure: commit=%s build_type=%s profile=%s config=%s\n' "$COMMIT" "$BUILD_TYPE" "$PROFILE" "$CONFIG" >&2

[ $# -gt 0 ] || exit 0
exec "$@"
