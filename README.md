![image](logloader_logo.png)

Downloads ArduPilot DataFlash log files (.bin) and uploads them to a local server and optionally a remote server.

The **config.toml** file is used to configure the program settings.

### Behavior
logloader is split into two cooperating programs that coordinate through the `logs/` directory:
- **`logloader_download.py`** (pymavlink) downloads ArduPilot DataFlash `.bin` logs over MAVLink while the vehicle is disarmed, writing each completed file into `logs/`.
- **`logloader`** (C++) watches `logs/`, and uploads new logs to a local server and optionally a remote one.

An sqlite database per server tracks log upload status.

> Note: MAVSDK's `LogFiles` plugin does not work against ArduPilot (`LOG_REQUEST_LIST` times out), so the download side uses pymavlink and the C++ binary no longer depends on MAVSDK.

### Build
Install dependencies
```
sudo apt-get install libsqlite3-dev
pip install pymavlink
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
Start the download backend and the uploader (both read the same config):
```bash
python3 logloader_download.py        # downloads .bin logs into logs/
./build/logloader | tee output.txt   # uploads them
```

`logloader_download.py`:
```
Connecting to udpin:0.0.0.0:14551
Connected (system 1, component 1)
3 log(s) on vehicle
Downloading LOG0007 (1015808 bytes, 11287 chunks) -> LOG0007_2024-01-02T03:04:05Z.bin
  done: LOG0007_2024-01-02T03:04:05Z.bin
```

`logloader`:
```
Log upload SUCCESS: Success: 127.0.0.1:5006/browse?log=...
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
