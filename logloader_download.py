#!/usr/bin/env python3
"""Download ArduPilot DataFlash (.bin) logs over MAVLink using pymavlink.

This is the download backend for logloader. MAVSDK's LogFiles plugin does not work
against ArduPilot (LOG_REQUEST_LIST times out with no LOG_ENTRY reply), so the
download side is implemented here with pymavlink while the C++ binary keeps owning
the upload side.

Coordination is purely through the filesystem: this script writes completed logs
into the same logs/ directory the C++ uploader watches. Files are downloaded to a
"<name>.part" temp file and atomically renamed to "<name>.bin" only once complete,
so the uploader never sees a partial file. Naming matches what the uploader expects:
    LOG<id:04d>_<date>.bin            e.g. LOG0007_2024-01-02T03:04:05Z.bin
The uploader derives its dedup UUID from this filename, so the id and date must be
embedded here exactly as written.

Only one process may speak MAVLink on the connection, so the C++ side no longer
connects; this script is the sole MAVLink client.
"""

import argparse
import glob
import math
import os
import signal
import sys
import time
import tomli
from datetime import datetime, timezone

from pymavlink import mavutil

CHUNK = 90  # LOG_DATA payload size in bytes

_should_exit = False


def _handle_signal(signum, frame):
    global _should_exit
    _should_exit = True
    print("\nReceived signal, finishing up...", flush=True)


def log(msg):
    print(msg, flush=True)


def resolve_config_path(cli_path):
    """Mirror the C++ config lookup: --config wins, else user override, else deb default."""
    if cli_path:
        return cli_path
    home = os.environ.get("HOME", "/tmp")
    user = os.path.join(home, ".config/ark/logloader/config.toml")
    default = "/opt/ark/share/logloader/config.toml"
    return user if os.path.exists(user) else default


def logs_directory(config):
    """Mirror main.cpp / LogLoader: <application_directory>/logs/."""
    home = os.environ.get("HOME", "/tmp")
    default_app_dir = os.path.join(home, ".local/share/ark/logloader") + "/"
    app_dir = config.get("application_directory", default_app_dir)
    if not app_dir.endswith("/"):
        app_dir += "/"
    return app_dir + "logs/"


def to_pymavlink_url(url):
    """Translate the config connection_url into a pymavlink connection string.

    MAVSDK-style 'udp://:14551' means "listen on 14551" -> pymavlink 'udpin:0.0.0.0:14551'.
    'udp://host:port' means "send to host" -> 'udpout:host:port'. Already-native
    pymavlink forms and serial/tcp are passed through.
    """
    native_prefixes = ("udpin:", "udpout:", "udpbcast:", "tcp:", "tcpin:", "serial:", "/dev/")
    if url.startswith(native_prefixes):
        return url
    if url.startswith("udp://"):
        rest = url[len("udp://"):]
        host, _, port = rest.rpartition(":")
        if not port:
            return url  # malformed; let pymavlink raise a clear error
        return f"udpin:0.0.0.0:{port}" if host in ("", "0.0.0.0", "*") else f"udpout:{host}:{port}"
    if url.startswith("tcp://"):
        return "tcp:" + url[len("tcp://"):]
    return url


def format_date(time_utc):
    """ISO8601 UTC from LOG_ENTRY.time_utc; 'unknown' when the vehicle has no GPS time."""
    if time_utc and time_utc > 0:
        return datetime.fromtimestamp(time_utc, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return "unknown"


def send_gcs_heartbeat(master):
    """Announce ourselves as a GCS. ArduPilot / mavlink-router only route replies
    (LOG_ENTRY, LOG_DATA, command acks) back to endpoints that emit heartbeats."""
    master.mav.heartbeat_send(
        mavutil.mavlink.MAV_TYPE_GCS,
        mavutil.mavlink.MAV_AUTOPILOT_INVALID,
        0, 0, mavutil.mavlink.MAV_STATE_ACTIVE)


def wait_autopilot(master, timeout=30):
    """Lock target_system/target_component onto the actual autopilot.

    wait_heartbeat() would latch onto the first heartbeat from *any* component
    (a gimbal, a companion, another GCS), and requests sent there are ignored.
    Autopilots advertise a real autopilot type (not MAV_AUTOPILOT_INVALID)."""
    deadline = time.time() + timeout
    next_hb = 0.0

    while time.time() < deadline and not _should_exit:
        now = time.time()
        if now >= next_hb:
            send_gcs_heartbeat(master)
            next_hb = now + 1.0

        msg = master.recv_match(type="HEARTBEAT", blocking=True, timeout=1)
        if msg is None:
            continue
        if msg.autopilot == mavutil.mavlink.MAV_AUTOPILOT_INVALID:
            continue  # a GCS/peripheral, not the autopilot

        master.target_system = msg.get_srcSystem()
        master.target_component = msg.get_srcComponent()
        return msg

    return None


def request_log_list(master, timeout=15):
    """Return a list of LOG_ENTRY messages for all logs on the vehicle."""
    master.mav.log_request_list_send(master.target_system, master.target_component, 0, 0xFFFF)

    entries = {}
    num_logs = None
    deadline = time.time() + timeout
    last_request = 0.0
    next_hb = 0.0

    while time.time() < deadline and not _should_exit:
        now = time.time()
        # Keep the GCS heartbeat alive and periodically re-request so a single dropped
        # UDP packet doesn't stall the whole pass.
        if now >= next_hb:
            send_gcs_heartbeat(master)
            next_hb = now + 1.0
        if now - last_request > 2.0 and (num_logs is None or len(entries) < num_logs):
            master.mav.log_request_list_send(master.target_system, master.target_component, 0, 0xFFFF)
            last_request = now

        msg = master.recv_match(type="LOG_ENTRY", blocking=True, timeout=0.5)
        if msg is None:
            continue

        num_logs = msg.num_logs
        if num_logs == 0:
            return []  # vehicle reports no logs

        # id 0 with a nonzero num_logs is just the summary entry on some stacks; keep real ids.
        if msg.id > 0 or msg.size > 0:
            entries[msg.id] = msg

        if len(entries) >= num_logs:
            break

    if num_logs is None:
        log("  no LOG_ENTRY reply (vehicle is not answering log requests on this link)")
    elif len(entries) < num_logs:
        log(f"  incomplete log list: {len(entries)}/{num_logs} entries (will retry next pass)")

    return [entries[k] for k in sorted(entries)]


def download_one(master, entry, logs_dir, rounds=20):
    """Download a single log to logs_dir, atomically. Returns True on success/skip."""
    log_id = entry.id
    size = entry.size

    if size == 0:
        log(f"Skipping LOG{log_id:04d}: zero size")
        return True

    # Skip if already present (match on id prefix; date is stable per log).
    if glob.glob(os.path.join(logs_dir, f"LOG{log_id:04d}_*.bin")):
        return True

    date = format_date(entry.time_utc)
    final_path = os.path.join(logs_dir, f"LOG{log_id:04d}_{date}.bin")
    part_path = final_path + ".part"

    num_chunks = math.ceil(size / CHUNK)
    log(f"Downloading LOG{log_id:04d} ({size} bytes, {num_chunks} chunks) -> {os.path.basename(final_path)}")

    have = set()  # offsets received

    with open(part_path, "wb+") as f:
        f.truncate(size)

        for _round in range(rounds):
            if _should_exit or master.motors_armed():
                break

            # Request the whole log on the first round, then only the missing chunks.
            missing = [i * CHUNK for i in range(num_chunks) if i * CHUNK not in have]
            if not missing:
                break

            if _round == 0:
                master.mav.log_request_data_send(master.target_system, master.target_component, log_id, 0, 0xFFFFFFFF)
            else:
                log(f"  re-requesting {len(missing)} missing chunk(s)")
                for ofs in missing:
                    count = min(CHUNK, size - ofs)
                    master.mav.log_request_data_send(master.target_system, master.target_component, log_id, ofs, count)

            # Drain incoming LOG_DATA until the link goes quiet.
            idle_deadline = time.time() + 3.0
            next_hb = 0.0
            while time.time() < idle_deadline and not _should_exit:
                now = time.time()
                if now >= next_hb:
                    send_gcs_heartbeat(master)  # keep the link routing to us during long downloads
                    next_hb = now + 1.0
                msg = master.recv_match(type="LOG_DATA", blocking=True, timeout=1)
                if msg is None:
                    continue
                if msg.id != log_id or msg.ofs in have:
                    continue
                count = min(msg.count, size - msg.ofs)
                if count <= 0:
                    continue
                f.seek(msg.ofs)
                f.write(bytes(msg.data[:count]))
                have.add(msg.ofs)
                idle_deadline = time.time() + 3.0
                if len(have) >= num_chunks:
                    break

    complete = len(have) >= num_chunks
    if complete:
        os.replace(part_path, final_path)  # atomic; uploader only ever sees the finished file
        log(f"  done: {os.path.basename(final_path)}")
        return True

    # Incomplete: drop the partial so the next pass retries cleanly.
    try:
        os.remove(part_path)
    except OSError:
        pass
    log(f"  incomplete ({len(have)}/{num_chunks} chunks), will retry next pass")
    return False


def connect(config):
    conn_url = to_pymavlink_url(config.get("connection_url", "udp://:14551"))
    # Identify as a real GCS. The pymavlink default source_component is 0
    # (MAV_COMP_ID_ALL), which is a broadcast/target address, not a valid sender —
    # ArduPilot and mavlink-router can ignore requests sent from it. 255/190 mirrors
    # what MAVProxy uses. Both are overridable in config.toml.
    source_system = int(config.get("source_system", 255))
    source_component = int(config.get("source_component", mavutil.mavlink.MAV_COMP_ID_MISSIONPLANNER))

    log(f"Connecting to {conn_url} as system {source_system}, component {source_component}")
    master = mavutil.mavlink_connection(
        conn_url, dialect="ardupilotmega",
        source_system=source_system, source_component=source_component)

    hb = wait_autopilot(master)
    if hb is None:
        log("No autopilot heartbeat received. Check the connection and that the vehicle is on the link.")
        return None

    log(f"Connected to autopilot (system {master.target_system}, component {master.target_component}, "
        f"autopilot={hb.autopilot}, type={hb.type})")
    return master


def probe(master, seconds):
    """Diagnostic: send a GCS heartbeat + LOG_REQUEST_LIST and report which message
    types arrive. Tells you definitively whether the vehicle answers log requests."""
    log(f"Probing for {seconds}s (sys {master.target_system}, comp {master.target_component})...")
    counts = {}
    deadline = time.time() + seconds
    next_hb = 0.0
    requested = False

    while time.time() < deadline and not _should_exit:
        now = time.time()
        if now >= next_hb:
            send_gcs_heartbeat(master)
            master.mav.log_request_list_send(master.target_system, master.target_component, 0, 0xFFFF)
            requested = True
            next_hb = now + 1.0

        msg = master.recv_match(blocking=True, timeout=0.5)
        if msg:
            counts[msg.get_type()] = counts.get(msg.get_type(), 0) + 1

    log("Message types received:")
    for t in sorted(counts):
        log(f"  {t}: {counts[t]}")
    if requested and "LOG_ENTRY" not in counts:
        log("** No LOG_ENTRY received -> the vehicle is NOT answering log requests on this link.")
        log("   Likely causes: requests not reaching the autopilot (one-way link / wrong port),")
        log("   another GCS owns the link, or LOG_BACKEND_TYPE has no MAVLink-readable logs.")
    elif "LOG_ENTRY" in counts:
        log("** LOG_ENTRY received -> log download works on this link.")


def run(config, logs_dir, poll_interval, once):
    master = connect(config)
    if master is None:
        return

    was_armed = None

    while not _should_exit:
        master.wait_heartbeat(timeout=5)

        armed = bool(master.motors_armed())
        if armed != was_armed:
            log("Vehicle armed - pausing downloads until disarmed" if armed else "Vehicle disarmed - checking for logs")
            was_armed = armed
        if armed:
            time.sleep(2)
            continue

        log("Requesting log list...")
        entries = request_log_list(master)
        if entries:
            log(f"{len(entries)} log(s) on vehicle")
            for entry in entries:
                if _should_exit or master.motors_armed():
                    break
                try:
                    download_one(master, entry, logs_dir)
                except Exception as exc:  # noqa: BLE001 - keep the daemon alive across one bad log
                    log(f"Error downloading LOG{entry.id:04d}: {exc}")

        # Tell the vehicle we're done with the log protocol for this pass.
        master.mav.log_request_end_send(master.target_system, master.target_component)

        if once:
            break

        slept = 0
        while slept < poll_interval and not _should_exit:
            time.sleep(1)
            slept += 1

    log("Exiting.")


def main():
    parser = argparse.ArgumentParser(description="Download ArduPilot DataFlash logs over MAVLink.")
    parser.add_argument("--config", help="Path to config.toml (overrides default lookup)")
    parser.add_argument("--once", action="store_true", help="Run a single download pass and exit")
    parser.add_argument("--poll-interval", type=int, default=30, help="Seconds between download passes")
    parser.add_argument("--probe", type=int, metavar="SECONDS",
                        help="Diagnostic: connect, request the log list, and report which "
                             "MAVLink message types arrive (does not download)")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    config_path = resolve_config_path(args.config)
    try:
        with open(config_path, "rb") as f:
            config = tomli.load(f)
    except (OSError, tomli.TOMLDecodeError) as exc:
        log(f"Failed to read config {config_path}: {exc}")
        return 1

    if args.probe:
        master = connect(config)
        if master is None:
            return 1
        probe(master, args.probe)
        return 0

    logs_dir = logs_directory(config)
    os.makedirs(logs_dir, exist_ok=True)
    log(f"Logs directory: {logs_dir}")

    run(config, logs_dir, args.poll_interval, args.once)
    return 0


if __name__ == "__main__":
    sys.exit(main())
