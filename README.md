![image](logloader_logo.png)

Downloads PX4 log files (.ulg) and uploads them to a local server and optionally a remote server.

The **config.toml** file is used to configure the program settings.

Works with PX4 (`.ulg`) and ArduPilot (`.BIN`).

### Behavior
Downloading and uploading will only occur while the vehicle is not armed. Downloading and uploading operations are performed in separate threads. An sqlite database per server is used to track log file download/upload status.

### Log transport
The bulk transfer uses **MAVLink FTP** (`FILE_TRANSFER_PROTOCOL`, msg 110), not the classic log protocol (`LOG_DATA`, msg 120).

`LOG_DATA` has no `target_system`/`target_component` fields, so a router in between — mavlink-router, for instance — has no addressing information to work with and copies every chunk to every endpoint it serves, telemetry radio included. `FILE_TRANSFER_PROTOCOL` is addressed, and both PX4 and ArduPilot send the reply back to the requesting sysid/compid, so the transfer is unicast to logloader and the other endpoints stay quiet. No router or autopilot configuration change is needed.

The log *list* still comes from `LOG_ENTRY` (msg 118), which is also untargeted, but it is a handful of bytes per log rather than megabytes, and it is the only source of the timestamp the databases are keyed on.

At startup logloader probes for the vehicle's log directory (`/fs/microsd/log` on PX4, `/APM/LOGS` on ArduPilot) and uses the result to pick the log file extension. Each downloaded file is size-checked against what `LOG_ENTRY` reported before it is moved into the logs directory; anything that does not line up is discarded and re-fetched over `LOG_DATA` instead.

Requirements:
- The autopilot's MAVLink instance must have FTP enabled (`mavlink start -x` on PX4).
- If FTP is unavailable logloader falls back to `LOG_DATA` automatically. On ArduPilot without FTP, set `log_extension = ".BIN"` so downloaded files are named correctly.

### ArduPilot notes
ArduPilot needs FTP: MAVSDK's `LogFiles` plugin cannot enumerate its logs. The plugin assumes PX4's zero-based log ids — it discards any `LOG_ENTRY` whose id is not below `num_logs` and reads the collected entries back out at indices `0..num_logs-1` — while ArduPilot numbers list entries from one, so its final entry always trips that check and `get_entries()` returns `NoLogfiles`. When the FTP probe reports ArduPilot, logloader runs the `LOG_REQUEST_LIST` exchange itself (`LogEntryLister`) without assuming where the numbering starts.

Mapping a list entry onto a file is also stack-specific. ArduPilot reports a *list entry number*, not the log number in the file name, and log numbers wrap at `LOG_MAX_FILES`. logloader reads `LASTLOG.TXT` over FTP and reproduces ArduPilot's own oldest-first ordering to resolve the two.

`.BIN` files are not accepted by review.px4.io, so leave `upload_enabled = false` or point `remote_server` somewhere that understands dataflash logs.

Set `download_protocol` in **config.toml** to `"mavlink"` to restore the old behavior, or `"ftp"` to never fall back.

### Configuration
| Key | Default | Description |
| --- | --- | --- |
| `connection_url` | `udp://:14551` | MAVSDK connection string |
| `local_server` | `http://127.0.0.1:5006` | Local upload target |
| `remote_server` | `https://review.px4.io` | Remote upload target |
| `email` | `""` | Email attached to remote uploads |
| `remote_api_key` | `""` | Per-account API key for authenticated Flight Review (`Authorization: Bearer` + `X-API-Key`). Empty = upload without API key headers (open servers). Generate under /account |
| `upload_enabled` | `false` | Upload to the remote server |
| `public_logs` | `false` | Mark remote uploads public |
| `download_protocol` | `"auto"` | `"auto"`, `"ftp"` or `"mavlink"` |
| `remote_log_directory` | `""` | Override the vehicle log directory |
| `log_extension` | `""` | Override the downloaded file extension |
| `ftp_use_burst` | `true` | Use FTP burst reads |

### Build
Install dependencies
```
sudo apt-get install libsqlite3-dev
```
Install MAVSDK if you haven't already, the latest releases can be found at https://github.com/mavlink/MAVSDK/releases
```
sudo dpkg -i libmavsdk-dev_2.4.1_debian12_arm64.deb
```
Or install MAVSDK from source
```
git clone --recurse-submodules https://github.com/mavlink/MAVSDK.git
cd MAVSDK
cmake -Bbuild/default -DCMAKE_BUILD_TYPE=Release -H.
sudo cmake --build build/default -j$(nproc) --target install
cd ..
```
Upgrade OpenSSL if your version is less than 3.0.2
```
openssl version
```
A script is provided for your convenience
```
./install_openssl.sh
```

Clone this repository
```
git clone --recurse-submodules https://github.com/ARK-Electronics/logloader.git
cd logloader
make
```

### Run
```bash
./build/logloader | tee output.txt
```

```
Downloading...	2023-10-05T15:06:42Z	0.97249500MB	100%	3889.98000000 Kbps
Downloading...	2023-10-05T15:06:58Z	0.46985700MB	98%	3686.40000000 Kbps
Downloading...	2023-10-05T15:10:56Z0 Kb1.86199000MB	100%	4965.30666667 Kbps
Downloading...	2023-10-07T08:52:14Z	0.38373500MB	100%	inf Kbps
Downloading...	2023-10-07T08:56:18Z	0.26428000MB	100%	inf Kbps
Downloading...	2023-10-07T09:04:18Z	0.42428500MB	100%	3394.28000000 Kbps
Downloading...	2023-10-07T09:04:40Z	0.91263600MB	100%	7301.08800000 Kbps
Downloading...	2023-10-07T09:05:20Z	1.33783000MB	100%	3567.54666667 Kbps
Downloading...	2023-10-07T09:07:32Z	0.85526800MB	100%	3421.07200000 Kbps
Downloading...	2023-10-07T09:22:00Z	0.82437500MB	100%	6595.00000000 Kbps
Downloading...	2023-10-07T09:22:26Z	0.79509800MB	100%	6360.78400000 Kbps
Downloading...	2023-10-07T09:26:10Z	0.88394300MB	100%	3535.77200000 Kbps
Downloading...	2023-10-07T10:14:00Z	10.71558000MB	100%	2449.27542857 Kbps
Downloading...	2023-10-07T12:03:40Z	0.64674900MB	100%	5173.99200000 Kbps
Downloading...	2023-10-07T12:05:04Z	3.79014100MB	100%	3032.11280000 Kbps
Downloading...	2023-10-07T12:14:46Z	2.24213400MB	100%	2989.51200000 Kbps
Downloading...	2023-10-07T12:50:12Z	0.78112200MB	100%	3124.48800000 Kbps
```

### Install
Build and install
```
make install
```
| | |
|---------------------|-----------------------------------------|
| **Binary path**      | `/opt/ark/bin/`                       |
| **Default config**   | `/opt/ark/share/logloader/config.toml` |
| **User config**      | `~/.config/ark/logloader/config.toml`  |
| **Data directory**   | `~/.local/share/ark/logloader/`        |
| **Logs directory**   | `~/.local/share/ark/logloader/logs/`   |

### Performance
Monitor network traffic
```
sudo iftop -i wlo1
```
Or use a Wireshark filter
```
mavlink_proto.msgid == 117 || mavlink_proto.msgid == 118 || mavlink_proto.msgid == 119 || mavlink_proto.msgid == 120
```

Watch your beautiful logs arrive

### Future developments
- Multiple backends: e.g. RobotoAI, DroneLogbook, Auterion Suite, Aloft etc
