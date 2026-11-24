# ClawStage Factory Reset

Reset your ClawStage from a Windows, Mac, or Linux computer. Download the release ZIP, extract the entire folder, and connect your computer and ClawStage to the same local network.

If ClawStage returns to the QR code screen after a factory reset, but scanning the code with the mobile app still shows that the device is already bound, please contact [support@hoorii.io](mailto:support@hoorii.io). Include your factory reset log and the account name you use in the app.

| Your computer | Start here |
| --- | --- |
| **Windows** | Double-click **`Reset-ClawStage.cmd`**. |
| **macOS** | Double-click **`Reset-ClawStage.command`**. If blocked, follow [First-time opening](#first-time-opening). |
| **Linux** | Open a terminal in the extracted folder and run **`sh ./Reset-ClawStage.sh`**. |

**This erases device account bindings, OpenClaw user data, and Home Assistant, Matter, and Thread data. There is no backup.** If one device is found, the reset starts automatically. If several are found, select the device you want to reset. The tool signs in using the device's default `pi` / `goodlife` credentials.

Your `.log` file is saved **beside the launcher**. Keep the window open until the result appears. **Every normal launch starts a new full reset, even if a previous task succeeded.** To read an existing task without resetting again, use `-Follow` on Windows or `--follow` on macOS/Linux. A device with a reset already running rejects another launch; it does not queue a second cleanup. Keep hidden `.clawstage-*` files for explicit log following.

## Windows

### Requirements and starting

Use Windows x64 with Windows PowerShell 5.1 or later. Python and SSH tools do not need to be installed separately.

1. Extract the ZIP to a folder you can write to, such as Downloads. Do not run it inside the ZIP preview.
2. Connect to the same Wi-Fi network as ClawStage.
3. Double-click `Reset-ClawStage.cmd` and select a device if prompted.
4. Wait for the result. The window closes after 10 seconds; the `.log` remains in the same folder.

### Firewall and device discovery

If Windows Firewall asks about **Windows PowerShell**, allow it on your **trusted Private network**. If you previously denied access, open **Windows Security → Firewall & network protection → Allow an app through firewall** and allow Windows PowerShell for that network. Changing firewall permissions may require an administrator. Keep the firewall enabled. [Microsoft firewall help](https://support.microsoft.com/en-us/windows/security/windows-security/firewall-and-network-protection-in-the-windows-security-app).

Automatic discovery needs one active Wi-Fi IPv4 address with subnet mask `255.255.255.0`. A VPN, guest Wi-Fi, client isolation, or blocked mDNS traffic can prevent discovery. For Ethernet, another subnet mask, or a known device address, use `-Device` below. Specifying an address bypasses discovery; it still requires SSH access to the device.

### Optional commands

Open PowerShell in the extracted folder. Replace the example IP and port with your device's values.

```powershell
# Reset a specific device
.\Reset-ClawStage.cmd -Device 192.168.3.178

# Reset using a non-default SSH port
.\Reset-ClawStage.cmd -Device 192.168.3.178 -Port 2222

# Read the last saved task only; do not start another reset
.\Reset-ClawStage.cmd -Follow

# Check package files only; do not connect to a device
.\Reset-ClawStage.cmd -VerifyOnly

# Check device requirements; upload a check script, but do not reset
.\Reset-ClawStage.cmd -Device 192.168.3.178 -InspectOnly
```

Use `-Follow` after a disconnection to check the saved task without clearing data again. `-NewReset` remains accepted for compatibility but is no longer needed: a normal launch already starts a new reset. This Windows launcher requires the default device password.

## macOS

### Requirements and starting

Use an Intel or Apple Silicon Mac with **Python 3.9+** and **OpenSSH 8.4+**. If Python is missing, install it from [python.org](https://www.python.org/downloads/macos/). The launcher checks common Python installation locations.

1. Extract the ZIP to a writable folder, such as Downloads.
2. Connect to the same local network as ClawStage.
3. Double-click `Reset-ClawStage.command` and select a device if prompted.
4. Wait for the result. Finder will try to select the local `.log` file. Press Enter to finish the launcher.

### First-time opening

If macOS says it cannot verify the developer or cannot open the file:

1. Dismiss the message after attempting to open `Reset-ClawStage.command`.
2. Go to **System Settings → Privacy & Security**.
3. Find the blocked-file notice, click **Open Anyway**, and confirm the system prompt.

Only approve a package obtained from a trusted source. This script package is not signed or notarized by Apple; the system's first-open approval happens before the script can run. [Apple opening instructions](https://support.apple.com/en-us/102445).

If the file opens as text, use **Open With → Terminal**. If it lacks executable permission, run `chmod +x Reset-ClawStage.command` in the extracted folder. If Terminal starts in another directory, the launcher still finds its files and saves logs beside itself.

### Network and logs

Automatic discovery uses the default network interface and requires one IPv4 address on a `/24` network (`255.255.255.0`). VPNs, guest networks, or local network restrictions can interfere. Use a device IP or select an interface below if needed.

The final **`Local log saved on this computer`** line gives the Mac file path. **`Device log on ClawStage (remote copy)`** refers to a second copy on the device. Your Mac log is always in the tool folder, even when Terminal starts elsewhere.

### Optional commands

Open Terminal in the extracted folder. Replace the example IP, port, or interface as needed.

```sh
# Reset a specific device, optionally on a different SSH port
sh ./Reset-ClawStage.command --device 192.168.3.178
sh ./Reset-ClawStage.command --device 192.168.3.178 --port 2222

# Read the last saved task only; do not start another reset
sh ./Reset-ClawStage.command --follow

# Check package files only; do not connect
sh ./Reset-ClawStage.command --verify-only

# Find devices only, using a selected network interface
sh ./Reset-ClawStage.command --discover-only --interface en0

# Check device requirements without resetting it
sh ./Reset-ClawStage.command --device 192.168.3.178 --inspect-only
```

`--interface en0` without `--discover-only` runs the normal reset flow. Use `--follow` to read a saved task without resetting again. `--new-reset` is an optional compatibility alias for the default new-reset behavior. If the device password has changed, set `CLAWSTAGE_PASSWORD` in the terminal environment before launching.

## Linux

### Requirements and starting

Use Linux x64 or ARM64 with **Python 3.9+**, **OpenSSH client 8.4+**, and **iproute2** (`ip`) for automatic discovery. Install missing dependencies with your distribution's package manager. Run the tool as your normal user; no `sudo` is needed on your computer.

1. Extract the ZIP to a writable folder.
2. Connect to the same local network as ClawStage.
3. Open a terminal in that folder and run `sh ./Reset-ClawStage.sh`.
4. Select a device if prompted and wait for the result. The local `.log` path appears at the end.

File managers handle double-clicking shell scripts differently, so the terminal command is the supported starting method.

### Network and permissions

Automatic discovery uses the default network interface and requires one IPv4 address on a `/24` network (`255.255.255.0`). If multiple interfaces or a VPN make the choice ambiguous, use `--interface` or `--device` below.

If discovery finds nothing, check that your firewall and router allow local mDNS traffic and that Wi-Fi client isolation is off. Keep the firewall enabled. A direct IP skips discovery but still requires access to the device's SSH port. If the log cannot be created, move the complete tool folder to a writable location and run it again as your normal user.

### Optional commands

Run these from the extracted folder. Replace the example IP, port, or interface as needed.

```sh
# Reset a specific device, optionally on a different SSH port
sh ./Reset-ClawStage.sh --device 192.168.3.178
sh ./Reset-ClawStage.sh --device 192.168.3.178 --port 2222

# Read the last saved task only; do not start another reset
sh ./Reset-ClawStage.sh --follow

# Check package files only; do not connect
sh ./Reset-ClawStage.sh --verify-only

# Find devices only, using a selected network interface
sh ./Reset-ClawStage.sh --discover-only --interface wlan0

# Check device requirements without resetting it
sh ./Reset-ClawStage.sh --device 192.168.3.178 --inspect-only
```

`--interface wlan0` without `--discover-only` runs the normal reset flow. Use `--follow` to read a saved task without resetting again. `--new-reset` is an optional compatibility alias for the default new-reset behavior. If the device password has changed, set `CLAWSTAGE_PASSWORD` in the terminal environment before launching.

---

**Need help?** Read [results, logs, and reconnecting](docs/HELP.md), or contact [support@hoorii.io](mailto:support@hoorii.io) with your `.log` file after reviewing its contents. Logs are never uploaded automatically. Local reset success does not guarantee that the cloud account record was removed.

**Want to review the tool?** See [what it resets](docs/OPERATIONS.md), [privacy and network access](docs/PRIVACY.md), [files and source code](docs/FILES.md), and [third-party notices](docs/THIRD_PARTY.md).
