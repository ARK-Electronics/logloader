![image](logloader_logo.png)

Downloads PX4 ulogs from the flight controller SD card and optionally uploads them to a one or more servers.

The **config.toml** file is used to configure the program settings.

### Behavior
Downloading and uploading will only occur while the vehicle is not armed. Downloading and uploading operations are performed in separate threads. The downloading thread will only download logs with a datetime greater than the most recent log found locally in the `logging_directory`. If no logs are found locally only the most recent log will be downloaded. The upload thread will only upload logs that are not recorded in the `uploaded_logs_file`. Logs are named with the ISO 8601 date and time format **yyyy-mm-ddThh:mm:ssZ.ulg**.

### Build
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
| **Binary path**      | `~/.local/bin`                       |
| **Application directory** | `~/.local/share/logloader/`       |
| **Logs directory**   | `~/.local/share/logloader/logs/`       |
| **Config File**      | `~/.local/share/logloader/config.toml` |

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
