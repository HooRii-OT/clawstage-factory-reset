# Results, logs, and reconnecting

[Back to the quick start](../README.md)

## Read the result

| Result | Meaning | What to do |
| --- | --- | --- |
| `SUPPORT RESULT=SUCCESS exit_code=0` | Local cleanup and health checks finished. | Set up the device again. Contact support if the app still shows it as bound. |
| `FAILED` or a nonzero exit code | A step failed; some data may already be erased. | Keep the log and contact support. |
| `UNKNOWN`, a timeout, or a disconnected window | Completion has not been confirmed. | Use `-Follow` (Windows) or `--follow` (macOS/Linux) from the same folder. |

The tool tries cloud unbinding once, for up to 12 seconds. A cloud failure does not block local cleanup. A local cleanup or health-check failure still makes the task fail. An HTTP 200 response alone does not prove the cloud account record was deleted.

## Find your log

Each run saves a new `Reset-ClawStage-YYYYMMDD-HHMMSS-ID.log` **in the same folder as the launcher**. Logs and filenames use UTC+8; the tool does not change your computer or device timezone.

On macOS, the `.command` launcher tries to select the file in Finder. On Linux and macOS, the final `Local log saved on this computer` message prints its full path. On Windows, the path appears at startup, and the file remains after the window closes.

The device retains a second copy at `/var/lib/clawstage-support-reset/<Run ID>/reset.log`. That path belongs to ClawStage, not your computer. Its `result.json` records the final exit code. Support can read these files with device administrator access.

An operating-system block or a missing Python installation may stop the launcher before it can create a log. Keep the system message or terminal error in that case.

## Continue after a disconnection

The reset runs in the background on ClawStage. Closing the computer window or losing SSH does not cancel it. **A normal launch always requests a new full reset; it does not reuse an old success result.** To read the last saved task without clearing data again, use:

```powershell
# Windows
.\Reset-ClawStage.cmd -Follow
```

```sh
# macOS
sh ./Reset-ClawStage.command --follow
# Linux
sh ./Reset-ClawStage.sh --follow
```

Use the same tool folder and, when needed, the same `-Device`/`-Port` or `--device`/`--port` options. Keep `.clawstage-session-*` and `.clawstage-key-*` when updating. Follow mode requires a matching saved task and never starts a reset if that record is missing. Its result describes that historical task, not the device's current account state.

A new invocation gets a new run ID and performs the full reset again. The latest task replaces the local follow target; previous device logs remain available. `-NewReset` and `--new-reset` are still accepted but are no longer necessary.

If a support reset is already active on the device, a second start is rejected without queuing another cleanup. The client records the active task for explicit follow mode. This device-side check also covers requests from another computer. Wait for the active task to finish before starting a new reset.

If a launch response is lost, use follow mode to determine the outcome. The tool does not automatically retry the destructive operation. A normal launch after the previous task finishes will erase again.

The task has a 40-minute limit. A power interruption does not automatically resume that process. Existing firmware may have its own recovery behavior; it does not prove that this tool finished successfully.

## Common messages

| Message or symptom | Next step |
| --- | --- |
| No ClawStage found | Check power, network, VPN, firewall, and guest Wi-Fi isolation. Use the platform's explicit-IP option if you know the device address. |
| Several devices found | Select exactly the device you intend to erase. The tool does not reset all devices. |
| SSH host key changed | Stop and contact support to verify the device identity. An SD-card replacement can change the key, but it should not be accepted automatically. |
| Package checksum mismatch | Download and extract a fresh, trusted release. Preserve saved task files when replacing an existing copy. |
| Missing required command or component | The installed device does not meet the tool's checks. Send the log to support. |
| Optional `clawstage-shadowlink-reset.service` not loaded | Some older firmware lacks this optional unit. Check the final result; this message alone does not mean reset failed. |
| App still shows a binding after local success | Cloud unbinding may need separate support follow-up. Do not repeatedly erase the device. |

## Contact support

Review the log before sending it to [support@hoorii.io](mailto:support@hoorii.io). Logs can contain device names, local IP addresses, paths, versions, and diagnostic output. Do not send passwords or full device configuration files. The tool does not send email or upload logs for you.
