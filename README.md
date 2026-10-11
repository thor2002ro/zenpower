# Zenpower
Zenpower is Linux kernel driver for reading temperature, voltage(SVI2), current(SVI2) and power(SVI2) for AMD Zen family CPUs.

Make sure that your Linux kernel have support for your CPUs as Zenpower is using kernel function `amd_smn_read` to read values from SMN. A fallback method (which may or may not work!) will be used when it is detected that kernel function `amd_smn_read` lacks support for your CPU.
For AMD family 17h Model 70h (Ryzen 3000) CPUs you need kernel version 5.3.4 or newer or kernel with this patch: https://patchwork.kernel.org/patch/11043277/

## Installation
You can install this module via dkms.

### Arch Linux / Manjaro package

The local `PKGBUILD` packages this checkout as `zenpower-dkms`, including its
sources and DKMS configuration in `/usr/src/zenpower-0.6.0`. Install `base-devel`
and the headers matching each kernel you want to use (for example,
`linux-headers` on Arch's standard kernel, `linux-lts-headers` for Arch LTS, or
the matching Manjaro kernel headers).

Run from this repository as your normal user:

```sh
makepkg -si
```

This builds a pacman package, installs the `dkms` dependency if needed, and
installs the package. Arch's DKMS hooks build and install the module for kernels
with headers installed, and rebuild it after kernel updates. Check the result
with `dkms status`, then follow the module activation steps below.

To build without installing, run `makepkg -s`. To remove the package and its
DKMS modules, run `sudo pacman -R zenpower-dkms`.

The package installs `/usr/lib/modprobe.d/zenpower.conf` to blacklist k10temp
from automatic loading. Removing the package removes this blacklist. If
k10temp is already loaded, unload it before activating zenpower as shown below.

If you edit a packaged source file, refresh the `PKGBUILD` checksums with
`updpkgsums` (from `pacman-contrib`) and rebuild with `makepkg -fsi`. Keep
`pkgver` in sync with `VERSION` in the Makefile when changing the module version.

### Installation commands for Ubuntu
```
sudo apt install dkms git build-essential linux-headers-$(uname -r)
cd ~
git clone https://github.com/ocerman/zenpower.git
cd zenpower
sudo make dkms-install
```

## Module activation
Because zenpower is using same PCI device as k10temp, you have to disable k10temp first.

1. Check if k10temp is active. `lsmod | grep k10temp`
2. Unload k10temp `sudo modprobe -r k10temp`
3. For manual installations, optionally copy the blacklist file:
   `sudo install -Dm644 zenpower.conf /etc/modprobe.d/zenpower.conf`.
   The Arch package already installs it.
4. Activate zenpower `sudo modprobe zenpower`

If k10temp is not blacklisted, you may have to manually unload k10temp after each restart.

## Sensors monitoring
You can use this app: [zenmonitor](https://github.com/ocerman/zenmonitor), or your favourie sensors monitoring software

## Update instructions
1. Unload zenpower `sudo modprobe -r zenpower`
2. Goto zenpower directory `cd ~/zenpower`
3. Uninstall old version `sudo make dkms-uninstall`
4. Update code from git `git pull`
5. Install new version `sudo make dkms-install`
6. Activate zenpower `sudo modprobe zenpower`

## Help needed
It would be very helpful for me for further development of Zenpower if you can share debug data from zenpower. [Read more](https://github.com/ocerman/zenpower/issues/12)

## Notes
 - Some users reported that a restart is needed after module installation
 - If you are having trouble compiling zenpower under Ubuntu 18.04 (or older) with new upstream kernel, see [#23](https://github.com/ocerman/zenpower/issues/23)
 - The meaning of raw current values from SVI2 telemetry are not standardised so the current/power readings may not be accurate on all systems (depends on the board model).
