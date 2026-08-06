"""Bake WARTHOG_VERSION into the build.

Source priority:
  1. WARTHOG_VERSION env var — release workflows set this from the git tag.
  2. `git describe --tags --always --dirty` — local dev, unique per-commit.
  3. "unknown" — last resort.
"""

import os
import subprocess

Import("env")


def _derive_version():
    explicit = os.environ.get("WARTHOG_VERSION")
    if explicit:
        return explicit
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        return out or "unknown"
    except Exception:
        return "unknown"


VERSION = _derive_version()
print(f"warthog: WARTHOG_VERSION={VERSION}")
env.Append(CPPDEFINES=[("WARTHOG_VERSION", env.StringifyMacro(VERSION))])
