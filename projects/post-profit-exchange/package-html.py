#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed the compiled runtime, UI, notices and corresponding source in one HTML."""
import base64
import hashlib
import html
import io
import json
import pathlib
import re
import sys
import zipfile

project = pathlib.Path(__file__).resolve().parent
root = project.parent.parent
empty_history = project / "records/empty-history.json"
if json.loads(empty_history.read_text(encoding="utf-8")) != {"schema_version": "exchange.history.v1", "observations": []}:
    raise SystemExit("The packaged cold-start history must remain empty; use a separate file for observations")
runtime_path = pathlib.Path(sys.argv[1]).resolve()
sdk = pathlib.Path(sys.argv[2]).resolve()
runtime = runtime_path.read_text(encoding="utf-8")
embedded = re.search(r'findWasmBinary\(\)\{return base64Decode\("([A-Za-z0-9+/=]+)"\)', runtime)
if not embedded or not base64.b64decode(embedded.group(1), validate=True).startswith(b'\x00asm\x01\x00\x00\x00'):
    raise SystemExit("Expected an embedded SINGLE_FILE WebAssembly 1 module")

notices = [
    ("Repository and independent exchange application (MIT)", root / "LICENSE"),
    ("Simple exponential smoothing (EWMA, MIT)", root / "tools/exponential-smoothing/LICENSE"),
    ("Pricing core and bounded solver (Worker Protection License)", root / "tools/price-optimization/LICENSE"),
    ("nlohmann/json (MIT)", root / "tools/price-optimization/third_party/nlohmann/LICENSE.MIT"),
    ("Emscripten runtime", sdk / "upstream/emscripten/LICENSE"),
    ("LLVM libc++", sdk / "upstream/emscripten/system/lib/libcxx/LICENSE.TXT"),
    ("LLVM libc++abi", sdk / "upstream/emscripten/system/lib/libcxxabi/LICENSE.TXT"),
    ("musl C library", sdk / "upstream/emscripten/system/lib/libc/musl/COPYRIGHT"),
    ("compiler-rt", sdk / "upstream/emscripten/system/lib/compiler-rt/LICENSE.TXT"),
    ("LLVM libc", sdk / "upstream/emscripten/system/lib/llvm-libc/LICENSE.TXT"),
    ("LLVM libunwind", sdk / "upstream/emscripten/system/lib/libunwind/LICENSE.TXT"),
]
notice_text = [(title, path.read_text(encoding="utf-8")) for title, path in notices]
archive = io.BytesIO()
with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
    files = [root / "LICENSE", root / "docs/licensing.md", root / "docs/third-party-notices.md",
             root / "docs/exchange-simulation-verification.md", root / "docs/post-profit-economics.md",
             root / "docs/adversarial-cooperation.md", root / "docs/reorganization.md",
             root / "docs/research-plan.md", root / "projects/post-profit-assurance/README.md",
             root / "projects/post-profit-assurance/CONTRACT.md"]
    files += [p for p in project.rglob("*") if p.is_file() and not {"dist", "runs", "__pycache__"}.intersection(p.relative_to(project).parts)
              and ("records" not in p.relative_to(project).parts or p in {empty_history, project / "records/sample-history.json", project / "records/README.md"})
              and ("configs" not in p.relative_to(project).parts or p == project / "configs/exchange.cfg")]
    files += [p for p in (root / "tools/price-optimization").rglob("*") if p.is_file()]
    files += [p for p in (root / "tools/exponential-smoothing").rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    for path in sorted(set(files)):
        info = zipfile.ZipInfo(path.relative_to(root).as_posix(), (2026, 9, 20, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        bundle.writestr(info, path.read_bytes())
    for i, (title, content) in enumerate(notice_text):
        info = zipfile.ZipInfo(f"runtime-notices/{i}.txt", (2026, 9, 20, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        bundle.writestr(info, title + "\n\n" + content)

source_data = base64.b64encode(archive.getvalue()).decode("ascii")
licenses = ('<p>The application source is MIT. This combined file also contains the separately '
            'licensed pricing engine and third-party runtime code. Their terms remain applicable. '
            'It contains no AMPL binaries or vendor entitlement.</p>'
            '<p><a download="post-profit-exchange-source.zip" href="data:application/zip;base64,'
            + source_data + '">Download corresponding source and build scripts (ZIP)</a></p>')
licenses += '<p>Emscripten 4.0.15; compiled runtime SHA-256: <code>' + hashlib.sha256(runtime.encode()).hexdigest() + '</code></p>'
for title, content in notice_text:
    licenses += '<details><summary>' + html.escape(title) + '</summary><pre>' + html.escape(content) + '</pre></details>'

page = (project / "web/index.template.html").read_text(encoding="utf-8")
parts = {
    "__STYLE__": (project / "web/style.css").read_text(encoding="utf-8"),
    "__RUNTIME__": runtime.replace("</script", "<\\/script"),
    "__APP__": (project / "web/app.js").read_text(encoding="utf-8").replace("</script", "<\\/script"),
    "__LICENSES__": licenses,
}
for marker, content in parts.items():
    if page.count(marker) != 1:
        raise SystemExit(f"Expected one template marker {marker}")
    page = page.replace(marker, content)
destination = project / "dist/post-profit-exchange.html"
destination.parent.mkdir(exist_ok=True)
destination.write_text(page, encoding="utf-8", newline="\n")
print(f"Standalone HTML: {destination} ({destination.stat().st_size:,} bytes)")
