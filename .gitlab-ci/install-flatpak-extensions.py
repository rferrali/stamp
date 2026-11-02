#!/usr/bin/env python3
"""Install missing CI SDK extensions without updating the image's SDK."""

import configparser
import json
from pathlib import Path
import subprocess
import sys


manifest = json.loads(Path(sys.argv[1]).read_text())
arch = sys.argv[2]
sdk_parts = manifest["sdk"].split("/")
sdk_branch = sdk_parts[2] if len(sdk_parts) == 3 else manifest["runtime-version"]
sdk_ref = f"runtime/{sdk_parts[0]}/{arch}/{sdk_branch}"

metadata = configparser.ConfigParser(interpolation=None)
metadata.read_string(subprocess.check_output(
    ["flatpak", "info", "--show-metadata", sdk_ref], text=True,
))

for extension in manifest.get("sdk-extensions", []):
    group = f"Extension {extension}"
    if not metadata.has_section(group):
        group = f"Extension {extension.rsplit('.', 1)[0]}"
    if not metadata.has_section(group):
        raise SystemExit(f"SDK {sdk_ref} has no extension point for {extension}")

    branch = metadata.get(group, "version", fallback=sdk_branch)
    ref = f"runtime/{extension}/{arch}/{branch}"
    installed = subprocess.run(
        ["flatpak", "info", "--show-ref", ref],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    if installed.returncode == 0:
        print(f"Using installed SDK extension: {ref}", flush=True)
        continue

    # Only add the requested extension. The CI image supplies the SDK, runtime
    # and related refs, whose deployments may live on container image layers.
    subprocess.run([
        "flatpak", "install", "--user", "--noninteractive", "--assumeyes",
        "--no-related", "--no-deps", "flathub", ref,
    ], check=True)
