# Third-party notices

[Back to the quick start](../README.md)

## First-party source

First-party scripts, Shadowlink C source, and build records are included for inspection. No package-wide first-party open-source license has been designated. Availability for reading does not itself grant unrestricted modification or redistribution rights. Existing third-party licenses remain applicable.

## PuTTY for Windows

- Version: **0.85**, Windows x64 `plink.exe` and `pscp.exe`.
- Included license: `PUTTY-LICENCE.txt` in the package root.
- Official download URLs and SHA-256 hashes: `PUTTY-SOURCE.json`.
- Upstream source: [PuTTY 0.85 source archive](https://the.earth.li/~sgtatham/putty/0.85/putty-0.85.tar.gz). Its SHA-256 is also recorded in `PUTTY-SOURCE.json`; the archive is not expanded in this package.
- Reference: [PuTTY command-line options](https://the.earth.li/~sgtatham/putty/0.85/htmldoc/Chapter3.html#using-cmdline).

Linux and macOS use the computer's installed OpenSSH rather than these executables.

## Shadowlink and ARM64 libraries

The matching Shadowlink source is in `source/resource/shadowlink/native/`. The runtime is in `payload/shadowlink/runtime/`. Its top-level `shadowlink-server` is a readable launcher; the compiled program is in `bin/`.

| Runtime file | Contents |
| --- | --- |
| `BUILD-INFO` | Toolchain, build time, architecture, and other build details. |
| `SOURCE-SHA256SUMS` | Recorded source and build-input hashes. |
| `SHA256SUMS` | Runtime file hashes. |
| `PACKAGES` | Exact Debian library-package versions. |
| `licenses/` | Copyright and license records supplied by those packages. |

Full upstream library sources are not all included. Use `PACKAGES` and the source information in `licenses/*.copyright` to locate the relevant versions through [Debian Sources](https://sources.debian.org/) or [Debian Snapshot](https://snapshot.debian.org/). Each component retains its own license. Redistributors must meet the applicable source-availability and other license requirements; these notices do not replace the license texts.

## Existing system and device components

Python, PowerShell, system OpenSSH, device Python/systemd/Docker, installed OpenClaw, HA/Matter/OTBR images, and installed BLE/UI services are provided by the computer or device. Their full source is not redistributed here. See [what the reset does](OPERATIONS.md) for the entries this tool uses.
