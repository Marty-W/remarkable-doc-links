# Third-party notices

## Modified betterToc

This package bundles a modified fork of **betterToc** by Mitchell Scott.

- Upstream project: <https://github.com/rmitchellscott/xovi-qmd-extensions>
- Upstream file basis: `3.26/betterToc.qmd`
- Upstream commit: `2de6891b9456b6b673428adf677073096f341404`
- Upstream license: `GPL-3.0-only`
- Included license text: [`licenses/bettertoc-LICENSE`](./licenses/bettertoc-LICENSE)

The bundled `files/betterToc.qmd` is a modified derivative and remains GPL-3.0-only.

## Selection-flow basis

`files/linkToDocumentFromSelection.qmd` started from the general shape of `tocFromSelection.qmd` and related selection-menu patches used in the XOVI ecosystem.

Main references used during development:

- `tocFromSelection.qmd`
- `addTrashcanToSelection.qmd`
- `addTagsToContextMenu.qmd`

## XOVI message broker dependency

The installer downloads `xovi-message-broker.so` from the rm-xovi-extensions release stream at install time.

- Upstream project: <https://github.com/asivery/rm-xovi-extensions>
- Artifact currently downloaded by the installer: `xovi-aarch64.tar.gz`

That broker binary is not stored in this source tree; it is fetched during install.

## Native helper naming and legacy signal names

This repo ships the tablet-side native plugin as `remarkable-xovi-native.so`.

For compatibility with earlier development tooling, the exported broker signal names currently still use the older `desktopClipboard...` naming internally.
