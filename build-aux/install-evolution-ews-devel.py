#!/usr/bin/env python3

import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=Path("."))
parser.add_argument("--prefix", type=Path, default=Path("/app"))
args = parser.parse_args()
version = re.search(
    r"project\(evolution-ews\s+VERSION\s+([\d.]+)",
    (args.source / "CMakeLists.txt").read_text(),
).group(1)
pkgconfig = args.prefix / "lib/pkgconfig"
pkgconfig.mkdir(parents=True, exist_ok=True)

for directory, name in (
    ("EWS/common", "evolution-ews"),
    ("Microsoft365/common", "evolution-microsoft365"),
    ("common", "evolution-ews-common"),
):
    headers = args.prefix / "include/evolution-ews" / name
    headers.mkdir(parents=True, exist_ok=True)
    for header in (args.source / "src" / directory).glob("*.h"):
        text = header.read_text()
        # EWS headers refer to siblings using the source tree's common/ path.
        text = re.sub(r'(#include\s+[<"])common/', r'\1evolution-ews/', text)
        (headers / header.name).write_text(text)

    if name == "evolution-ews-common":
        continue
    requires = (
        "camel-1.2 libebackend-1.2 libecal-2.0 libedata-cal-2.0 "
        "libedataserver-1.2 libsoup-3.0 json-glib-1.0 libxml-2.0"
    )
    (pkgconfig / f"{name}.pc").write_text(
        f"prefix={args.prefix}\n"
        "libdir=${prefix}/lib\n"
        "includedir=${prefix}/include/evolution-ews\n"
        f"\nName: {name}\n"
        f"Description: Bundled {name} API for Stamp\n"
        f"Version: {version}\n"
        f"Requires: {requires}\n"
        f"Libs: -L${{libdir}}/evolution-ews -l{name} -levolution-ews-common "
        "-Wl,-rpath,${libdir}/evolution-ews\n"
        "Cflags: -I${includedir}\n"
    )
