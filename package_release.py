#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import tarfile
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parent
FILES_DIR = ROOT / "files"
DIST_DIR = ROOT / "dist"
NATIVE_HELPER = ROOT / "remarkable-xovi-native" / "dist" / "remarkable-xovi-native-aarch64.so"
DOCS_TO_COPY = ["README.md", "THIRD_PARTY.md", "install.py", "uninstall.py", "VERSION", "LICENSE"]


def version() -> str:
    return (ROOT / "VERSION").read_text().strip()


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a remarkable-doc-links release bundle")
    parser.add_argument("--version", help="override version string")
    parser.add_argument("--no-archives", action="store_true", help="only build the directory tree")
    return parser.parse_args(list(argv) if argv is not None else None)


def ensure_inputs() -> None:
    required_files = [
        FILES_DIR / "betterToc.qmd",
        FILES_DIR / "linkToDocumentFromSelection.qmd",
        FILES_DIR / "linkBackGesture.qmd",
        ROOT / "README.md",
        ROOT / "install.py",
        ROOT / "uninstall.py",
        ROOT / "LICENSE",
        ROOT / "THIRD_PARTY.md",
    ]
    missing = [str(path) for path in required_files if not path.exists()]
    if missing:
        raise SystemExit("Missing required inputs:\n- " + "\n- ".join(missing))
    if not NATIVE_HELPER.exists():
        raise SystemExit(
            "Missing native helper build artifact:\n"
            f"- {NATIVE_HELPER}\n\n"
            "Build it first from ./remarkable-xovi-native/."
        )


def build_release(version_string: str) -> Path:
    release_name = f"remarkable-doc-links-{version_string}"
    release_root = DIST_DIR / release_name
    if release_root.exists():
        shutil.rmtree(release_root)

    (release_root / "files").mkdir(parents=True)
    (release_root / "licenses").mkdir(parents=True)

    for name in DOCS_TO_COPY:
        src = ROOT / name
        if src.exists():
            shutil.copy2(src, release_root / name)

    shutil.copy2(ROOT / "licenses" / "bettertoc-LICENSE", release_root / "licenses" / "bettertoc-LICENSE")

    for name in ("betterToc.qmd", "linkToDocumentFromSelection.qmd", "linkBackGesture.qmd"):
        shutil.copy2(FILES_DIR / name, release_root / "files" / name)

    shutil.copy2(NATIVE_HELPER, release_root / "files" / "remarkable-xovi-native.so")
    return release_root


def build_archives(release_root: Path) -> tuple[Path, Path]:
    zip_path = Path(shutil.make_archive(str(release_root), "zip", root_dir=release_root.parent, base_dir=release_root.name))
    tar_path = Path(str(release_root) + ".tar.gz")
    with tarfile.open(tar_path, "w:gz") as tar:
        tar.add(release_root, arcname=release_root.name)
    return zip_path, tar_path


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    ensure_inputs()
    version_string = args.version or version()
    release_root = build_release(version_string)
    print(f"Built release tree: {release_root}")

    if not args.no_archives:
        zip_path, tar_path = build_archives(release_root)
        print(f"Built zip: {zip_path}")
        print(f"Built tar.gz: {tar_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
