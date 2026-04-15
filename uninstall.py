#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Iterable

DEFAULT_HOST = "remarkable"
DEFAULT_USER = "root"
DEFAULT_TIMEOUT = 30
BACKUP_ROOT = "/home/root/.local/share/remarkable-doc-links/backups"
REMOTE_START_CMD = "/home/root/xovi/start"


class UninstallError(RuntimeError):
    pass


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
        raise UninstallError(f"Unsupported transport: {transport}")
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
    raise UninstallError("Remote command failed: " + " | ".join(errors))


def remote_exists(host: str, user: str, transport: str, remote_path: str) -> bool:
    _, stdout, _ = run_remote(
        host,
        user,
        transport,
        f"if [ -e {shlex.quote(remote_path)} ]; then printf yes; else printf no; fi",
    )
    return stdout.strip() == "yes"


def restart_xovi(host: str, user: str, transport: str) -> str:
    transport_name, _, _ = run_remote(host, user, transport, REMOTE_START_CMD, timeout=90)
    return transport_name


def latest_backup_dir(host: str, user: str, transport: str) -> str:
    _, stdout, _ = run_remote(
        host,
        user,
        transport,
        f"if [ -d {shlex.quote(BACKUP_ROOT)} ]; then ls -1dt {shlex.quote(BACKUP_ROOT)}/* 2>/dev/null | head -n 1; fi",
    )
    backup_dir = stdout.strip()
    if not backup_dir:
        raise UninstallError(f"No backups found under {BACKUP_ROOT}")
    return backup_dir


def load_manifest(host: str, user: str, transport: str, backup_dir: str) -> dict[str, object]:
    _, stdout, _ = run_remote(host, user, transport, f"cat {shlex.quote(backup_dir + '/manifest.json')}")
    try:
        return json.loads(stdout)
    except json.JSONDecodeError as exc:
        raise UninstallError(f"Could not parse manifest from {backup_dir}") from exc


def restore_from_manifest(host: str, user: str, transport: str, backup_dir: str, manifest: dict[str, object]) -> None:
    files = manifest.get("files")
    if not isinstance(files, list):
        raise UninstallError("Backup manifest is missing a valid 'files' list")

    for entry in files:
        if not isinstance(entry, dict):
            continue
        remote_path = entry.get("remote_path")
        backup_name = entry.get("backup_name")
        existed = bool(entry.get("existed"))
        if not remote_path or not backup_name:
            continue

        backup_path = f"{backup_dir}/{backup_name}"
        if existed and remote_exists(host, user, transport, backup_path):
            run_remote(
                host,
                user,
                transport,
                (
                    f"mkdir -p {shlex.quote(str(Path(str(remote_path)).parent))} && "
                    f"cp {shlex.quote(backup_path)} {shlex.quote(str(remote_path))}"
                ),
            )
        else:
            run_remote(host, user, transport, f"rm -f {shlex.quote(str(remote_path))}")


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Restore a remarkable-doc-links backup")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"tablet host (default: {DEFAULT_HOST})")
    parser.add_argument("--user", default=DEFAULT_USER, help=f"tablet user (default: {DEFAULT_USER})")
    parser.add_argument(
        "--transport",
        choices=["auto", "ssh", "tailscale-ssh"],
        default="auto",
        help="transport preference (default: auto)",
    )
    parser.add_argument("--backup-dir", help="explicit remote backup directory to restore")
    return parser.parse_args(list(argv) if argv is not None else None)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    backup_dir = args.backup_dir or latest_backup_dir(args.host, args.user, args.transport)
    manifest = load_manifest(args.host, args.user, args.transport, backup_dir)
    restore_from_manifest(args.host, args.user, args.transport, backup_dir, manifest)
    restart_transport = restart_xovi(args.host, args.user, args.transport)

    print(f"Restored remarkable-doc-links backup from: {backup_dir}")
    print(f"Restarted XOVI via: {restart_transport}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except UninstallError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
