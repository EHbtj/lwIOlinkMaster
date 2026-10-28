"""Record local vendor changes after fetching pinned originals into .local/upstream.

This does not download code or modify the protocol implementation. Keep the
upstream paths listed in third_party/README.md and review its changes manually.
"""
from pathlib import Path
import difflib
import hashlib

root = Path(__file__).resolve().parents[1]
paths = [
    "src/iolinki_master/master.h",
    "src/vendor/iolinki_master/master_internal.h",
    "src/vendor/iolinki_master/master_port.c",
    "src/vendor/iolinki_master/master_isdu.c",
    "src/vendor/iolinki_master/master_parameters.c",
    "third_party/iolinki-master/tests/fake_iolink_device.c",
    "third_party/iolinki-master/tests/fake_iolink_device.h",
]
patch = []
for path in paths:
    original = root / ".local/upstream" / path
    if not original.exists():
        raise SystemExit(f"Pinned original is missing: {original}")
    patch.extend(difflib.unified_diff(original.read_text(encoding="utf-8").splitlines(True),
                                    (root / path).read_text(encoding="utf-8").splitlines(True),
                                    fromfile="a/" + path, tofile="b/" + path))
(root / "third_party/local-changes.patch").write_text("".join(patch), encoding="utf-8", newline="\n")
files = []
for directory in ["src/iolinki", "src/iolinki_master", "src/vendor", "third_party/iolinki-master/tests"]:
    files.extend(p for p in (root / directory).rglob("*") if p.is_file())
lines = []
for path in sorted(files):
    normalized = path.read_text(encoding="utf-8").encode("utf-8")
    lines.append(f"{hashlib.sha256(normalized).hexdigest()}  {path.relative_to(root).as_posix()}\n")
(root / "third_party/SHA256SUMS").write_text("".join(lines), encoding="utf-8", newline="\n")
