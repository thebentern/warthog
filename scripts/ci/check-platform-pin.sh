#!/bin/sh
# Fails unless platformio.ini, PlatformIO and (with --installed) the installed platform, its penv and its packages
# are the ones scripts/ci/pins.env and the pinned platform's platform.json name.
#
# Usage: scripts/ci/check-platform-pin.sh [--installed]
#   PIO (default: pio), PLATFORMIO_CORE_DIR (default: ~/.platformio; --installed wants a core dir this project
#   alone uses), PYTHON (default: python3). PIO_PLATFORM and PIO_VERSION in the environment must equal pins.env's.
#   Exit 0 ok, 1 failed, 2 usage or setup error.

set -u
unset CDPATH

ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
INI="$ROOT/platformio.ini"
PINS="$ROOT/scripts/ci/pins.env"
# PlatformIO's core dir as pio reads it: a leading ~ expanded, a relative one made absolute.
CORE=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}
case $CORE in "~"/*) CORE=$HOME/${CORE#"~/"} ;; /*) ;; *) CORE=$PWD/$CORE ;; esac
PIO="${PIO:-pio}"
INSTALLED=0
case "${1:-}" in
  --installed) INSTALLED=1 ;;
  '') ;;
  *) echo "usage: $0 [--installed]" >&2; exit 2 ;;
esac
[ -f "$INI" ] || { echo "error: no $INI" >&2; exit 2; }
pin() { sed -n "s/^$1=\\([^ ]*\\)$/\\1/p" "$PINS" 2>/dev/null | head -n 1; }
WANT_PLATFORM=$(pin PIO_PLATFORM) WANT_PIO=$(pin PIO_VERSION)
[ -n "$WANT_PLATFORM" ] && [ -n "$WANT_PIO" ] || { echo "error: no PIO_PLATFORM= and PIO_VERSION= in $PINS" >&2; exit 2; }
URL="https://github.com/pioarduino/platform-espressif32/releases/download/${WANT_PLATFORM}/platform-espressif32.zip"

fail=0
ok()   { echo "ok   $1"; }
bad()  { echo "FAIL $1"; fail=1; }

# The workflows load pins.env into their environment; a value set anywhere else must not differ.
[ "${PIO_PLATFORM:-$WANT_PLATFORM}" = "$WANT_PLATFORM" ] || bad "PIO_PLATFORM is ${PIO_PLATFORM} in the environment, $WANT_PLATFORM in pins.env"
[ "${PIO_VERSION:-$WANT_PIO}" = "$WANT_PIO" ] || bad "PIO_VERSION is ${PIO_VERSION} in the environment, $WANT_PIO in pins.env"

# Every env's platform, as PlatformIO reads platformio.ini (extends, extra_configs, key case, '=' or ':').
if ! cfg=$(cd "$ROOT" && "$PIO" project config --json-output); then
  bad "$PIO project config failed"
elif ! envs=$(printf '%s\n' "$cfg" | "${PYTHON:-python3}" -I -c '
import json, re, sys
for s, opts in json.load(sys.stdin):
    if s.startswith("env:"):
        o = dict(opts)
        pk = o.get("platform_packages") or []
        pk = [pk] if isinstance(pk, str) else pk
        idf = [re.sub(r"^framework-espidf\s*@\s*", "", p.strip()) for p in pk if re.match(r"^framework-espidf\s*@", p.strip())]
        print(s, o.get("platform") or "(none)", idf[0] if idf else "-")'); then
  bad "$PIO project config printed no JSON the env list could be read from"
elif [ -z "$envs" ]; then
  bad "platformio.ini has no env"
else
  wrong=$(printf '%s\n' "$envs" | awk -v want="$URL" '$2 != want')
  if [ -n "$wrong" ]; then
    printf '%s\n' "$wrong" | while read -r s p _; do echo "FAIL platformio.ini [$s] platform = $p (want $URL)"; done
    fail=1
  else
    ok "platformio.ini: all $(printf '%s\n' "$envs" | wc -l | tr -d ' ') envs pin $WANT_PLATFORM"
  fi
  # A framework-espidf override in platform_packages replaces the platform's IDF; every env must name the same one.
  IDF_URL=$(printf '%s\n' "$envs" | awk '{print $3}' | sort -u)
  case $IDF_URL in
    *"
"*) bad "platformio.ini envs override framework-espidf differently: $(printf '%s' "$IDF_URL" | tr '\n' ' ')" ;;
    -) IDF_URL="" ;;
    *) ok "platformio.ini: every env takes framework-espidf from $IDF_URL" ;;
  esac
fi

pio_is() {
  got=$("$1" --version 2>&1)
  if [ "$got" = "PlatformIO Core, version $WANT_PIO" ]; then ok "$2: PlatformIO Core $WANT_PIO"
  else bad "$2 --version printed '$got', want 'PlatformIO Core, version $WANT_PIO'"; fi
}
pio_is "$PIO" "$PIO"

# The platform installs an unpinned pioarduino into its penv; UV_CONSTRAINT names this file to hold it at the pin.
CONS="$ROOT/scripts/ci/penv-constraints.txt"
if ! cons=$(grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$CONS" 2>/dev/null) && [ ! -r "$CONS" ]; then
  bad "no readable $CONS"
elif [ "$cons" = "pioarduino==$WANT_PIO" ]; then
  ok "penv-constraints.txt: pioarduino==$WANT_PIO"
else
  bad "penv-constraints.txt holds '${cons:-nothing}', want exactly pioarduino==$WANT_PIO"
fi
if [ -z "${UV_CONSTRAINT:-}" ]; then
  [ "${GITHUB_ACTIONS:-}" != true ] || bad "UV_CONSTRAINT is not set; CI must set it to $CONS"
else
  case $UV_CONSTRAINT in
    *" "*) bad "UV_CONSTRAINT '$UV_CONSTRAINT' holds a space; uv reads it as a list of files, so use a checkout path without one" ;;
    *) uvc=$(cd "$(dirname "$UV_CONSTRAINT")" 2>/dev/null && pwd -P)/$(basename "$UV_CONSTRAINT")
       [ "$uvc" = "$CONS" ] || bad "UV_CONSTRAINT is $UV_CONSTRAINT, want $CONS" ;;
  esac
fi

if [ "$INSTALLED" = 1 ]; then
  count=0 plat=""
  for d in "$CORE"/platforms/espressif32*; do
    [ -d "$d" ] || continue
    count=$((count + 1)) plat=$d
    uri=$(sed -n 's/.*"uri"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$d/.piopm" 2>/dev/null | head -n 1)
    if [ "$uri" = "$URL" ]; then
      ok "${d#"$CORE"/} is $WANT_PLATFORM"
    else
      bad "${d#"$CORE"/} was installed from ${uri:-an unknown uri (no .piopm)}, want $URL"
    fi
  done
  [ "$count" != 0 ] || { echo "error: no $CORE/platforms/espressif32* (run a pio build first)" >&2; exit 2; }
  [ "$count" = 1 ] || bad "$count espressif32 platforms in $CORE/platforms; want exactly one"
  # pioarduino's penv runs the build; penv_setup may upgrade it past the pin.
  if [ -x "$CORE/penv/bin/pio" ]; then pio_is "$CORE/penv/bin/pio" penv/bin/pio
  else bad "no $CORE/penv/bin/pio"; fi
  # Each installed package against platform.json: framework-espidf by its download URL (and version.txt), the
  # idf_tools-installed ones by package-version, as platform.py compares them. platform.json lists others as info.
  out=$("${PYTHON:-python3}" -I - "$CORE" "$plat/platform.json" "${IDF_URL:-}" <<'EOF'
import json, os, re, sys
core, pj, idf_url = sys.argv[1], sys.argv[2], sys.argv[3]
pk = json.load(open(pj))["packages"]
if idf_url:
    pk["framework-espidf"] = dict(pk.get("framework-espidf", {}), version=idf_url)
def load(p):
    try:
        return json.load(open(p))
    except (OSError, ValueError):
        return None
names = sorted(n for n in os.listdir(os.path.join(core, "packages")) if not n.startswith("."))
for need in ("framework-espidf", "toolchain-xtensa-esp-elf", "tool-esptoolpy"):
    if need not in names:
        print("FAIL packages/%s is not installed" % need)
for n in names:
    d = os.path.join(core, "packages", n)
    spec = pk.get(n)
    base = re.split(r"[@.]", n, maxsplit=1)[0]
    if spec is None and base != n and base in pk:
        print("FAIL packages/%s is a second copy of %s" % (n, base))
        continue
    meta = load(os.path.join(d, "package.json")) or {}
    have = meta.get("version", "")
    if spec is None:
        print("info packages/%s %s: platform.json does not list it" % (n, have or "(no package.json)"))
    elif "package-version" in spec:
        want = spec["package-version"]
        print(("ok   packages/%s %s, platform.json's package-version" if have == want else
               "FAIL packages/%s is %s, platform.json's package-version is " + want) % (n, have or "(no package.json)"))
    elif re.match(r"https?://", spec.get("version", "")):
        want = spec["version"]
        uri = ((load(os.path.join(d, ".piopm")) or {}).get("spec") or {}).get("uri") or ""
        if uri != want:
            print("FAIL packages/%s was installed from %s, %s %s" % (n, uri or "an unknown uri (no .piopm)", "platformio.ini overrides it with" if n == "framework-espidf" and idf_url else "platform.json pins", want))
            continue
        line = "ok   packages/%s from %s, %s" % (n, want, "platformio.ini's override" if n == "framework-espidf" and idf_url else "platform.json's pin")
        if n == "framework-espidf":
            m = re.search(r"/v(\d+\.\d+\.\d+)/", want)
            try:
                txt = open(os.path.join(d, "version.txt")).readline().strip().lstrip("v")
            except OSError:
                txt = ""
            if not m or txt != m.group(1):
                line = "FAIL packages/%s version.txt is %s, its pinned URL is %s" % (n, txt or "(none)", m.group(1) if m else want)
            else:
                line += ", version.txt " + txt
        print(line)
    else:
        print("info packages/%s %s: platform.json pins no version" % (n, have))
EOF
)
  rc=$?
  [ -n "$out" ] && printf '%s\n' "$out"
  if [ "$rc" != 0 ]; then bad "the package comparison exited $rc"
  elif printf '%s\n' "$out" | grep -q '^FAIL'; then fail=1; fi
fi

if [ "$fail" = 0 ]; then echo "PLATFORM PIN OK"; else echo "PLATFORM PIN FAILED"; fi
exit "$fail"
