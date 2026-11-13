"""Download a fixed ModelScope revision directly, with resumable checked files."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", help="ModelScope owner/model")
    parser.add_argument("revision", help="40-character commit SHA")
    parser.add_argument("manifest", type=Path, help="ModelScope recursive file listing for this commit")
    parser.add_argument("destination", type=Path)
    parser.add_argument("files", nargs="*", help="Optional exact file paths")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo) or not re.fullmatch(
        r"[0-9a-f]{40}", args.revision
    ):
        parser.error("repo must be owner/model and revision must be a commit SHA")

    root = args.destination.resolve()
    entries = json.loads(args.manifest.read_text())["Data"]["Files"]
    selected = [
        entry for entry in entries if entry["Type"] == "blob" and (not args.files or entry["Path"] in args.files)
    ]
    if not selected or (args.files and {entry["Path"] for entry in selected} != set(args.files)):
        raise ValueError("requested files are not present in the manifest")
    environment = {
        key: value for key, value in os.environ.items() if key.lower() not in {"http_proxy", "https_proxy", "all_proxy"}
    }
    for entry in selected:
        target = (root / entry["Path"]).resolve()
        if root not in target.parents or not re.fullmatch(r"[0-9a-f]{64}", entry.get("Sha256", "")):
            raise ValueError(f"invalid manifest entry: {entry['Path']}")
        target.parent.mkdir(parents=True, exist_ok=True)
        part = target.with_name(target.name + ".part")
        if part.is_symlink():
            raise ValueError(f"partial file is a symlink: {part}")
        part_size = part.stat().st_size if part.exists() else -1
        if not target.exists() and part_size > entry["Size"]:
            raise ValueError(f"partial file exceeds manifest size: {part}")
        if not target.exists() and part_size != entry["Size"]:
            url = f"https://www.modelscope.cn/api/v1/models/{args.repo}/repo?Revision={args.revision}&FilePath={quote(entry['Path'], safe='/')}"
            print(f"downloading {entry['Path']} ({entry['Size']} bytes)", flush=True)
            subprocess.run(
                [
                    "curl",
                    "-q",
                    "--noproxy",
                    "*",
                    "--fail",
                    "--location",
                    "--silent",
                    "--show-error",
                    "--retry",
                    "2",
                    "--connect-timeout",
                    "30",
                    "--speed-limit",
                    "1024",
                    "--speed-time",
                    "180",
                    "--max-filesize",
                    str(entry["Size"]),
                    "--continue-at",
                    "-",
                    "--output",
                    str(part),
                    url,
                ],
                check=True,
                env=environment,
            )
        candidate = target if target.exists() else part
        digest = hashlib.sha256()
        with candidate.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(block)
        if candidate.stat().st_size != entry["Size"] or digest.hexdigest() != entry["Sha256"]:
            raise ValueError(f"size or checksum mismatch: {candidate}")
        if candidate == part:
            part.replace(target)
        print(f"verified {entry['Path']}", flush=True)


if __name__ == "__main__":
    main()
