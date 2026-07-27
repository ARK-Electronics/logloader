![image](logloader_logo.png)

Downloads flight logs from the vehicle and uploads them to a local server and optionally a remote server. Works with PX4 (`.ulg`) and ArduPilot (`.BIN`).

The **config.toml** file is used to configure the program settings.

### Behavior
Downloading and uploading will only occur while the vehicle is not armed. Downloading and uploading operations are performed in separate threads. An sqlite database per server is used to track log file download/upload status.

### Log transport
Everything runs over **MAVLink FTP** (`FILE_TRANSFER_PROTOCOL`, msg 110): the directory listing that finds the logs and the bulk transfer that downloads them. The classic log protocol (`LOG_REQUEST_LIST` / `LOG_ENTRY` / `LOG_DATA`, msgs 117-120) is not used at all.

`FILE_TRANSFER_PROTOCOL` carries `target_system` and `target_component`, and both PX4 and ArduPilot address their replies back to the requesting sysid/compid, so a download is unicast between the vehicle and logloader. `LOG_DATA` has no target fields, which leaves a router in between — mavlink-router, for instance — no choice but to copy every chunk to every endpoint it serves, telemetry radio included. A log download over the old protocol saturates links that have no interest in it.

The autopilot's MAVLink instance must have FTP enabled (`mavlink start -x` on PX4). If it does not, logloader says so and idles.

### Log discovery
At startup logloader lists the vehicle's log directory, trying `@MAV_LOG` first — the virtual log directory the MAVLink FTP specification defines, supported by PX4 v1.17 and newer — then the physical `/fs/microsd/log` (PX4) and `/APM/LOGS` (ArduPilot). Set `remote_log_directory` to skip the probe. The listing is what identifies a log: its path below the log root plus its size. PX4's nested `<date>/<time>.ulg` layout and ArduPilot's flat `<number>.BIN` layout are both handled, and the file extension follows whatever the vehicle actually has.

Log timestamps come from the modification time in the listing when the vehicle supports the `ListDirectoryWithTime` opcode (PX4 v1.17 and newer). Otherwise PX4 logs fall back to the start time encoded in the path, and ArduPilot logs have no timestamp — nothing depends on having one.

Downloads are staged in a temporary directory and only moved next to the finished logs once the transferred size matches the listing, so a partial file is never mistaken for a complete one.

`.BIN` files are not accepted by review.px4.io, so on ArduPilot leave `upload_enabled = false` or point `remote_server` somewhere that understands dataflash logs.

### Upgrading from a pre-FTP logloader
Older versions identified logs by the timestamp `LOG_ENTRY` reported, which MAVLink FTP cannot reproduce. On first start the existing `logs` table is renamed to `logs_legacy` and its rows are matched against the FTP listing by size, so logs already downloaded and uploaded are not fetched or uploaded a second time. Nothing is deleted; `logs_legacy` stays behind for inspection.

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
| `remote_log_directory` | `""` | Override the vehicle log directory |
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
mavlink_proto.msgid == 110
```

Watch your beautiful logs arrive

### Future developments
- Multiple backends: e.g. RobotoAI, DroneLogbook, Auterion Suite, Aloft etc
