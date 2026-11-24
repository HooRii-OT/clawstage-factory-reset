# Building, checksums, and verification

This page is for source reviewers and maintainers. In the release package it is located at `source/BUILD.md`; run the commands below from the package root. To use the release, start with the [platform instructions](../README.md); no build is required.

## Verify a downloaded package

On Linux/macOS:

```sh
sh ./Reset-ClawStage.sh --verify-only
```

On Windows:

```powershell
.\Reset-ClawStage.cmd -VerifyOnly
```

Verification creates a local log but does not connect to a device. `package.json` hashes the delivered files except itself. The payload has its own manifest. These hashes check consistency, not publisher identity.

To compare the expanded payload with the archive without executing it, run this from the package root with Python 3.9+:

```sh
python3 - <<'PY'
from pathlib import Path
import tarfile
root = Path('.')
with tarfile.open(root / 'payload.tar.gz') as archive:
    names = set()
    for member in archive.getmembers():
        assert member.isfile(), member.name
        path = root / 'payload' / member.name
        assert path.resolve().is_relative_to((root / 'payload').resolve())
        assert archive.extractfile(member).read() == path.read_bytes(), member.name
        names.add(member.name)
    assert names == {p.relative_to(root / 'payload').as_posix()
                     for p in (root / 'payload').rglob('*') if p.is_file()}
print('Archive matches expanded payload')
PY
```

Runtime `SOURCE-SHA256SUMS` paths are relative to `source/resource/shadowlink/native/`. Runtime `SHA256SUMS` paths are relative to the runtime directory. `BUILD-INFO` records the architecture and toolchain; `PACKAGES` lists bundled library versions.

## Package from the installer repository

Build an ARM64 Shadowlink runtime matching the current source using the installer's existing native build process. Prepare official PuTTY 0.85 x64 `plink.exe`, `pscp.exe`, and the `LICENCE` from that version's source archive.

```sh
python3 script/package-support-reset.py \
  --runtime resource/shadowlink/runtime \
  --windows-tools /path/to/putty-0.85-w64 \
  --output dist/ClawStage-Factory-Reset.zip
```

The packager checks the fixed PuTTY hashes, runtime files, and recorded source hashes. It produces a ZIP, an expanded folder, and `.zip.sha256`. It refuses to overwrite an existing output ZIP or folder. Build into a fresh staging directory, verify it, and then replace release files while preserving any user logs and saved tasks.

Packaging uses the current branch and working-tree contents, including uncommitted changes. The manifest records the commit, branch, source status, version, and file hashes. A release is a fixed snapshot; it does not fetch later changes at runtime.

## Repackage the exported source

Use `source/script/package-support-reset.py` with `--runtime` pointing to the package's `payload/shadowlink/runtime`. The tools directory must contain both official EXEs and a file named `LICENCE` with the contents of `PUTTY-LICENCE.txt`.

Changes to C source require rebuilding the runtime first. Without Git metadata, commit/branch are recorded as null, `original_source` retains the export's origin, and hashes record the current file contents.

The export contains selected source/build inputs, not the full installer repository. The full Docker build still needs that repository. To build and test Shadowlink alone, install CMake, a C11 compiler, pkg-config, OpenSSL 3, and the GLib/GIO, json-c, and libcurl development libraries in a Linux environment:

```sh
cmake -S source/resource/shadowlink/native -B /tmp/clawstage-shadowlink-build
cmake --build /tmp/clawstage-shadowlink-build -j2
ctest --test-dir /tmp/clawstage-shadowlink-build --output-on-failure
```

An x64 host build cannot replace the ARM64 runtime. A device release needs the ARM64 toolchain, matching libraries, and target-architecture validation. Source hashes do not prove a byte-for-byte reproducible build.

## Automated checks

From the exported package:

```sh
python3 -m unittest discover -s source/test -p 'test_support_reset.py' -v
python3 -m unittest discover -s source/test -p 'test_unix_support_reset.py' -v
# Requires PowerShell; PWSH can select its executable on Linux.
python3 -m unittest discover -s source/test -p 'test_windows_support_reset.py' -v
```

The tests use temporary directories and simulated devices/transports, not real device cleanup. The worker suite has 12 checks, the Unix client suite 16, and the Windows client suite 13. Windows tests skip when PowerShell is unavailable; a skip is not a pass.

Coverage includes a new run on every default launch, explicit follow after lost launch responses, missing saved tasks, device-side concurrent-launch rejection, host-key changes, missing requirements, corrupt payloads, exit codes, password redaction, timeouts, network/DNS parsing, paths with spaces, and log location. Launcher checks include an unrelated working directory, a limited PATH, an interactive terminal waiting for Enter, Finder success/failure handling, and refusing connection when a log cannot be created.

The default-new-reset behavior added on 2026-11-24 is covered by simulated transport and concurrent-admission tests. It has not been used to erase a real device during this change.

## Device-validation status

| Platform | Recorded evidence | Remaining limitation |
| --- | --- | --- |
| Windows | Customer logs show local resets on 0.2.6 and 0.2.16-test4. | This does not validate every Windows or firmware version. |
| macOS | A 2026-11-24 customer terminal run completed a local 0.2.6 reset in 64 seconds. | The later Finder/log-launcher adjustments still need Mac verification; Gatekeeper approval cannot be tested on Linux. |
| Linux | Real discovery, automatic SSH login, upload, and inspection passed against 0.2.6 on 2026-11-23. | No complete real-device reset was performed through the Linux client. |

Local success and a Gateway HTTP 200 response do not prove cloud business-level unbinding. Complete device acceptance also needs setup/rebinding checks. Use check-only modes for diagnostics; do not run a destructive reset on a customer device merely to test connectivity.

## Publishing a release

Publish the clean package folder and, optionally, the release ZIP and checksum. Exclude logs, `.clawstage-*`, credentials, and device configuration. `.gitignore` does not untrack files that were already committed. `.gitattributes` preserves bytes so checkout line-ending conversion does not invalidate hashes.

Keep the versioned source, expanded payload, archive, and manifests consistent. A GitHub-generated source archive may not retain executable permissions; Unix users can invoke the launchers with `sh`. First-party licensing and third-party obligations are described in [third-party notices](../docs/THIRD_PARTY.md).

The release keeps user documentation only in `docs/` and build notes in `source/BUILD.md`. It does not duplicate the documentation under `source/docs/`. Client scripts and user documents are copied from explicit file lists, excluding scratch scripts and unrelated Markdown files. The exported packager reads documentation from the package root when rebuilding from `source/`.
