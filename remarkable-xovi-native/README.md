# remarkable-xovi-native

Tablet-side native XOVI plugin used by `remarkable-doc-links`.

This is **not** a desktop binary. It is a shared library that gets installed onto the reMarkable itself under `xovi/extensions.d/`.

## Current purpose

Right now this plugin is mainly used to:

- inspect live xochitl/QML object trees from native code during development
- expose helper functions over `xovi-message-broker`
- create a new linked notebook directly for the **Create linked note** flow

## Compatibility

The bundled build artifact is currently:

- `dist/remarkable-xovi-native-aarch64.so`

That means the prebuilt binary in this repo is intended for **Paper Pro / aarch64**.

If you want to support another reMarkable model or architecture, rebuild it yourself.

## Broker signal names

For compatibility, the exported broker signal names still use the older `desktopClipboard...` naming for now.

Notable signals currently used or useful during development:

- `desktopClipboardPing`
- `desktopClipboardDumpUi`
- `desktopClipboardInspectObject`
- `desktopClipboardCreateLinkedNotebook`

## Build

```bash
./build.sh
```

Build output:

- `dist/remarkable-xovi-native-aarch64.so`

The build uses `eeems/remarkable-toolchain:latest-rmpp` and `xovigen` from the upstream `asivery/xovi` repo.
