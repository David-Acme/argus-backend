#!/bin/bash
usage() {
  cat <<'EOF'
usage: measure-guard.sh --build <build-dir> --target <ninja-target> [--config <file>]
                        [--profile dev|prod] [--source <dir>] [--] <command...>

The one gate a measurement runs through. It refuses to start the command when
the tree that built it has uncommitted changes, or when the artefact the target
names is older than a tracked source file, and it prints the measurement key the
report is filed under:

  measure: commit=<sha> build_type=<Debug|Release> profile=<dev|prod> config=<path>

--profile defaults to prod: a dev build is reachable only by passing
--profile dev, and the build directory must then be build/dev.

Both refusals are conservative by design and both are stated in the message:
an untracked scratch file in the tree counts as an uncommitted change, and any
tracked file newer than the artefact counts as a rebuild owed — even one the
target does not read.

Exit 2 is a refusal: no measurement was taken.
EOF
}

refuse() {
  printf 'REFUSED: %s\n' "$1" >&2
  exit 2
}

BUILD=""
TARGET=""
CONFIG="-"
SOURCE=""
PROFILE="prod"
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help) usage; exit 0 ;;
    --build) [ $# -ge 2 ] || { usage; exit 1; }; BUILD="$2"; shift ;;
    --target) [ $# -ge 2 ] || { usage; exit 1; }; TARGET="$2"; shift ;;
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
[ -n "$TARGET" ] || { usage; exit 1; }
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
  refuse "$SOURCE has uncommitted changes (untracked files count); a measurement refuses to run on a build tree that is not its commit:
$DIRTY"

COMMIT="$(git -C "$SOURCE" rev-parse HEAD)"
BUILD_TYPE="$(awk -F= '/^CMAKE_BUILD_TYPE:/ { print $2; exit }' "$BUILD/CMakeCache.txt")"
[ -n "$BUILD_TYPE" ] || refuse "$BUILD/CMakeCache.txt declares no CMAKE_BUILD_TYPE"

ARTEFACT=""
[ -f "$BUILD/$TARGET" ] && ARTEFACT="$TARGET"
if [ -z "$ARTEFACT" ]; then
  ARTEFACT="$(ninja -C "$BUILD" -t query "$TARGET" 2>/dev/null |
    awk '/^  input:/ { inside = 1; next } inside && /^    / { print $1; exit }')"
fi
[ -n "$ARTEFACT" ] || ARTEFACT="$TARGET"
[ -f "$BUILD/$ARTEFACT" ] ||
  refuse "$BUILD/$ARTEFACT is missing; the target $TARGET has not been built"

ARTEFACT_TS="$(stat -c %Y "$BUILD/$ARTEFACT")"
NEWER="$(git -C "$SOURCE" ls-files -z |
  (cd "$SOURCE" && xargs -0 -r stat -c '%Y %n' 2>/dev/null) |
  awk -v t="$ARTEFACT_TS" '$1 + 0 > t { print $2 }')"
[ -z "$NEWER" ] ||
  refuse "$(printf 'a tracked file is newer than %s, so it holds code the commit does not; rebuild before measuring:\n%s' \
    "$ARTEFACT" "$(printf '%s\n' "$NEWER" | head -5)")"

printf 'measure: commit=%s build_type=%s profile=%s config=%s\n' "$COMMIT" "$BUILD_TYPE" "$PROFILE" "$CONFIG" >&2

[ $# -gt 0 ] || exit 0
exec "$@"
