# Files and source code

[Back to the quick start](../README.md)

Keep the extracted folder together. Most users only need the launcher for their platform and the `.log` it produces.

| File or folder | What it is for |
| --- | --- |
| `Reset-ClawStage.cmd` | Windows double-click entry. |
| `Reset-ClawStage.command` | macOS double-click entry. |
| `Reset-ClawStage.sh` | Linux entry and shared Unix launcher. |
| `Reset-ClawStage.ps1`, `Reset-Transport.ps1` | Windows orchestration, process handling, and logging. |
| `Find-ClawStage.ps1` | Windows device discovery. |
| `reset-client.py`, `discover_devices.py`, `ssh-askpass.sh` | Linux/macOS orchestration, discovery, and authentication helper. |
| `device-reset.py` | Device checks, background-task startup, and log following. |
| `payload.tar.gz` | The fixed reset payload uploaded to ClawStage. |
| `payload/` | An expanded copy of that same payload for review. |
| `payload/factory-reset.sh` | The device reset sequence. |
| `payload/health-check.sh` | Device service checks. |
| `payload/stack-install.sh` | Home Assistant/Matter/OTBR cleanup helper. |
| `payload/shadowlink/runtime/` | ARM64 programs, libraries, build information, and licenses. |
| `source/` | Matching Shadowlink C source, selected build inputs, tool scripts, documentation, and tests. |
| `plink.exe`, `pscp.exe` | Official PuTTY 0.85 Windows x64 SSH/file-transfer tools. |
| `PUTTY-SOURCE.json`, `PUTTY-LICENCE.txt` | PuTTY provenance, checksums, and license. |
| `package.json`, `payload/manifest.json` | Source metadata and file checksums. |
| `docs/` | User help, file descriptions, reset operations, privacy, and third-party notices. |
| `source/BUILD.md` | Optional build and verification instructions for source reviewers. |
| `.gitignore`, `.gitattributes` | Exclude local run records and preserve released file bytes in Git. |

## What you can inspect

The Python, PowerShell, and Shell scripts are readable source files. Shadowlink is compiled ARM64 code; its matching C source and recorded build inputs are included. PuTTY and shared libraries are also binaries, with upstream/version/license information in [third-party notices](THIRD_PARTY.md).

The expanded `payload/` is provided for review. The device receives `payload.tar.gz`. Editing only the expanded copy does not change the uploaded archive and will fail package verification. Modified releases must be rebuilt and repackaged.

The installed OpenClaw cleanup script and other existing device components are reused. Their full implementations are not copied into this package; review the installed versions as well if you need to assess the entire system.

For optional technical work, see [building, checksums, and verification](../source/BUILD.md). Building anything is not required to use the release ZIP.
