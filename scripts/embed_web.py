"""Embed the local web UI in firmware; no separate filesystem upload is needed."""
import gzip
from pathlib import Path

Import("env")

root = Path(env.subst("$PROJECT_DIR"))
out = Path(env.subst("$BUILD_DIR")) / "generated"
out.mkdir(parents=True, exist_ok=True)
parts = ["#pragma once\n#include <Arduino.h>\n"]
for name, filename in (("CONTROL_PAGE", "index.html"), ("SETUP_PAGE", "setup.html")):
    page = (root / "web" / filename).read_text(encoding="utf-8")
    if name == "CONTROL_PAGE":
        page = page.replace("</head>", "<script>window.LAMP_HTTP = true;</script></head>")
    data = gzip.compress(page.encode("utf-8"), mtime=0)
    parts.append(f"static const uint8_t {name}[] PROGMEM = {{\n")
    for i in range(0, len(data), 24):
        parts.append(",".join(str(b) for b in data[i:i + 24]) + ",\n")
    parts.append("};\n")
header = out / "web_pages.h"
content = "".join(parts)
if not header.exists() or header.read_text() != content:
    header.write_text(content)
env.Append(CPPPATH=[str(out)])
