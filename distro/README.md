# Yocto distro

This directory contains a Yocto-based distro for `cockscreen`.

Layout:
- `oe-core/`: OpenEmbedded Core on the latest upstream `master`
- `bitbake/`: BitBake on the latest upstream `master`
- `meta-openembedded/`: OpenEmbedded companion layers
- `meta-raspberrypi/`: Raspberry Pi BSP layer
- `meta-qt6/`: Qt 6 layer
- `meta-cockscreen/`: local layer with distro, machine, image, kernel, Qt, and app integration
- `scripts/`: tracked helper scripts, including idempotent hotfix application for vendored upstream trees
- `sources/`: local source checkouts used by recipes that would otherwise fetch during the build
- `build.sh`: bootstraps the build and runs `bitbake cockscreen-image`

Target choices:
- Distro: `cockscreen`
- Machine: `cockscreen-rpi0-2w-64`
- Init system: SysV init
- No desktop environment, no `systemd`, no `NetworkManager`, no `ModemManager`
- Kernel pinned to the newest `linux-raspberrypi` series currently exposed by the checked-out layer: `6.12%`

Clone:

Fresh clone with pinned submodules:

```bash
git clone --recurse-submodules <repo-url>
cd cockscreen
```

If the repository is already cloned without submodules:

```bash
cd cockscreen
git submodule update --init --recursive
```

Build:

```bash
git submodule update --init --recursive
cd distro
./build.sh
```

`build.sh` reapplies the tracked `oe-core` hotfix script on each run so a fresh checkout does not rely on manual edits inside the ignored upstream clone.
