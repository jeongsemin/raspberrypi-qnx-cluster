"""Packs the project folder (qnx-everywhere-projects) into payload.zip for ClusterConsole.exe, which installs it
into WSL when the environment check finds it missing or out of date.

    python make_payload.py <output zip>

Build outputs, caches, recordings, reports and the screenshots of the tutorial are left out. Text files get LF line
endings, so the scripts run in WSL as they are (a Windows checkout may have CRLF).

The payload also holds qnx_cluster/.payload_version, a hash of everything in it. The environment check compares the
installed copy with the one in the exe, so a newer exe offers to update the projects.
"""
import hashlib
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))       # .../qnx-everywhere-projects
TOP = "qnx-everywhere-projects"
SKIP_DIRS = {"__pycache__", "x86_64", "aarch64le", "dist", "recordings", "reports", "screenshots", ".git", ".claude"}
SKIP_NAMES = {"CLAUDE.md", "AGENTS.md"}   # notes for the coding assistant of this PC, of no use to other people
SKIP_PREFIX = ("build",)                  # qt_cluster/build, qt_cluster/build-aarch64le
SKIP_FILES = (".nvm", ".pyc", ".exe", ".zip")
TEXT = (".sh", ".py", ".c", ".h", ".cpp", ".qml", ".txt", ".json", ".html", ".md", ".code-workspace", ".ps1", ".cmake")
TEXT_NAMES = ("CMakeLists.txt",)
VERSION_FILE = TOP + "/qnx_cluster/.payload_version"


def wanted_dir(rel, name):
    if name in SKIP_DIRS:
        return False
    # build folders of qt_cluster only (not a file or folder that merely starts with "build" elsewhere)
    return not (rel.endswith("qt_cluster") and name.startswith(SKIP_PREFIX))


def main(out):
    n = 0
    digest = hashlib.sha256()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for d, dirs, files in os.walk(ROOT):
            rel = os.path.relpath(d, ROOT).replace("\\", "/")
            dirs[:] = sorted(x for x in dirs if wanted_dir(rel, x))
            for f in sorted(files):
                if f.endswith(SKIP_FILES) or f in SKIP_NAMES:
                    continue
                path = os.path.join(d, f)
                arc = "/".join(p for p in (TOP, "" if rel == "." else rel, f) if p)
                with open(path, "rb") as fh:
                    data = fh.read()
                if f.endswith(TEXT) or f in TEXT_NAMES:
                    data = data.replace(b"\r\n", b"\n")
                z.writestr(arc, data)
                digest.update(arc.encode() + b"\0" + data + b"\0")
                n += 1
        version = digest.hexdigest()[:16]
        z.writestr(VERSION_FILE, version + "\n")
    print("payload: %d files, version %s -> %s" % (n, version, out))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "payload.zip"))
