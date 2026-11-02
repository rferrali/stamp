#!/usr/bin/env python3
"""Add x86_64 CI checks to a copy of the normal Flatpak manifest."""

import json
from pathlib import Path
import sys


manifest = json.loads(Path(sys.argv[1]).read_text())
module = next(module for module in manifest["modules"] if module["name"] == "stamp")

manifest.setdefault("sdk-extensions", []).append("org.freedesktop.Sdk.Extension.llvm22")
options = module.setdefault("build-options", {})
options["prepend-path"] = "/usr/lib/sdk/llvm22/bin"
options.setdefault("test-args", []).extend(["--share=ipc", "--socket=x11"])

# The other gcovr dependencies (Jinja2, lxml, Pygments) are supplied by the SDK.
# Extract pure Python wheels into the source tree; no tools enter the app bundle.
for url, checksum in [
    (
        "https://files.pythonhosted.org/packages/ae/50/b049fc9af7d3b0e4bd3af90ebba0e2f4ec42857674c8ba5ed15b5d719170/gcovr-8.4-py3-none-any.whl",
        "1016d013d6c55225b1f716a4325e16ad5d626ff05b20b8a12d542e2d9a82ac21",
    ),
    (
        "https://files.pythonhosted.org/packages/e3/51/9b208e85196941db2f0654ad0357ca6388ab3ed67efdbfc799f35d1f83aa/colorlog-6.9.0-py3-none-any.whl",
        "5906e71acd67cb07a71e779c47c4bcb45fb8c2993eebe9e5adcd6a6f1b283eff",
    ),
]:
    module["sources"].append({
        "type": "archive",
        "archive-type": "zip",
        "url": url,
        "sha256": checksum,
        "strip-components": 0,
        "dest": ".ci-tools",
    })

# test-commands run inside the SDK before cleanup removes development files.
module.setdefault("test-commands", []).append("bash ../.gitlab-ci/flatpak-checks.sh")
Path(sys.argv[2]).write_text(json.dumps(manifest, indent=2) + "\n")
