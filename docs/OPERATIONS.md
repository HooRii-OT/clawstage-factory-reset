# What the reset does

[Back to the quick start](../README.md)

The tool clears the device's account and application data, then checks that services are ready for setup again. It preserves the device identity and installed firmware version. It does not make a backup of the data it erases.

The package contains a fixed reset implementation. It does not download newer reset code while running, and it does not call the device's old `factory_reset.py`. There is no firmware-version whitelist: the device must pass architecture, command, directory, and component-entry checks before proceeding. This does not guarantee compatibility with every past or future release.

## Reset sequence

1. **Check files and identify the device.** Verify package checksums, discover or select a device, check its SSH host key, and verify its ClawStage hostname.
2. **Check the installation.** Resolve `/home/pi/clawstage/current` and check the ARM64 architecture, required commands, and installed cleanup entries. Upload the reset worker and payload to a unique directory.
3. **Start a new background task.** Reject a second launch if another support reset is active; completed tasks do not block a new reset. Validate the uploaded archive and its manifest, freeze the files in a root-owned directory, and start a systemd task with a 40-minute limit.
4. **Prepare local cleanup.** Acquire maintenance locks, stop related maintenance workers, verify that the active installation has not changed, preserve available public Matter certificates, and replace stale reset/OTA/factory transaction records.
5. **Attempt cloud unbinding.** Make one request through the device's local Gateway, with a 12-second limit. Continue local cleanup regardless of the cloud response.
6. **Clear binding and OpenClaw data.** Stop BLE, clear account binding fields while preserving device identity, and run the installed OpenClaw cleanup entry as `pi`.
7. **Clear Home Assistant, Matter, and Thread data.** Use the bundled stack installer with the installed stack configuration. Restore the cached public Matter certificates.
8. **Restart and verify services.** Start the stack using local container images, restart the installed BLE service, run health checks, and send the local RESET notification.
9. **Record completion.** Restore the installation's ready marker, remove completed reset records, and save the result and detailed log.

A cloud failure does not stop local cleanup. A local cleanup or health-check failure does stop the task with an error; data may already have been partially erased. `SUCCESS` confirms local completion, not deletion of a cloud account record.

## Files and services involved

| Location or entry | Purpose |
| --- | --- |
| `/home/pi/clawstage/current` | Selects the installed release; checked again under the maintenance lock. |
| `/home/pi/clawstage-support-upload-<id>/` | Receives the worker and payload. Inspection mode also uploads a worker. |
| `/var/lib/clawstage-support-reset/<id>/` | Stores the frozen task files, `request.json`, `reset.log`, and `result.json`. |
| `/home/pi/clawstage/.factory-reset-paa` | Default temporary cache for public Matter certificates. |
| Installed `user/.hoorii/config.json` | Device account/binding configuration. |
| Installed `openclaw/hoorii_claw/script/clear.sh` | OpenClaw's existing account cleanup entry. Its implementation depends on the installed version. |
| Installed `user/.clawstage-openclaw-user-settings.json` | Legacy account settings record removed during cleanup. |
| Installed `user/.hoorii/stage-stack` | Home Assistant, Matter, and OTBR/Thread data. |
| Installed `homeassistant/stage-stack/compose.release.yaml` | Defines the stack restarted from local images. |
| `shadowlink-ble-server.service` | Existing BLE service restarted after cleanup. |

The bundled stack installer runs with `--purge all -f --no-pull --no-start`. The reset then starts the existing Compose stack with `--no-build --pull never`. Missing local images cause failure rather than an automatic download. The installer prints `Services started` only after an actual successful start; the reset log records its separate start and health-check steps.

Some old firmware lacks the optional `clawstage-shadowlink-reset.service`. A warning when stopping that unit does not mean the package is incomplete. Failures involving required services are still reported.

## Scope and retained data

This is an account/application reset, not a whole-SD-card wipe or a secure-erasure procedure. Historical copies, system journals, and storage remnants are not guaranteed to disappear.

The tool does not change the firmware version or the `current` symlink and does not replace installed scripts or binaries. It runs its support payload separately. The installed OpenClaw cleanup script, container images, Compose configuration, BLE service, and UI remain part of the operation. Their full source code is not included in this package.

Upload directories and task records remain on the device for troubleshooting. Local logs, saved task files, and host-key records remain on the computer. Do not delete task files while a reset is running. The tool does not roll back erased data or automatically begin another cleanup after failure.

For a command-level review, read `device-reset.py` and `payload/factory-reset.sh`. See [privacy and network access](PRIVACY.md) for what information is read or transmitted.
