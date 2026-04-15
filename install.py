#!/usr/bin/env python3
from __future__ import annotations

import argparse
import io
import json
import shlex
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path
from typing import Iterable

BROKER_RELEASE_URL = "https://github.com/asivery/rm-xovi-extensions/releases/download/v18-23032026/xovi-aarch64.tar.gz"
BROKER_TAR_MEMBER = "xovi/inactive-extensions/xovi-message-broker.so"
DEFAULT_HOST = "remarkable"
DEFAULT_USER = "root"
DEFAULT_TIMEOUT = 30
BACKUP_ROOT = "/home/root/.local/share/remarkable-doc-links/backups"
REMOTE_QMD_DIR = "/home/root/xovi/exthome/qt-resource-rebuilder"
REMOTE_EXT_DIR = "/home/root/xovi/extensions.d"
REMOTE_START_CMD = "/home/root/xovi/start"


class InstallError(RuntimeError):
    pass


FILES = {
    "betterToc.qmd": f"{REMOTE_QMD_DIR}/betterToc.qmd",
    "linkToDocumentFromSelection.qmd": f"{REMOTE_QMD_DIR}/linkToDocumentFromSelection.qmd",
    "linkBackGesture.qmd": f"{REMOTE_QMD_DIR}/linkBackGesture.qmd",
    "remarkable-xovi-native.so": f"{REMOTE_EXT_DIR}/remarkable-xovi-native.so",
    "xovi-message-broker.so": f"{REMOTE_EXT_DIR}/xovi-message-broker.so",
}

LEGACY_FILES = {
    "desktop-clipboard-native.so": f"{REMOTE_EXT_DIR}/desktop-clipboard-native.so",
}


def script_dir() -> Path:
    return Path(__file__).resolve().parent


def version() -> str:
    version_path = script_dir() / "VERSION"
    return version_path.read_text().strip() if version_path.exists() else "0.0.0"


def release_payload_path(name: str) -> Path:
    return script_dir() / "files" / name


def source_payload_path(name: str) -> Path:
    root = script_dir()
    if name in {"betterToc.qmd", "linkToDocumentFromSelection.qmd", "linkBackGesture.qmd"}:
        return root / "files" / name
    if name == "remarkable-xovi-native.so":
        return root / "remarkable-xovi-native" / "dist" / "remarkable-xovi-native-aarch64.so"
    raise InstallError(f"Unknown payload: {name}")


def resolve_payload_path(name: str) -> Path:
    for candidate in (release_payload_path(name), source_payload_path(name)):
        if candidate.exists():
            return candidate
    raise InstallError(f"Missing payload file: {name}")


def remote_target(host: str, user: str) -> str:
    return f"{user}@{host}"


def remote_shell(command: str) -> str:
    return f"sh -lc {shlex.quote(command)}"


def transport_commands(host: str, user: str, transport: str, remote_command: str) -> list[tuple[str, list[str]]]:
    target = remote_target(host, user)
    if transport == "ssh":
        return [("ssh", ["ssh", target, remote_command])]
    if transport == "tailscale-ssh":
        return [("tailscale-ssh", ["tailscale", "ssh", target, remote_command])]
    if transport != "auto":
        raise InstallError(f"Unsupported transport: {transport}")
    return [
        ("tailscale-ssh", ["tailscale", "ssh", target, remote_command]),
        ("ssh", ["ssh", target, remote_command]),
    ]


def completed_detail(exc: subprocess.CalledProcessError) -> str:
    stderr = exc.stderr or ""
    stdout = exc.stdout or ""
    if isinstance(stderr, bytes):
        stderr = stderr.decode(errors="replace")
    if isinstance(stdout, bytes):
        stdout = stdout.decode(errors="replace")
    return (stderr or stdout or f"exit {exc.returncode}").strip()


def run_remote(host: str, user: str, transport: str, command: str, *, timeout: int = DEFAULT_TIMEOUT) -> tuple[str, str, str]:
    errors: list[str] = []
    for transport_name, argv in transport_commands(host, user, transport, remote_shell(command)):
        try:
            completed = subprocess.run(
                argv,
                check=True,
                text=True,
                capture_output=True,
                timeout=timeout,
            )
            return transport_name, completed.stdout, completed.stderr
        except FileNotFoundError:
            errors.append(f"{transport_name}: command not found")
        except subprocess.CalledProcessError as exc:
            errors.append(f"{transport_name}: {completed_detail(exc)}")
        except subprocess.TimeoutExpired:
            errors.append(f"{transport_name}: timed out after {timeout}s")
    raise InstallError("Remote command failed: " + " | ".join(errors))


def write_remote_bytes(host: str, user: str, transport: str, remote_path: str, content: bytes, *, timeout: int = DEFAULT_TIMEOUT) -> str:
    command = (
        "umask 077; "
        f"mkdir -p {shlex.quote(str(Path(remote_path).parent))}; "
        f"tmp=$(mktemp {shlex.quote(remote_path)}.XXXXXX); "
        'cat >"$tmp"; '
        f'mv "$tmp" {shlex.quote(remote_path)}'
    )
    errors: list[str] = []
    for transport_name, argv in transport_commands(host, user, transport, remote_shell(command)):
        try:
            subprocess.run(
                argv,
                input=content,
                check=True,
                capture_output=True,
                timeout=timeout,
            )
            return transport_name
        except FileNotFoundError:
            errors.append(f"{transport_name}: command not found")
        except subprocess.CalledProcessError as exc:
            errors.append(f"{transport_name}: {completed_detail(exc)}")
        except subprocess.TimeoutExpired:
            errors.append(f"{transport_name}: timed out after {timeout}s")
    raise InstallError("Remote write failed: " + " | ".join(errors))


def write_remote_text(host: str, user: str, transport: str, remote_path: str, content: str, *, timeout: int = DEFAULT_TIMEOUT) -> str:
    return write_remote_bytes(host, user, transport, remote_path, content.encode("utf-8"), timeout=timeout)


def remote_exists(host: str, user: str, transport: str, remote_path: str) -> bool:
    _, stdout, _ = run_remote(
        host,
        user,
        transport,
        f"if [ -e {shlex.quote(remote_path)} ]; then printf yes; else printf no; fi",
    )
    return stdout.strip() == "yes"


def ensure_xovi(host: str, user: str, transport: str) -> None:
    run_remote(
        host,
        user,
        transport,
        f"test -x {shlex.quote(REMOTE_START_CMD)} && test -d {shlex.quote(REMOTE_QMD_DIR)}",
    )


def restart_xovi(host: str, user: str, transport: str) -> str:
    transport_name, _, _ = run_remote(host, user, transport, REMOTE_START_CMD, timeout=90)
    return transport_name


def download_broker_binary() -> bytes:
    with urllib.request.urlopen(BROKER_RELEASE_URL, timeout=60) as response:
        archive_bytes = response.read()
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:gz") as tar:
        member = tar.extractfile(BROKER_TAR_MEMBER)
        if member is None:
            raise InstallError(f"Missing {BROKER_TAR_MEMBER} in broker archive")
        return member.read()


def remote_backup_dir(timestamp: str) -> str:
    return f"{BACKUP_ROOT}/{timestamp}"


def backup_files(host: str, user: str, transport: str, *, skip_broker: bool) -> tuple[str, dict[str, object]]:
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    backup_dir = remote_backup_dir(timestamp)
    run_remote(host, user, transport, f"mkdir -p {shlex.quote(backup_dir)}")

    manifest: dict[str, object] = {
        "package": "remarkable-doc-links",
        "version": version(),
        "created_at": timestamp,
        "files": [],
    }

    for local_name, remote_path in FILES.items():
        if skip_broker and local_name == "xovi-message-broker.so":
            continue
        existed = remote_exists(host, user, transport, remote_path)
        backup_name = Path(remote_path).name
        if existed:
            run_remote(
                host,
                user,
                transport,
                f"cp {shlex.quote(remote_path)} {shlex.quote(backup_dir + '/' + backup_name)}",
            )
        manifest["files"].append(
            {
                "local_name": local_name,
                "remote_path": remote_path,
                "backup_name": backup_name,
                "existed": existed,
            }
        )

    for local_name, remote_path in LEGACY_FILES.items():
        existed = remote_exists(host, user, transport, remote_path)
        backup_name = Path(remote_path).name
        if existed:
            run_remote(
                host,
                user,
                transport,
                f"cp {shlex.quote(remote_path)} {shlex.quote(backup_dir + '/' + backup_name)}",
            )
        manifest["files"].append(
            {
                "local_name": local_name,
                "remote_path": remote_path,
                "backup_name": backup_name,
                "existed": existed,
                "legacy": True,
            }
        )

    write_remote_text(
        host,
        user,
        transport,
        f"{backup_dir}/manifest.json",
        json.dumps(manifest, indent=2) + "\n",
    )
    return backup_dir, manifest


def install_files(host: str, user: str, transport: str, *, skip_broker: bool) -> dict[str, str]:
    transports: dict[str, str] = {}
    for local_name, remote_path in FILES.items():
        if skip_broker and local_name == "xovi-message-broker.so":
            continue
        if local_name == "xovi-message-broker.so":
            payload = download_broker_binary()
        else:
            payload = resolve_payload_path(local_name).read_bytes()
        transports[local_name] = write_remote_bytes(host, user, transport, remote_path, payload, timeout=90)
    return transports


def cleanup_legacy_files(host: str, user: str, transport: str) -> None:
    for remote_path in LEGACY_FILES.values():
        run_remote(host, user, transport, f"rm -f {shlex.quote(remote_path)}")


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Install remarkable-doc-links onto a reMarkable tablet")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"tablet host (default: {DEFAULT_HOST})")
    parser.add_argument("--user", default=DEFAULT_USER, help=f"tablet user (default: {DEFAULT_USER})")
    parser.add_argument(
        "--transport",
        choices=["auto", "ssh", "tailscale-ssh"],
        default="auto",
        help="transport preference (default: auto)",
    )
    parser.add_argument("--skip-broker", action="store_true", help="do not install xovi-message-broker.so")
    return parser.parse_args(list(argv) if argv is not None else None)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)

    ensure_xovi(args.host, args.user, args.transport)
    backup_dir, manifest = backup_files(args.host, args.user, args.transport, skip_broker=args.skip_broker)
    transports = install_files(args.host, args.user, args.transport, skip_broker=args.skip_broker)
    cleanup_legacy_files(args.host, args.user, args.transport)
    restart_transport = restart_xovi(args.host, args.user, args.transport)

    print(f"Installed remarkable-doc-links {version()} to {args.user}@{args.host}")
    print(f"Backup saved on tablet: {backup_dir}")
    print(f"Restarted XOVI via: {restart_transport}")
    for entry in manifest["files"]:
        local_name = entry["local_name"]
        if args.skip_broker and local_name == "xovi-message-broker.so":
            continue
        if local_name in transports:
            print(f"- {local_name}: {transports[local_name]} -> {entry['remote_path']}")
        elif entry.get("legacy"):
            print(f"- removed legacy plugin copy: {entry['remote_path']}")

    print("\nNote: this bundle replaces any existing betterToc.qmd with the bundled modified fork.")
    print("If you also installed upstream betterToc elsewhere, disable or uninstall that package to avoid future overwrite.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except InstallError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
