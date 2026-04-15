# remarkable-doc-links

Experimental internal links for reMarkable notebooks via XOVI/QMD.

## What it does

- add **Link to document** to the notebook selection / lasso popout
- add **Create linked note** to the same selection flow
- persist source-page link markers in a dedicated `links.rm` sidecar
- tap a marker to open the linked document/page
- keep a transient **back stack** for linked-document navigation
- expose links in a sidebar **Links** view

## Current scope

This release is aimed at the **selection-tool / handwritten lasso** flow first.

What is currently supported well:

- handwritten / lasso source links
- cross-document picking
- create linked note directly from a selection
- back navigation after following links
- sidebar browsing of document links

What is still limited:

- glyph/text-selection links still use the simpler phase-1 cross-document flow
- back stack is session state, not persisted across xochitl restarts
- older links created before the page-space marker fix may render slightly off
- the create-linked-note path may briefly flash a modal on some setups, but should dismiss immediately

## Compatibility

Tested primarily on:

- **reMarkable Paper Pro**
- **reMarkable OS `3.26.0.68`**
- **XOVI** with `qt-resource-rebuilder`

This repo includes a **tablet-side native plugin**. The bundled prebuilt binary is currently for **Paper Pro / aarch64**.

## Important dependency / conflict note

This package includes a **modified fork of `betterToc.qmd`**.

That means:

- you do **not** install upstream betterToc separately for this package
- if you already have another `betterToc.qmd`, this package **replaces it**
- the installer backs up the existing `betterToc.qmd` before replacing it
- if you installed betterToc through another package manager, uninstall or disable that package first so it does not overwrite this fork later

Your existing `toc.rm` data should remain usable; the main conflict is the patch file itself.

## Files installed on the tablet

The installer writes these files:

- `/home/root/xovi/exthome/qt-resource-rebuilder/betterToc.qmd`
- `/home/root/xovi/exthome/qt-resource-rebuilder/linkToDocumentFromSelection.qmd`
- `/home/root/xovi/exthome/qt-resource-rebuilder/linkBackGesture.qmd`
- `/home/root/xovi/extensions.d/remarkable-xovi-native.so`
- `/home/root/xovi/extensions.d/xovi-message-broker.so`

Backups are stored on the tablet under:

- `/home/root/.local/share/remarkable-doc-links/backups/<timestamp>/`

If a tablet already has the older legacy plugin name `desktop-clipboard-native.so`, the installer backs it up and removes that duplicate copy during install.

## Install

From a release bundle or this source tree:

```bash
python3 install.py
```

Useful options:

```bash
python3 install.py --host remarkable
python3 install.py --transport ssh
python3 install.py --skip-broker
```

Installer expectations:

- `python3` on your desktop/laptop
- `ssh` and/or `tailscale ssh`
- working SSH access to `root@remarkable`
- XOVI already installed on the tablet

The installer will:

1. verify that XOVI exists on the tablet
2. back up any existing conflicting files
3. install the bundled QMD patches
4. install the tablet-side native plugin
5. download/install `xovi-message-broker.so` unless `--skip-broker` is used
6. remove any legacy `desktop-clipboard-native.so` copy to avoid duplicate loading
7. restart XOVI via `/home/root/xovi/start`

## Uninstall / restore

To restore the most recent backup:

```bash
python3 uninstall.py
```

To restore a specific backup directory:

```bash
python3 uninstall.py --backup-dir /home/root/.local/share/remarkable-doc-links/backups/<timestamp>
```

## Maintainer release assembly

Build a releasable bundle under `dist/`:

```bash
python3 package_release.py
```

That script copies:

- release docs
- installer / uninstaller
- bundled QMD files
- the current prebuilt native plugin from `./remarkable-xovi-native/dist/remarkable-xovi-native-aarch64.so`

If the native plugin is missing, rebuild it first from `./remarkable-xovi-native/`.

## Repo layout

- [`files/`](./files/) — bundled QMD patches
- [`remarkable-xovi-native/`](./remarkable-xovi-native/) — tablet-side native plugin source

## Licensing

This package should be treated as **GPL-3.0-only** because it bundles a modified derivative of `betterToc.qmd`.

See:

- [`LICENSE`](./LICENSE)
- [`THIRD_PARTY.md`](./THIRD_PARTY.md)
- [`licenses/bettertoc-LICENSE`](./licenses/bettertoc-LICENSE)
