![image](logloader_logo.png)

Downloads flight logs from the vehicle over MAVLink FTP and uploads them to a local Flight Review and, optionally, a remote one. Works with PX4 (`.ulg`) and ArduPilot (`.BIN`).

### What it fetches, and when

logloader does **not** try to mirror the SD card. A companion computer that is powered on for the first time, or one whose database has been reset, would otherwise pull and upload the vehicle's entire history over a link that has better things to do.

The rule is:

- **A log that appears while logloader is watching** is downloaded and uploaded. In practice that means the log a flight just produced.
- **On a database that has never seen this vehicle**, only the most recent log is taken. Everything else is left alone.
- **Anything else happens because it was asked for**, from the ARK-OS Logs page or the HTTP API below.

Two further guards fall out of the same idea. A log is only considered at all once two consecutive listings agree on its size, so the log being written right now is never fetched at a size it will not keep. And if a single listing turns up more than `download.max_auto_queue` new logs, that is a card logloader has not seen rather than a flight that just happened, so it queues only the newest and says so.

Downloading and uploading are suspended while the vehicle is armed.

### HTTP API

A small localhost API on port 3005 (`[api]` in the config) is what the ARK-OS Logs page talks to.

| | |
| --- | --- |
| `GET /status` | connection, arm state, current transfer |
| `GET /logs` | every known log with its download and upload state |
| `GET /events` | the same as server-sent events, one per change |
| `POST /logs/download` | `{"ids": [1,2]}` or `{"all": true}`; `"upload": false` to fetch without uploading |
| `POST /logs/upload` | `{"ids": [1,2]}` or `{"all": true}`, optional `"targets": ["local"]` |
| `POST /logs/cancel` | `{"ids": [1,2]}` — clears pending requests |
| `DELETE /logs/{id}/file` | removes the downloaded copy to free space |

A successful upload records the path Flight Review redirected to (`/plot_app?log=<uuid>`), which is what lets the UI link straight to the plot.

### Log transport

Everything runs over **MAVLink FTP** (`FILE_TRANSFER_PROTOCOL`, msg 110): the directory listing that finds the logs and the bulk transfer that downloads them. The classic log protocol (`LOG_REQUEST_LIST` / `LOG_ENTRY` / `LOG_DATA`, msgs 117-120) is not used at all.

`FILE_TRANSFER_PROTOCOL` carries `target_system` and `target_component`, and both PX4 and ArduPilot address their replies back to the requesting sysid/compid, so a download is unicast between the vehicle and logloader. `LOG_DATA` has no target fields, which leaves a router in between — mavlink-router, for instance — no choice but to copy every chunk to every endpoint it serves, telemetry radio included. A log download over the old protocol saturates links that have no interest in it.

The autopilot's MAVLink instance must have FTP enabled (`mavlink start -x` on PX4). If it does not, logloader says so and idles.

### Log discovery

logloader lists the vehicle's log directory, trying `@MAV_LOG` first — the virtual log directory the MAVLink FTP specification defines, supported by PX4 v1.17 and newer — then the physical `/fs/microsd/log` (PX4), `/APM/LOGS` (ArduPilot) and `/log` (PX4 SITL). Set `download.remote_directory` to skip the probe.

The listing is what identifies a log: its path below the log root plus its size. PX4's nested `<date>/<time>.ulg` layout and ArduPilot's flat `<number>.BIN` layout are both handled.

Timestamps come from the modification time in the listing when the vehicle supports the `ListDirectoryWithTime` opcode (PX4 v1.17 and newer). Otherwise PX4 logs fall back to the start time encoded in the path, and ArduPilot logs have no timestamp — nothing depends on having one.

Downloads are staged in a temporary directory and only moved next to the finished logs once the transferred size matches the listing, so a partial file is never mistaken for a complete one.

`.BIN` files are not accepted by review.px4.io, so on ArduPilot leave the remote target disabled or point it somewhere that understands dataflash logs.

### Upgrading from a pre-FTP logloader

Older versions kept a database per server and identified logs by the timestamp `LOG_ENTRY` reported, which MAVLink FTP cannot reproduce. On first start `local_server.db` and `remote_server.db` are imported into a single `logloader.db` and their rows matched against the FTP listing by size, so logs already downloaded and uploaded are not fetched or uploaded a second time. The old files are left untouched.

### Configuration

`config.toml`, in full, is documented inline in the shipped file. The keys:

| Key | Default | Description |
| --- | --- | --- |
| `connection_url` | `udp://:14551` | MAVSDK connection string |
| `log_level` | `info` | `error`, `warn`, `info` or `debug` |
| `data.directory` | `$XDG_DATA_HOME/ark/logloader/` | Logs and the database |
| `api.enabled` / `api.bind` / `api.port` | `true` / `127.0.0.1` / `3005` | Local HTTP API |
| `download.auto` | `true` | Queue logs that appear while running |
| `download.latest_on_first_start` | `true` | On a fresh database, take only the newest |
| `download.auto_upload` | `true` | Upload what was queued automatically |
| `download.max_auto_queue` | `5` | Bulk-discovery guard; 0 disables |
| `download.index_interval` | `30` | Seconds between listings |
| `download.remote_directory` | `""` | Override the vehicle log directory |
| `download.use_burst` | `true` | FTP burst reads |
| `upload.interval` | `10` | Seconds between upload passes |
| `upload.local.*` | enabled, `http://127.0.0.1:5006` | Flight Review on the companion |
| `upload.remote.*` | disabled, `https://logs.px4.io` | `url`, `email`, `public`, `api_key` |

The flat keys the previous layout used (`local_server`, `remote_server`, `upload_enabled`, `public_logs`, `email`, `remote_api_key`, `remote_log_directory`, `ftp_use_burst`, `application_directory`) are still read, so an existing config keeps working.

### Build

```
sudo apt-get install libsqlite3-dev
```
Install MAVSDK if you haven't already; releases are at https://github.com/mavlink/MAVSDK/releases
```
sudo dpkg -i libmavsdk-dev_3.17.1_ubuntu24.04_amd64.deb
```
OpenSSL must be 3.0.2 or newer (`openssl version`); `./install_openssl.sh` is provided if it is not.

```
git clone --recurse-submodules https://github.com/ARK-Electronics/logloader.git
cd logloader
make
```

`make` builds Release. `make debug` builds with debug symbols and no optimisation; the log level is a runtime setting either way.

### Run

```bash
./build/logloader --config config.toml
```

```
logloader starting, configuration from config.toml
API listening on 127.0.0.1:3005
Connected to the autopilot
MAVLink FTP available, indexed 6 logs in /fs/microsd/log
First index: 5 logs on the vehicle, queueing only the newest. Use the ARK-OS Logs page to fetch any of the others.
Downloading 1/1: 2026-07-28/17_21_01.ulg (1.1 MB)
Downloaded 2026-07-28/17_21_01.ulg in 1.5s (691.4 kB/s)
Uploaded 2026-07-28/17_21_01.ulg to local: http://127.0.0.1:5006/plot_app?log=95699be6-...
```

### Testing against PX4 SITL

SITL keeps its logs in `/log` relative to the working directory, which is in the probe list, so no configuration is needed beyond the port:

```
PX4_SIM_MODEL=none build/px4_sitl_default/bin/px4 build/px4_sitl_default/etc \
    -s build/px4_sitl_default/etc/init.d-posix/rcS -d
./build/logloader --config <config with connection_url = "udp://:14540">
```

### Install

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

### Future developments
- Resume interrupted transfers rather than restarting them.
- Multiple backends: e.g. RobotoAI, DroneLogbook, Auterion Suite, Aloft.
