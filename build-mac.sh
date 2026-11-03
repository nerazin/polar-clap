#!/bin/sh
# Polar Reconstruction CLAP — one-command macOS build.
#
# Produces the arm64 (Apple Silicon) .clap bundle and installs it where
# Bitwig Studio looks for CLAP plugins:
#   ~/Library/Audio/Plug-Ins/CLAP/
# ...then verifies the installed bundle exactly the way a host loads it.
#
# Requirements: Apple Command Line Tools (clang), CMake, and nothing else.
#   xcode-select --install        # if no developer tools yet
#   brew install cmake            # if cmake is missing
set -eu
cd "$(dirname "$0")"

if ! xcode-select -p >/dev/null 2>&1; then
    echo "error: Apple Command Line Tools not found." >&2
    echo "  Run: xcode-select --install" >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found." >&2
    echo "  Run: brew install cmake   (or download from cmake.org)" >&2
    exit 1
fi

BUILD=build-mac
echo "==> Configuring (arm64, macOS 11+)..."
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

echo "==> Building..."
cmake --build "$BUILD" --parallel

SRC="$BUILD/PolarReconstruction.clap"
if [ ! -d "$SRC" ]; then
    echo "error: bundle not found at $SRC" >&2
    exit 1
fi

DEST_DIR="${HOME}/Library/Audio/Plug-Ins/CLAP"
DEST="$DEST_DIR/Polar Reconstruction.clap"
mkdir -p "$DEST_DIR"
rm -rf "$DEST"
cp -R "$SRC" "$DEST"

# Ad-hoc signature (required for arm64 code to load; harmless if re-signing).
codesign --force --sign - "$DEST" >/dev/null 2>&1 || \
    echo "note: codesign skipped (binary keeps its implicit ad-hoc signature)"

# ---------------------------------------------------------------------------
# Post-install verification — everything a host checks before the plugin can
# show up in the browser. A failure here is a plugin Bitwig cannot load.
# ---------------------------------------------------------------------------
echo "==> Verifying installed bundle..."

PLIST="$DEST/Contents/Info.plist"
BIN_DIR="$DEST/Contents/MacOS"

# 1. Info.plist must parse.
plutil -lint "$PLIST" >/dev/null || { echo "error: $PLIST is malformed" >&2; exit 1; }

# 2. The executable named in Info.plist must exist in Contents/MacOS/.
EXEC_NAME=$(plutil -extract CFBundleExecutable raw "$PLIST")
BIN="$BIN_DIR/$EXEC_NAME"
if [ ! -f "$BIN" ]; then
    echo "error: Info.plist names executable '$EXEC_NAME', but '$BIN' is missing" >&2
    exit 1
fi

# 3. The binary must be executable.
chmod +x "$BIN"

# 4. The binary must export the spec-mandated entry symbol 'clap_entry'.
#    (macOS nm prefixes symbol names with '_'.)
if ! nm -gU "$BIN" 2>/dev/null | grep -q '_clap_entry'; then
    echo "error: $EXEC_NAME does not export 'clap_entry'." >&2
    echo "       Hosts would reject the bundle (\"Not a plug-in file\")" >&2
    exit 1
fi

# 5. The (ad-hoc) code signature must verify.
codesign --verify --force "$DEST" 2>/dev/null || \
    { echo "error: code signature of $DEST failed verification" >&2; exit 1; }

# 6. Live load check: dlopen the binary, resolve clap_entry, call
#    init -> get_factory -> get_plugin_descriptor, i.e. precisely the
#    metadata flow Bitwig's indexer performs.
if command -v python3 >/dev/null 2>&1; then
    python3 - "$BIN" "$DEST" <<'PY' || exit 1
import ctypes, sys

binary, bundle = sys.argv[1], sys.argv[2]

class Version(ctypes.Structure):
    _fields_ = [("major", ctypes.c_uint32),
                ("minor", ctypes.c_uint32),
                ("revision", ctypes.c_uint32)]

class Entry(ctypes.Structure):
    _fields_ = [("clap_version", Version),
                ("init", ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_char_p)),
                ("deinit", ctypes.CFUNCTYPE(None)),
                ("get_factory", ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p))]

class Factory(ctypes.Structure):
    _fields_ = [("get_plugin_count", ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)),
                ("get_plugin_descriptor", ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32))]

class Descriptor(ctypes.Structure):
    _fields_ = [("clap_version", Version),
                ("id", ctypes.c_char_p),
                ("name", ctypes.c_char_p),
                ("vendor", ctypes.c_char_p),
                ("url", ctypes.c_char_p),
                ("manual_url", ctypes.c_char_p),
                ("support_url", ctypes.c_char_p),
                ("version", ctypes.c_char_p),
                ("description", ctypes.c_char_p)]

def die(msg):
    sys.stderr.write("  load check FAILED: %s\n" % msg)
    sys.exit(1)

try:
    lib = ctypes.CDLL(binary)
except OSError as e:
    die("dlopen failed (arch/signature/dependencies?): %s" % e)
try:
    sym = getattr(lib, "clap_entry")
except AttributeError:
    die("symbol 'clap_entry' not found")

entry = Entry.from_address(ctypes.cast(sym, ctypes.c_void_p).value)
v = entry.clap_version
if v.major != 1 or v.minor < 1:
    die("unsupported CLAP version %d.%d.%d" % (v.major, v.minor, v.revision))
if not entry.init(bundle.encode()):
    die("clap_entry.init() returned false")
f = entry.get_factory(b"clap.plugin-factory")
if not f:
    die("get_factory(clap.plugin-factory) returned null")
factory = Factory.from_address(f)
count = factory.get_plugin_count(ctypes.c_void_p(f))
d = factory.get_plugin_descriptor(ctypes.c_void_p(f), 0)
if count < 1 or not d:
    die("no plugins advertised by the factory")
desc = Descriptor.from_address(d)
print("  load check OK: %d plugin(s): id '%s', name '%s' v%s by %s" % (
    count, desc.id.decode(), desc.name.decode(),
    desc.version.decode(), desc.vendor.decode() or "unknown"))
entry.deinit()
PY
else
    echo "  (python3 not found — skipped live load check; checks 1-5 still ran)"
fi

echo ""
echo "Installed and verified: $DEST"
echo ""
echo "Next: restart Bitwig Studio and add the device"
echo "  'Polar Reconstruction' (under Audio Effects / CLAP)."
echo "  If it still does not appear: Bitwig Settings -> Plug-ins -> Re-scan All."
echo ""
echo "Uninstall with:"
echo "  rm -rf \"$DEST\""
