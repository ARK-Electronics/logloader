## Load ur logs
Downloads PX4 .ulg logs from the SD card and uploads to Flight Review

- Downloads the most recently recorded log if no logs are found locally.
- Downloads logs with a datetime greater than the greatest log in the logs/ directory.
- Uploads logs to PX4 Flight Review that have not been recorded in the **uploaded_logs.txt** file.

### Build
```
make
```

### Run
```bash
./build/logloader --email <your@email.com>
```