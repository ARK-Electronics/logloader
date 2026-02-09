# Logloader

## Architecture

- `LogLoader` orchestrates download (MAVSDK) + upload (all backends)
- `UploadBackend` base class with SQLite DB tracking
- `FlightReviewBackend` extends UploadBackend - HTTP multipart upload to flight review servers
- `RobotoBackend` extends UploadBackend - RobotoAI API + S3 upload with SigV4 signing
- Config at `~/.local/share/logloader/config.toml`
- Local server is always Flight Review (installed with ARK OS)

## Build

- `make` in this directory (cmake under the hood)
- C++20, `-Wall -Wextra -Werror -Wpedantic`
- MAVSDK is NOT installed on dev machines - it's an embedded target dependency

## Pending Refactoring

The following issues were identified during code review and should be addressed:

### Bugs (fix first)

1. **Data race on `_should_exit` in UploadBackend** (`UploadBackend.hpp:69`): `_should_exit` is a plain `bool` written from the main thread via `stop()` and read from the upload thread. Make it `std::atomic<bool>`.

2. **Data race on `_loop_disabled` in LogLoader** (`LogLoader.hpp:65`): Same issue — plain `bool` accessed from multiple threads. Make it `std::atomic<bool>`.

3. **`upload_pending_logs` returns early instead of continuing** (`LogLoader.cpp:367-375`): If one log entry has an empty UUID or missing filepath, the function `return`s, which kills the entire upload queue for that backend. Change `return` to `continue`.

4. **Unsafe `getenv("HOME")`** (`main.cpp:21`): `getenv("HOME")` can return `nullptr` (e.g. systemd service). `std::string(nullptr)` is UB. Guard it.

5. **Hardcoded `substr(8)` in RobotoBackend** (`RobotoBackend.cpp:118` and throughout): Assumes URL starts with exactly `https://`. Extract the host at construction time, similar to `FlightReviewBackend::sanitize_url_and_determine_protocol()`.

6. **Missing space in log message** (`LogLoader.cpp:176`): `"Received " << size << "log entries"` — missing space before "log".

### Restructure: Separate download tracking from upload backends

**Problem:** `UploadBackend` currently owns download-related methods (`num_logs_to_download`, `get_next_log_to_download`, `update_download_status`, `add_log_entry`). Download is a single operation but is tracked redundantly in 3 separate databases. Every download-related call must be replicated across all backends:
```cpp
_local_server->add_log_entry(entry);
_remote_server->add_log_entry(entry);
if (_roboto_backend) _roboto_backend->add_log_entry(entry);
```
This pattern repeats in `request_log_entries` and `download_next_log`.

**Solution:** Move download tracking to `LogLoader` with a single `logs.db`. Each `UploadBackend` only tracks its own upload state. Remove `add_log_entry`, `update_download_status`, `num_logs_to_download`, `get_next_log_to_download` from `UploadBackend`.

### Restructure: Simplify `upload_log`

**Problem:** `UploadBackend::upload_log()` (`UploadBackend.cpp:177-252`) reverse-engineers the UUID from the filename by parsing out the id/date, reading file size from disk, fabricating a `mavsdk::LogFiles::Entry`, and hashing. This is fragile — if `fs::file_size()` differs from the original MAVSDK-reported `size_bytes`, the UUID won't match and the upload silently fails.

**Solution:** The caller (`upload_pending_logs`) already has the UUID. Pass it through, or restructure so `upload_log` receives the UUID directly. The filename-parsing logic can be removed entirely.

### Nice-to-have

- **No HTTP timeouts in FlightReviewBackend**: httplib clients have no timeout. A hanging server blocks the upload thread forever. Add `set_read_timeout()`.
- **New SSL client per API call in RobotoBackend**: 5 TLS handshakes per upload. Consider reusing a single client across the upload flow.
- **Redundant settings storage**: Both `_settings` and inherited `_base_settings` store `logs_directory`, `db_path`, `upload_enabled`. Pick one place.
