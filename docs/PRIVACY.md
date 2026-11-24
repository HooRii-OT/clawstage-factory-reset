# Privacy and network access

[Back to the quick start](../README.md)

## On your computer

The tool reads its own package and saved task files, local network-interface/routing information, and device information returned over SSH. It does not scan your documents, photos, browser history, or account directories. It does not install a background service, upload logs, or send usage information to GitHub.

It writes logs, `.clawstage-key-*`, `.clawstage-session-*`, and Unix lock files in the tool folder. It also uses temporary files for SSH authentication:

- **Windows:** a temporary password file, deleted during normal exit.
- **Linux/macOS:** an askpass helper and temporary known-hosts file. The password is passed through the child environment and sudo's standard input. Temporary files are removed during normal exit.

A forced termination or power loss can leave temporary files behind. On macOS, the `.command` launcher also asks Finder to reveal the local log after completion.

The default `pi` / `goodlife` credentials are visible in the source. The known login password is redacted from normal logs and is not placed in SSH command-line arguments. This is not a credential vault: an administrator or another process with sufficient access may inspect memory, process environments, or temporary files. Linux/macOS support a changed password through `CLAWSTAGE_PASSWORD`.

The Unix client uses system OpenSSH with explicit connection settings. It does not modify your personal SSH configuration or known-hosts file. Windows uses the bundled PuTTY tools.

## On ClawStage

A reset needs to read and modify binding/account configuration, service state, and component data. The unbind operation may use existing account credentials to contact the cloud through the installed Gateway. Those credentials are then cleared locally as part of the reset.

Typical logs include device names, local IP addresses, versions, paths, run IDs, timestamps, steps, exit codes, and summaries such as whether credentials are present. The tool cannot guarantee that arbitrary output from older installed components contains no sensitive information. Review a log before sharing it; support does not need passwords or full configuration files.

## Network connections

| Connection | Purpose | Destination |
| --- | --- | --- |
| mDNS, UDP `224.0.0.251:5353` | Find ClawStage names and IPv4 addresses on one local `/24` subnet. | Local multicast. |
| SSH/SCP/SFTP, default TCP 22 | Authenticate, upload the reset payload, run checks, and read logs. | The selected device IPv4 address. |
| Gateway unbind API | Attempt account unbinding once. | Device-local API, normally `http://127.0.0.1:18789/hoorii/device/unbind`; the installed Gateway may then contact the cloud. |
| Health checks and RESET WebSocket | Check services and notify the device UI. | Device services according to their existing configuration. |
| Existing services after restart | Resume device operation and setup. | May include their normal internet connections. |

The tool adds no telemetry or log-upload endpoint. It does not automatically download dependencies, fetch new code from GitHub, or pull new container images. The overall reset is not guaranteed to be offline: cloud unbinding and existing services can use the internet.

## Device identity and file integrity

On the first connection, the tool records the advertised SSH host key and checks the device hostname. Later key changes stop the connection. This first-use trust does not independently authenticate the very first device response, so use a trusted local network. Contact support if an SD-card replacement causes a key change.

The SHA-256 manifests detect corruption and inconsistent files. They do not authenticate a package if someone replaces both the files and the manifests. Obtain the release through a trusted channel. Source hashes are not a reproducible-build guarantee or an independent security audit.

## Sharing logs

Logs are never sent automatically. Review the file, then send it to support yourself if needed. The supplied `.gitignore` excludes logs and saved task files from new Git tracking; it does not remove files that were already tracked. See [third-party notices](THIRD_PARTY.md) for source and licensing information.
