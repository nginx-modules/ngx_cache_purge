"""Export the selected Ubuntu archive mirror, including hosted mirror lists."""

import os
from pathlib import Path
import re
from urllib.parse import urlsplit


def source_uris(apt_root):
    for name in ("sources.list.d/ubuntu.sources", "sources.list"):
        path = apt_root / name
        if not path.is_file():
            continue
        for line in path.read_text().splitlines():
            match = re.match(r"^(?:URIs:\s+|deb\s+(?:\[[^\]]+\]\s+)?)(\S+)",
                             line.strip())
            if not match:
                continue
            uri = match[1]
            if uri.startswith("mirror+file:/etc/apt/"):
                # GitHub's Ubuntu image uses /etc/apt/apt-mirrors.txt. Follow
                # the configured list; an unreferenced list may be stale.
                relative = Path(uri[len("mirror+file:/etc/apt/"):])
                if ".." in relative.parts or relative.is_absolute():
                    continue
                mirror_list = apt_root / relative
                if mirror_list.is_file():
                    for entry in mirror_list.read_text().splitlines():
                        fields = entry.split()
                        if fields and not fields[0].startswith("#"):
                            yield fields[0]
            else:
                yield uri


def select_mirror(apt_root):
    for uri in source_uris(apt_root):
        mirror = uri.rstrip("/") + "/"
        if not re.fullmatch(r"https?://[A-Za-z0-9.:-]+/[A-Za-z0-9._~/%+-]*", mirror):
            continue
        if urlsplit(mirror).hostname in ("security.ubuntu.com", "azure.archive.ubuntu.com"):
            continue
        return mirror
    return ""


def action_outputs(mirror):
    # docker/build-push-action rejects an empty value such as "apt_mirror=".
    secret = "apt_mirror=" + mirror if mirror else ""
    return f"mirror={mirror}\nbuildkit_secret={secret}\n"


if __name__ == "__main__":
    selected = select_mirror(Path("/etc/apt"))
    with open(os.environ["GITHUB_OUTPUT"], "a") as output:
        output.write(action_outputs(selected))
    print("Container APT mirror: " + (selected or "distribution default"))
