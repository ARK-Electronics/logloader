## Hardware test results (Pi + PX4 / ARK Pi6X)

Tested branch `claude/logdata-mavlink-targeting-uj8d0y` including `7a23c8e` (`fix(upload): stop spamming when flight-review is offline`) on a live companion:

| Item | Value |
|------|--------|
| Host | Raspberry Pi (ark-os), aarch64 |
| Autopilot | PX4 via `mavlink-router` UART @ 2 Mbaud |
| Link | `udp://:14551` (logloader endpoint) |
| Binary | Built from this branch with MAVSDK 3.17.1, `DEBUG_BUILD=ON` |

### Connect / index

```text
Connected.
MAVLink FTP available, PX4 logs in /fs/microsd/log (68 files)
Received 68 log entries in ~0.26s
```

After FC reboot this was reliable. Earlier mid-session hard-kills of logloader left the FC with `FTP list … File Does Not Exist` / `0 log entries` until reboot.

### FTP download (success)

| Log | Protocol | Result |
|-----|----------|--------|
| `LOG0058` (~40 MB) | MAVLink FTP (burst) | **Finished in 94s** — on-disk file is valid ULog (`ULog\x01…`) |
| Path | `/fs/microsd/log/2025-02-05/20_39_10.ulg` | Mapped and transferred |

### Path mapping miss → auto fallback (success)

For a small entry (`LOG0020`, LOG_ENTRY size **259471**):

```text
Downloading …/LOG0020_….ulg from /fs/microsd/log/2024-06-18/16_06_57.ulg
Size mismatch …: got 276495 bytes, expected 259471
FTP download failed
Downloading …/LOG0020_….ulg          # LOG_DATA fallback (download_protocol=auto)
Finished in 0.55 seconds
```

Final file size **259471**, valid ULog magic. So **auto** correctly falls back when FTP path resolution picks the wrong remote file.

### Large-log FTP timeouts

Newest/largest queue head (~100 MB, `LOG0067`) often hits:

```text
FTP download of …/00_02_04.ulg failed: Timeout
```

Burst mode makes progress (multi‑Mbps) but MAVSDK can still time out on very large files in short runs. Non-burst was ~800 kbps. Worth a follow-up (timeouts / resume / queue order), not a blocker for basic FTP path.

### Upload spam fix (`7a23c8e`)

With **flight-review not running** (`:5006` down) and multiple pending local uploads:

| Metric | Count |
|--------|------:|
| `Upload server … unreachable; skipping uploads for 60s` | **1** |
| `TEMPORARILY FAILED` (old spam) | **0** |
| Per-log `Connection to … failed` | **0** |

### Not tested here

- ArduPilot / `.BIN` / `LogEntryLister` path  
- Live upload to a running local flight-review (only “server down” path)  
- Full multi-hour campaign of all 68 logs over FTP only  

### Verdict

**Works on PX4 hardware after a healthy FC boot:** connect, FTP detect/index, successful multi‑MB FTP download, auto→LOG_DATA fallback on size mismatch, and no upload spam when flight-review is offline.
