#!/usr/bin/env python3
"""
Flask Serial Server for Pillex
- MVP (HTTP / HTTPS) contrlls serial device on /dev/serial0 @ 115200.
- Serves static files from ./static and maps / to /static/index.html
- Endpoints:
  See documentation: API.md

Data model (demo): SQLite for users; in-memory state for device/cartridge/prescription/schedule runtime.
Security: passwords hashed; session cookie.
Serial: background reader parses lines by newline and calls on_serial_line(line).
Scheduling: background tick checks for DUE/ACTIVE windows.
    - on start -> send_serial('SCREEN TAKE_PILL')
    - on missed -> send_serial(f"msg Dose missed! {time}")
    - on end of last active -> send_serial('SCREEN TIME')

By: Pawel Toborek (20 Aug 2025)
"""

from __future__ import annotations
import os
import json
import sqlite3
import threading
import time
from datetime import datetime, timedelta, date, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from flask import Flask, request, jsonify, session, send_from_directory, abort
from werkzeug.security import generate_password_hash, check_password_hash

# ---------------------------- Optional sound effects ----------------------------
# Leave imports and error handling as-is; gracefully degrade if not available.
try:
    import pygame
except Exception:  # pragma: no cover
    pygame = None  # type: ignore

sound_btn_short = None
sound_error = None
sound_success = None
sound_alarm = None

try:
    if pygame is not None:
        pygame.mixer.init()
        sound_btn_short = pygame.mixer.Sound("sounds/pop.wav"); sound_btn_short.set_volume(0.25)
        sound_error  = pygame.mixer.Sound("sounds/wpn_select.wav"); sound_error.set_volume(0.5)
        sound_success   = pygame.mixer.Sound("sounds/done.wav"); sound_success.set_volume(0.1)
        sound_alarm     = pygame.mixer.Sound("sounds/alarm1.wav"); sound_alarm.set_volume(0.25)
        sound_success.play()
except Exception as e:  # pragma: no cover
    sound_btn_short = sound_error = sound_success = sound_alarm = None
    print(f"Error loading sounds: {e}")

# Attempt to import pyserial; gracefully degrade if not available.
try:
    import serial  # type: ignore
except Exception:  # pragma: no cover
    serial = None  # type: ignore

APP_ROOT = Path(__file__).parent.resolve()
DB_PATH = APP_ROOT / "demo.db"
CARTRIDGES_DIR = APP_ROOT / "cartridges"
LOGS_MAX_LINES = 2000

# ---------------------------- Flask app ----------------------------
app = Flask(__name__, static_folder=str(APP_ROOT / "static"), static_url_path="/static")
app.config.update(SECRET_KEY=os.environ.get("FLASK_SECRET", "dev-secret-key"))

# ---------------------------- In-memory runtime state ----------------------------
state_lock = threading.RLock()

serial_port = "/dev/serial0"
serial_baud = 115200
ser: Optional["serial.Serial"] = None

serial_logs: List[str] = []  # rolling memory log

default_mem_size = 256
memory = bytearray(default_mem_size)
cartridge_id = "0000000000000000"
cartridge_id_old = "0000000000000000"
cartridge_data: Dict[str, Any] = {}
events: List[Dict[str, Any]] = []

# Track whether we're currently showing TAKE_PILL screen to avoid spamming the device
showing_take_pill = False

# ---------------------------- Utilities ----------------------------

def log_line(s: str) -> None:
    with state_lock:
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        serial_logs.append(f"[{ts}] {s}")
        # Trim rolling log without rebinding the list
        if len(serial_logs) > LOGS_MAX_LINES:
            del serial_logs[: len(serial_logs) - LOGS_MAX_LINES]


def now_local() -> datetime:
    return datetime.now()


def iso_date(d: date) -> str:
    return d.isoformat()

# ---------------------------- Serial I/O ----------------------------

def init_serial() -> None:
    global ser
    if serial is None:
        log_line("pyserial not available; running in no-serial mode")
        return
    try:
        ser = serial.Serial(serial_port, serial_baud, timeout=0.1)  # non-blocking
        log_line(f"Opened serial {serial_port} @ {serial_baud}")
    except Exception as e:  # pragma: no cover
        ser = None
        log_line(f"Failed to open serial: {e}")


def send_serial(cmd: str) -> None:
    """Send a line to the serial device, appending a newline."""
    line = (cmd.rstrip("\r\n") + "\n").encode("utf-8")
    if ser is not None and getattr(ser, "write", None):
        try:
            ser.write(line)
            log_line(f"TX: {cmd}")
            # print(f"Sending Serial: {cmd}")
        except Exception as e:  # pragma: no cover
            log_line(f"Serial write error: {e}; cmd={cmd!r}")
    else:
        # simulate in logs for demo
        log_line(f"TX (sim): {cmd}")


def parse_mem_line(line: str) -> None:
    """Parse lines like: $MEM 0x10: FF FF ... and update memory + cartridge id."""
    global cartridge_id, cartridge_id_old
    try:
        parts = line.split()
        # Expect: $MEM 0x10: FF FF ...
        address_str = parts[1].rstrip(':')
        start_addr = int(address_str, 16)
        hex_bytes = parts[2:]
        for i, hb in enumerate(hex_bytes):
            idx = start_addr + i
            if 0 <= idx < len(memory):
                memory[idx] = int(hb, 16)
    except Exception as e:
        print(f"⚠️ Error parsing data: {line} -> {e}")
    # First 8 bytes of memory contain the cartridge ID
    new_cid = memory[:8].hex().upper()
    if new_cid != cartridge_id:
        cartridge_id = new_cid
        if cartridge_id != cartridge_id_old:
            cartridge_id_old = cartridge_id
            print(f"Loading cartridge {cartridge_id}!")
            load_cartridge_json(cartridge_id)


def process_serial_command(data: str) -> None:
    data = data.strip()
    if not data or not data.startswith('$'):
        return
    # print(f"Serial received : {data}")
    if data.startswith("$MEM"):
        parse_mem_line(data)
        return
    if data.startswith("$screen"):
        screen = data.split(' ')[1] if ' ' in data else ''
        print(f"Screen updated to {screen}")
    if "Button" in data:
        if sound_alarm:
            try:
                sound_alarm.stop()
            except Exception:
                pass
        if sound_btn_short:
            try:
                sound_btn_short.play()
            except Exception:
                pass
        if "OK" in data:
            # Attempt to dispense pills for the currently ACTIVE schedule entry
            try:
                handle_ok_button_dispense()
            except Exception as e:  # pragma: no cover
                log_line(f"Error during OK dispense: {e}")
        elif "BACK long press" in data:
            send_serial('mem -r 0 f')
    if "drp ok" in data:
        if sound_success:
            try:
                sound_success.play()
            except Exception:
                pass
    if "drp fail" in data:
        if sound_error:
            try:
                sound_error.play()
            except Exception:
                pass
    if "screen ERROR" in data:
        if sound_error:
            try:
                sound_error.play()
            except Exception:
                pass
    if "init" in data:
        log_line(f"WARNING: Microcontroller reset, pooling cartridge ID...")
        send_serial("mem -r 0 f")


def serial_reader_thread() -> None:
    """Continuously read from serial, process complete lines when a newline is seen."""
    buf = b""
    while True:
        try:
            if ser is not None and getattr(ser, "read", None):
                chunk = ser.read(128)
            else:
                # In demo/no-serial mode, sleep a bit and continue
                time.sleep(0.25)
                continue
            if not chunk:
                time.sleep(0.01)
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    s = line.decode("utf-8", errors="replace").rstrip("\r")
                except Exception:  # pragma: no cover
                    s = repr(line)
                process_serial_command(s)
                log_line(f"RX: {s}")
        except Exception as e:  # pragma: no cover
            log_line(f"Serial read error: {e}")
            time.sleep(0.5)


def serial_heartbeat_thread() -> None:
    while True:
        try:
            now = datetime.now()
            send_serial(f"time {now:%H} {now:%M} {now:%S}")
        except Exception as e:  # pragma: no cover
            log_line(f"Heartbeat error: {e}")
        time.sleep(10)  # every 10s


# ---------------------------- Scheduling ----------------------------

def parse_hhmmss(s: str) -> timedelta:
    h, m, sec = map(int, s.split(":"))
    return timedelta(hours=h, minutes=m, seconds=sec)


def current_cycle_day(start_date: date, repeat_days: int) -> int:
    """Return the 1-based day number within the current cycle."""
    delta = (date.today() - start_date).days
    day_in_cycle = (delta % max(1, repeat_days)) + 1
    return day_in_cycle


def within_window(now: datetime, t_start: datetime, t_end: datetime) -> int:
    """Return -1 if before, 0 if in window, +1 if after (for a given day)."""
    if now < t_start:
        return -1
    if now > t_end:
        return 1
    return 0


def emit_event(kind: str, payload: Optional[Dict[str, Any]] = None) -> None:
    with state_lock:
        events.append({
            "ts": datetime.now().isoformat(timespec="seconds"),
            "kind": kind,
            "payload": payload or {},
        })
        # Keep events reasonable; avoid rebinding the list
        if len(events) > 2000:
            del events[: len(events) - 1000]


def _parse_iso_utc(dt_str: str) -> datetime:
    # Expecting Z suffix (UTC)
    if dt_str.endswith('Z'):
        dt_str = dt_str[:-1] + '+00:00'
    return datetime.fromisoformat(dt_str).astimezone(timezone.utc)


def _get_schedule_list() -> List[Dict[str, Any]]:
    return list(cartridge_data.get("schedule", []))


def _set_schedule_list(new_list: List[Dict[str, Any]]) -> None:
    cartridge_data["schedule"] = new_list


def _first_active_entry(now_utc: datetime) -> Optional[Dict[str, Any]]:
    for entry in cartridge_data.get("schedule", []):
        try:
            start = _parse_iso_utc(entry["datetime_start"])  # type: ignore
            end = _parse_iso_utc(entry["datetime_end"])      # type: ignore
        except Exception:
            continue
        status = entry.get("status")
        if status == "ACTIVE" and start <= now_utc <= end:
            return entry
    return None


def handle_ok_button_dispense() -> None:
    now_utc = datetime.now(timezone.utc)
    with state_lock:
        entry = _first_active_entry(now_utc)
        if not entry:
            # emit_event("no_active_dose_on_ok", {})
            return
        mods = entry.get("cmd") or {}
        # Accept both int and str values
        m1 = int(mods.get("mod1", 0) or 0)
        m2 = int(mods.get("mod2", 0) or 0)
        m3 = int(mods.get("mod3", 0) or 0)
        m4 = int(mods.get("mod4", 0) or 0)
        user_dispense_pills(m1, m2, m3, m4)
        # Mark taken
        entry["status"] = "TAKEN"
        entry["taken_at"] = now_utc.replace(microsecond=0).isoformat().replace("+00:00", "Z")
        emit_event("dose_taken", {"entry": {k: entry.get(k) for k in ("day", "datetime_start", "datetime_end", "hash")}})
        # After dispensing, go back to TIME screen
        # send_serial('screen TIME')
        global showing_take_pill
        showing_take_pill = False


def schedule_tick() -> None:
    """
    Check schedule windows and trigger start/end actions.
    - Transition DUE -> ACTIVE when window opens (send TAKE_PILL)
    - Transition ACTIVE -> MISSED when window ends and not taken (send missed message)
    - When no ACTIVE remains and we were showing TAKE_PILL, send screen TIME once
    """
    global showing_take_pill

    with state_lock:
        sched = _get_schedule_list()
        if not sched:
            return
        now_utc = datetime.now(timezone.utc)
        any_active_now = False
        for entry in sched:
            try:
                start = _parse_iso_utc(entry["datetime_start"])  # type: ignore
                end = _parse_iso_utc(entry["datetime_end"])      # type: ignore
            except Exception:
                continue
            status = entry.get("status") or "DUE"

            # If already taken, skip
            if status == "TAKEN":
                continue

            pos = within_window(now_utc, start, end)

            # Entering window
            if status in ("DUE", "UPCOMING") and pos == 0:
                entry["status"] = "ACTIVE"
                emit_event("dose_window_started", {"start": entry["datetime_start"], "end": entry["datetime_end"]})
                if not showing_take_pill:
                    send_serial('screen TAKE_PILL')  # As per top docstring
                    # Optionally play alarm once when a window starts
                    if sound_alarm:
                        try:
                            sound_alarm.play(loops=-1)
                        except Exception:
                            pass
                    showing_take_pill = True
                any_active_now = True
                continue

            # Still in window
            if entry.get("status") == "ACTIVE" and pos == 0:
                any_active_now = True
                continue

            # Window ended without being taken
            if entry.get("status") == "ACTIVE" and pos == 1:
                entry["status"] = "MISSED"
                emit_event("dose_missed", {"end": entry["datetime_end"]})
                send_serial(f"msg Dose missed! {end.astimezone().strftime('%H:%M')}")
                # do not set showing_take_pill here; may still be other actives
                continue

            # Future window
            # Leave status as-is (DUE/UPCOMING)

        # If no ACTIVE windows remain and we were showing TAKE_PILL, return to TIME once
        if not any_active_now and showing_take_pill:
            send_serial('SCREEN TIME')
            showing_take_pill = False


# ---------------------------- Cartridges & dispensing ----------------------------

def load_cartridge_json(cid: str) -> Dict[str, Any]:
    """Load cartridge JSON by ID from cartridges/{id}.json; fall back to default.
    The file should contain the exact structure like DEFAULT_CARTRIDGE_JSON.
    """
    global cartridge_data
    path = CARTRIDGES_DIR / f"{cid}.json"
    if path.exists():
        with path.open("r", encoding="utf-8") as f:
            try:
                raw = json.load(f)
            except json.JSONDecodeError as e:  # pragma: no cover
                print(f"Error parsing cartridge JSON: {e}")
                return {}
            cartridge_data = unpack_cartridge(raw) or raw
            print(f"Cartridge {cid} loaded!")
    else:
        print(f"Error: Path does not exist: {path}")
    return cartridge_data


def unpack_cartridge(data: Dict[str, Any], start_offset: int = 0) -> Optional[Dict[str, Any]]:
    # TODO: add start_offset functionality
    """
    - Expands base schedule to prescription['schedule_repeat_days'] days (pattern repeats).
    - Builds datetime_start/end (UTC ISO8601 with 'Z') from local times.
    - Marks upcoming entries as DUE, active windows as ACTIVE, expired as MISSED.
    """
    info = (data.get("info") or {})
    version = (info.get("version") or "").strip()
    if version != "v0.2":
        print(f"Error, incompatible cartridge version! Detected: {version}, required v0.2")
        return None

    # Do not early-return if activated; we may still want to (re)expand
    # isUnpacked = bool(info.get("activated"))
    # if isUnpacked:
    #     print("Cartridge data is already unpacked!")
    #     return data

    prescription = data.get("prescription", {}) or {}
    base_schedule = data.get("schedule", []) or []

    repeat_days = int(prescription.get("schedule_repeat_days") or 0)
    if repeat_days <= 0:
        repeat_days = max((int(e.get("day", 1) or 1) for e in base_schedule), default=1)

    start_day_str = prescription.get("start_day")
    if start_day_str:
        start_day_local = datetime.strptime(start_day_str, "%Y-%m-%d")
    else:
        now_l = datetime.now().astimezone()
        start_day_local = now_l.replace(hour=0, minute=0, second=0, microsecond=0).replace(tzinfo=None)

    local_tz = datetime.now().astimezone().tzinfo
    now_utc = datetime.now(timezone.utc)

    if base_schedule:
        pattern_length = max(int(e.get("day", 1) or 1) for e in base_schedule)
    else:
        pattern_length = 1

    expanded: List[Dict[str, Any]] = []
    for abs_day in range(1, repeat_days + 1):
        pattern_day = ((abs_day - 1) % pattern_length) + 1
        for tpl in base_schedule:
            if int(tpl.get("day", 1) or 1) != pattern_day:
                continue
            entry = dict(tpl)
            entry["day"] = abs_day

            day_offset = abs_day - 1
            entry_date_local = start_day_local + timedelta(days=day_offset)

            t_start = entry.get("time_start", "00:00:00")
            t_end = entry.get("time_end", "23:59:59")

            dt_start_local = datetime.strptime(
                f"{entry_date_local.strftime('%Y-%m-%d')} {t_start}", "%Y-%m-%d %H:%M:%S"
            ).replace(tzinfo=local_tz)
            dt_end_local = datetime.strptime(
                f"{entry_date_local.strftime('%Y-%m-%d')} {t_end}", "%Y-%m-%d %H:%M:%S"
            ).replace(tzinfo=local_tz)

            dt_start_utc = dt_start_local.astimezone(timezone.utc)
            dt_end_utc = dt_end_local.astimezone(timezone.utc)

            entry["datetime_start"] = dt_start_utc.replace(microsecond=0).isoformat().replace("+00:00", "Z")
            entry["datetime_end"] = dt_end_utc.replace(microsecond=0).isoformat().replace("+00:00", "Z")

            # Status
            if now_utc < dt_start_utc:
                entry["status"] = "DUE"
            elif dt_start_utc <= now_utc <= dt_end_utc:
                entry["status"] = "DUE" #will get ACTIVE in scheduler
            else:
                entry["status"] = "MISSED"

            # Ensure hash exists
            if entry.get("hash") in (None, "", 0):  # hash might be missing or null in json
                entry["hash"] = os.urandom(8).hex()

            expanded.append(entry)

    data["schedule"] = expanded
    data["prescription"] = prescription
    # Do not set activated here; caller may do it after successful activation
    return data


def user_dispense_pills(mod1: int, mod2: int, mod3: int, mod4: int) -> None:
    """Called to dispense pills and update quantities.
    Builds a drpall command and decrements cartridge quantities (min 0).
    """
    # Normalize to ints (protect against None)
    mod1 = int(mod1 or 0); mod2 = int(mod2 or 0); mod3 = int(mod3 or 0); mod4 = int(mod4 or 0)

    send_serial(f"drpall {mod1} {mod2} {mod3} {mod4}")

    with state_lock:
        cart = cartridge_data.get("cartridge", {})
        for i, val in enumerate((mod1, mod2, mod3, mod4), start=1):
            mod_key = f"mod{i}"
            if mod_key in cart and isinstance(cart[mod_key], dict):
                qty = int(cart[mod_key].get("quantity", 0) or 0)
                cart[mod_key]["quantity"] = max(0, qty - int(val))
        # save back
        cartridge_data["cartridge"] = cart
        emit_event("pills_dispensed", {"cmd": f"drpall {mod1} {mod2} {mod3} {mod4}"})


# ---------------------------- Database (users) ----------------------------

def db_init() -> None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                username TEXT NOT NULL,
                email TEXT UNIQUE NOT NULL,
                password_hash TEXT NOT NULL,
                is_admin INTEGER NOT NULL DEFAULT 0,
                account_created TEXT NOT NULL,
                last_login TEXT
            )
            """
        )
        conn.commit()


def db_create_user(username: str, email: str, password: str, is_admin: int = 0) -> int:
    ph = generate_password_hash(password)
    now = datetime.utcnow().isoformat()
    with sqlite3.connect(DB_PATH) as conn:
        cur = conn.execute(
            "INSERT INTO users(username, email, password_hash, is_admin, account_created) VALUES (?,?,?,?,?)",
            (username, email, ph, is_admin, now),
        )
        conn.commit()
        return int(cur.lastrowid)


def db_find_user_by_email(email: str) -> Optional[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        cur = conn.execute("SELECT * FROM users WHERE email = ?", (email,))
        row = cur.fetchone()
        return dict(row) if row else None


def db_find_user_by_id(uid: int) -> Optional[dict]:
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row
        cur = conn.execute("SELECT * FROM users WHERE id = ?", (uid,))
        row = cur.fetchone()
        return dict(row) if row else None


def db_update_last_login(uid: int) -> None:
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute("UPDATE users SET last_login = ? WHERE id = ?", (datetime.utcnow().isoformat(), uid))
        conn.commit()


# ---------------------------- Auth helpers ----------------------------

def require_login() -> dict:
    uid = session.get("user_id")
    if not uid:
        abort(401)
    user = db_find_user_by_id(int(uid))
    if not user:
        abort(401)
    return user


def require_admin() -> dict:
    user = require_login()
    if int(user.get("is_admin", 0)) != 1:
        abort(403)
    return user


# ---------------------------- Routes ----------------------------
@app.route("/")
def root_index():
    # Map root to /static/index.html
    return send_from_directory(app.static_folder, "index.html")


@app.route("/api/register", methods=["POST"])
def register():
    require_admin()
    data = request.get_json(force=True)
    username = (data.get("username") or "").strip() or data.get("email")
    email = (data.get("email") or "").strip().lower()
    password = data.get("password") or ""
    if not email or not password:
        return jsonify({"error": "email and password required"}), 400
    try:
        uid = db_create_user(username=username, email=email, password=password, is_admin=int(data.get("is_admin", 0)))
    except sqlite3.IntegrityError:
        return jsonify({"error": "email already registered"}), 409
    return jsonify({"ok": True, "user_id": uid})


@app.route("/api/login", methods=["POST"])
def login():
    data = request.get_json(force=True)
    email = (data.get("email") or "").strip().lower()
    password = data.get("password") or ""
    user = db_find_user_by_email(email)
    if not user or not check_password_hash(user["password_hash"], password):
        return jsonify({"error": "invalid credentials"}), 401
    session["user_id"] = int(user["id"])
    db_update_last_login(int(user["id"]))
    return jsonify({
        "ok": True,
        "user": {
            "id": user["id"],
            "username": user["username"],
            "email": user["email"],
            "account_created": user["account_created"],
            "last_login": user.get("last_login"),
            "is_admin": int(user.get("is_admin", 0)),
        }
    })


@app.route("/api/logout", methods=["POST"])  # POST to avoid CSRF-by-link in a real app
def logout():
    session.clear()
    return jsonify({"ok": True})


@app.route("/api/me")
def me():
    uid = session.get("user_id")
    if not uid:
        return jsonify({"authenticated": False})
    user = db_find_user_by_id(int(uid))
    if not user:
        return jsonify({"authenticated": False})
    return jsonify({
        "authenticated": True,
        "user": {
            "id": user["id"],
            "username": user["username"],
            "email": user["email"],
            "account_created": user["account_created"],
            "last_login": user.get("last_login"),
            "is_admin": int(user.get("is_admin", 0)),
        }
    })


# ---- Logged-in user endpoints ----
@app.route("/api/cartridge")
def get_cartridge():
    require_login()
    global cartridge_data
    with state_lock:
        if cartridge_id == "0000000000000000":
            send_serial("mem -r 0 f")
            time.sleep(1)
        return jsonify(cartridge_data.get("cartridge", {}))


@app.route("/api/all_cartridge_data")
def get_all_cartridge_data():
    require_login()
    with state_lock:
        return jsonify(cartridge_data)


@app.route("/api/schedule")
def get_schedule():
    require_login()
    with state_lock:
        return jsonify(cartridge_data.get("schedule", []))


@app.route("/api/info")
def get_info():
    require_login()
    with state_lock:
        return jsonify(cartridge_data.get("info", {}))


@app.route("/api/prescription")
def get_prescription():
    require_login()
    with state_lock:
        return jsonify(cartridge_data.get("prescription", {}))


@app.route("/api/activate_cartridge", methods=["POST"])  # fixed: wrong keys and assignments
def post_activate_cartridge():
    require_login()
    data = request.get_json(force=True) or {}
    offset = int(data.get("offset") or 0)
    global cartridge_data
    with state_lock:
        info = cartridge_data.get("info", {})
        # Expand/prepare schedule first
        expanded = unpack_cartridge(cartridge_data, start_offset=offset)
        if expanded is not None:
            cartridge_data = expanded
        # Now mark as activated if not already
        if not bool(info.get("activated")):
            info["activated"] = True
            cartridge_data["info"] = info
            return jsonify({"ok": True})
        else:
            return jsonify({"ok": False, "reason": "already activated"})


@app.route("/api/time")
def get_time():
    require_login()
    return jsonify({"server_time": now_local().isoformat(timespec="seconds")})


@app.route("/api/events")
def get_events():
    require_login()
    with state_lock:
        return jsonify({"events": events})


# Optional helper: dispense using an explicit cmd (mods map)
@app.route("/api/dispense", methods=["POST"])
def dispense():
    require_login()
    data = request.get_json(force=True) or {}
    cmd = data.get("cmd") or {}
    if not isinstance(cmd, dict):
        return jsonify({"error": "cmd must be a dict with mod1..mod4 quantities"}), 400
    # Coerce to ints with defaults
    user_dispense_pills(int(cmd.get("mod1", 0) or 0), int(cmd.get("mod2", 0) or 0), int(cmd.get("mod3", 0) or 0), int(cmd.get("mod4", 0) or 0))
    return jsonify({"ok": True})


# ---- Admin endpoints ----
@app.route("/api/send_cmd", methods=["POST"])
def admin_send_cmd():
    require_admin()
    data = request.get_json(force=True) or {}
    cmd = (data.get("cmd") or "").strip()
    if not cmd:
        return jsonify({"error": "cmd required"}), 400
    send_serial(cmd)
    return jsonify({"ok": True})


@app.route("/api/logs")
def admin_logs():
    require_admin()
    # Return as simple text; could be NDJSON
    with state_lock:
        body = "\n".join(serial_logs)
    return app.response_class(body, mimetype="text/plain")


@app.route("/api/play_sound", methods=["POST"])
def admin_play_sound():
    require_admin()
    data = request.get_json(force=True) or {}
    sound_name = (data.get("sound_name") or "").strip()
    if not sound_name:
        return jsonify({"error": "sound_name required"}), 400
    if "alarm" in sound_name:
        sound_alarm.play()
    elif "error" in sound_name:
        sound_error.play()
    elif "btn_short" in sound_name:
        sound_btn_short.play()
    elif "success" in sound_name:
        sound_success.play()
    else:
        return jsonify({"error": "sound not found"}), 404
    return jsonify({"ok": True})


# ---------------------------- Bootstrapping ----------------------------

def create_default_admin():
    # Create an admin user if none exists, with default credentials
    # Email: admin@example.com, Password: admin (DEMO ONLY)
    user = db_find_user_by_email("admin@example.com")
    if not user:
        db_create_user("admin", "admin@example.com", "admin", is_admin=1)


def startup() -> None:
    db_init()
    create_default_admin()
    init_serial()

    # Start background threads
    threading.Thread(target=serial_reader_thread, daemon=True).start()
    threading.Thread(target=scheduler_thread, daemon=True).start()
    threading.Thread(target=serial_heartbeat_thread, daemon=True).start()

    # This will update cartridge ID
    send_serial("mem -r 0 f")


def scheduler_thread() -> None:
    while True:
        try:
            schedule_tick()
        except Exception as e:  # pragma: no cover
            log_line(f"Scheduler error: {e}")
        time.sleep(1)  # 1 Hz tick is fine for demo


if __name__ == "__main__":
    startup()
    # Bind to 0.0.0.0 so it is reachable on the LAN; debug False for less noise
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", 5000)), debug=False)
