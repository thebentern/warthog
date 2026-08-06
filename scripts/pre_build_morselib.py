"""
Hooks that work around PlatformIO + morsemicro/halow build-ordering bugs:

  * objcopy mm6108.mbin and the BCF into ELF blob objects (the local
    components/firmware/ custom-command doesn't fire under PlatformIO).
  * Run ar -M + ranlib + librarymangler.py to produce libmorse.a (PlatformIO's
    section scanner reads .a files before CMake's custom command produces it).
  * Patch managed_components/.../morselib/CMakeLists.txt to mark libmorse.a as
    a dep of the IMPORTED morselib target so consumers don't need PlatformIO
    discovery.
  * Append libmorse.a + blob objects to firmware.elf's link line (NOT to the
    bootloader's — its smaller linker script chokes on libmorse's sections).
"""

import os
import subprocess
import sys

Import("env")

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

build_dir = env.subst("$BUILD_DIR")
project_dir = env.subst("$PROJECT_DIR")

# The halow component is a local fork at components/halow (override_path in
# main/idf_component.yml). IDF builds it under the component name `halow`, so
# build artifacts land in esp-idf/halow/, not esp-idf/morsemicro__halow/.
morselib_dir = os.path.join(
    build_dir, "esp-idf", "halow", "components", "morselib"
)
ar_script = os.path.join(morselib_dir, "libmorse.ar")
libmorse = os.path.join(morselib_dir, "libmorse.a")
liblibmorse = os.path.join(morselib_dir, "liblibmorse.a")
libmmhostap = os.path.join(
    build_dir, "esp-idf", "halow", "components", "hostap", "libmmhostap.a"
)

firmware_build_dir = os.path.join(build_dir, "esp-idf", "firmware")
fw_blob_dir = os.path.join(
    project_dir, "managed_components", "morsemicro__firmware", "mm6108"
)
# Filenames must track CONFIG_MM_BCF_FILE / CONFIG_MM_FW_FILE in sdkconfig.defaults.
fw_mbin = os.path.join(fw_blob_dir, "mm6108.mbin")
bcf_mbin = os.path.join(fw_blob_dir, "bcf_fgh100mhaamd.mbin")
fw_obj = os.path.join(firmware_build_dir, "mm6108.mbin.o")
bcf_obj = os.path.join(firmware_build_dir, "bcf_fgh100mhaamd.mbin.o")

cc = env.subst("$CC")
toolchain_bin = os.path.dirname(cc)
ar = os.path.join(toolchain_bin, "xtensa-esp32s3-elf-ar")
ranlib = os.path.join(toolchain_bin, "xtensa-esp32s3-elf-ranlib")
objcopy_bin = os.path.join(toolchain_bin, "xtensa-esp32s3-elf-objcopy")
toolchain_prefix = os.path.join(toolchain_bin, "xtensa-esp-elf-")

mm_sdk_tools = os.path.join(
    project_dir, "components", "halow",
    "components", "mm-iot-sdk", "framework", "tools",
)
mangler = os.path.join(mm_sdk_tools, "buildsystem", "librarymangler.py")
protected_syms_path = os.path.join(mm_sdk_tools, "metadata", "protected_syms.txt")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _c_identifier(path):
    return "".join(ch if ch.isalnum() else "_" for ch in path)


def _make_blob_object(src_mbin, dst_obj, prefix):
    if os.path.isfile(dst_obj):
        return
    os.makedirs(os.path.dirname(dst_obj), exist_ok=True)
    ident = _c_identifier(src_mbin)
    cmd = [
        objcopy_bin,
        "-I", "binary", "-O", "elf32-xtensa-le", "-B", "xtensa",
        src_mbin, dst_obj,
        "--redefine-sym", f"_binary_{ident}_start={prefix}_start",
        "--redefine-sym", f"_binary_{ident}_size={prefix}_size",
        "--redefine-sym", f"_binary_{ident}_end={prefix}_end",
        "--rename-section", ".data=.rodata._fw_mbin,contents,alloc,load,readonly,data",
    ]
    print(f"warthog: objcopy {os.path.basename(src_mbin)} -> {prefix}")
    subprocess.check_call(cmd)


def _load_protected_syms():
    if not os.path.isfile(protected_syms_path):
        return []
    out = []
    with open(protected_syms_path, "r") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                out.append(line)
    return out


def _patch_morselib_cmake():
    """Make IMPORTED morselib depend on the custom-command that produces
    libmorse.a, and wrap that .a in --whole-archive when consumers link it."""
    cmake_path = os.path.join(
        project_dir, "components", "halow",
        "components", "morselib", "CMakeLists.txt",
    )
    if not os.path.isfile(cmake_path):
        return
    marker = "# warthog: chain morselib_build + force whole-archive linkage"
    with open(cmake_path, "r") as f:
        content = f.read()
    if marker in content:
        return
    needle = 'set_target_properties(morselib PROPERTIES IMPORTED_LOCATION "${LIBMORSEMANG}")'
    if needle not in content:
        return
    # NOTE: deliberately no `add_dependencies(morselib morselib_build)` — it
    # forms a CMake target cycle (morselib_build -> libmorse -> mmutils/shims/
    # mmpktmem -> morselib). morselib_build's ALL + the firmware-ELF pre-action
    # below already guarantee the mangled archive exists before link.
    injection = "\n".join([
        "",
        marker,
        "set_property(TARGET morselib APPEND PROPERTY INTERFACE_LINK_LIBRARIES",
        '    "-Wl,--whole-archive" "${LIBMORSEMANG}" "-Wl,--no-whole-archive")',
    ])
    with open(cmake_path, "w") as f:
        f.write(content.replace(needle, needle + injection))
    print("warthog: patched morselib/CMakeLists.txt")


# ---------------------------------------------------------------------------
# Pre-actions
# ---------------------------------------------------------------------------

def build_libmorse(target, source, env):
    if os.path.isfile(fw_mbin) and os.path.isfile(bcf_mbin):
        _make_blob_object(fw_mbin, fw_obj, "firmware_binary")
        _make_blob_object(bcf_mbin, bcf_obj, "bcf_binary")

    if not (os.path.isfile(liblibmorse) and os.path.isfile(libmmhostap)
            and os.path.isfile(ar_script)):
        return

    # Skip rebuild if libmorse.a is newer than both inputs and is a valid archive.
    # A half-built libmorse.a (ar -M ran, ranlib didn't) makes ld report
    # "corrupt input" later, so we sniff the archive index with `ar t`.
    if os.path.isfile(libmorse):
        mtime = os.path.getmtime(libmorse)
        if (mtime > os.path.getmtime(liblibmorse)
                and mtime > os.path.getmtime(libmmhostap)):
            try:
                subprocess.check_call([ar, "t", libmorse], stdout=subprocess.DEVNULL)
                return
            except subprocess.CalledProcessError:
                pass

    print(f"warthog: mangling libmorse.a")
    try:
        os.remove(libmorse)
    except FileNotFoundError:
        pass
    with open(ar_script, "rb") as f:
        subprocess.check_call([ar, "-M"], stdin=f, cwd=morselib_dir)
    subprocess.check_call([ranlib, libmorse], cwd=morselib_dir)

    if os.path.isfile(mangler):
        if not os.access(mangler, os.X_OK):
            os.chmod(mangler, 0o755)
        py = sys.executable or env.subst("$PYTHONEXE") or "python3"
        cmd = [py, mangler, "-t", toolchain_prefix, "-m", morselib_dir]
        for sym in _load_protected_syms():
            cmd += ["-p", sym]
        cmd.append(libmorse)
        subprocess.check_call(cmd, cwd=morselib_dir)


def append_firmware_link_flags(target, source, env):
    """Append libmorse.a + blob objects to firmware.elf's link (only)."""
    env.Append(LINKFLAGS=[
        "-Wl,--whole-archive", libmorse, "-Wl,--no-whole-archive",
        fw_obj, bcf_obj,
    ])


# ---------------------------------------------------------------------------
# Wiring
# ---------------------------------------------------------------------------

_patch_morselib_cmake()

# Blob objects need to exist before bootloader.elf links — pre-actions only
# fire for firmware-side targets in this PIO env.
try:
    if os.path.isfile(fw_mbin) and os.path.isfile(bcf_mbin):
        _make_blob_object(fw_mbin, fw_obj, "firmware_binary")
        _make_blob_object(bcf_mbin, bcf_obj, "bcf_binary")
except Exception as e:
    print(f"warthog: script-load objcopy failed (will retry): {e}")

env.AddPreAction("$BUILD_DIR/sections.ld", build_libmorse)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", build_libmorse)
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", append_firmware_link_flags)
