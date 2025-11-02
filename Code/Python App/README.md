# API Reference — Flask Serial Server for Pillex

This doc is designed for a front-end developer integrating with the Flask server. It’s a session-cookie API (no JWT). All JSON unless noted.

---

## Base

* **Base URL:** `{{url}}`
* **Static UI:** `{{url}}/static/index.html` (root `/` maps here)
* **Prefix:** All endpoints below are under `/api/*` unless stated.

### Auth model

* Cookie-based session (Flask `session` cookie).
* Send requests with cookies: `fetch(url, { credentials: "include" })`.
* **Default admin (demo only):** `admin@example.com` / `admin` (auto-created on startup).

### Error model

* JSON errors: `{ "error": string }` with appropriate HTTP status:

  * `400` bad input
  * `401` not authenticated
  * `403` not admin
  * `409` conflict (e.g., registering an existing email)
* Some success payloads include `{ "ok": true }`. A few return `{ "ok": false, reason }`.

---

## Auth Endpoints

### POST `/api/login`

Log a user in; sets session cookie.

**Body**

```json
{ "email": "user@example.com", "password": "secret" }
```

**200**

```json
{
  "ok": true,
  "user": {
    "id": 1,
    "username": "admin",
    "email": "admin@example.com",
    "account_created": "2025-08-29T07:12:34.567890",
    "last_login": "2025-08-29T07:15:00.000000",
    "is_admin": 1
  }
}
```

**401**

```json
{ "error": "invalid credentials" }
```

---

### POST `/api/logout`

Clears the session.

**200**

```json
{ "ok": true }
```

---

### GET `/api/me`

Returns current session info.

**200 (not logged in)**

```json
{ "authenticated": false }
```

**200 (logged in)**

```json
{
  "authenticated": true,
  "user": { "id": 1, "username": "admin", "email": "admin@example.com", "account_created": "...", "last_login": "...", "is_admin": 1 }
}
```

---

### POST `/api/register`  *(Admin only)*

Create a user. Requires an **admin** session.

**Body**

```json
{
  "username": "Jane",
  "email": "jane@example.com",
  "password": "hunter2",
  "is_admin": 0
}
```

**200**

```json
{ "ok": true, "user_id": 2 }
```

**409**

```json
{ "error": "email already registered" }
```

**401/403** if not authenticated or not admin.

---

## User Endpoints (require login)

> All these require `credentials: "include"` in `fetch`.

### GET `/api/cartridge`

Returns the **cartridge** block only.

**200**

```json
{
  "mod1": { "description": "...", "med_name": "...", "expiry_date": "YYYY-MM-DD", "quantity": 14, "quantity_start": 14 },
  "mod2": { ... },
  "mod3": { ... },
  "mod4": { ... }
}
```

---

### GET `/api/all_cartridge_data`

Returns the full data object: `cartridge`, `info`, `prescription`, `schedule`.

**200** (example snippet)

```json
{
  "cartridge": { "mod1": { "description": "Pain relief", "med_name": "vicodin 5mg", "expiry_date": "2027-01-10", "quantity": 14, "quantity_start": 14 }, "...": {} },
  "info": { "activated": false, "id": "7B25E25C38F0A3A9", "manufactured": "2025-08-15", "version": "v0.2" },
  "prescription": { "doctor": { "full_name": "Dr Sarah Lin", "contact": "sarah.lin@example.com" }, "patient": { "full_name": "Mr John Smith", "dob": "1975-03-09" }, "expiry_date": "2026-10-31", "expired": false, "schedule_repeat_days": 30, "start_day": null },
  "schedule": [
    {
      "day": 1,
      "time_start": "07:30:00",
      "time_end": "09:00:00",
      "datetime_start": "2025-08-29T06:30:00Z",
      "datetime_end": "2025-08-29T08:00:00Z",
      "cmd": { "mod1": 1, "mod2": 0, "mod3": 0, "mod4": 0 },
      "status": "DUE",
      "hash": "081a0422266aa6df"
    }
  ]
}
```

---

### GET `/api/schedule`

Returns the `schedule` array only.
Note that the `cmd` references objects in `cartridge`

e.g.: `"mod1" : 3` means that three pills from module 1 will be dispensed

**200**

```json
[
  {
    "day": 1,
    "time_start": "13:00:00",
    "time_end": "14:30:00",
    "datetime_start": "2025-08-29T12:00:00Z",
    "datetime_end": "2025-08-29T13:30:00Z",
    "cmd": { "mod1": 3, "mod2": 1, "mod3": 1, "mod4": 0 },
    "status": "DUE",
    "hash": "5bf60127e43d1f16"
  }
]
```

---

### GET `/api/info`

Returns the `info` object from the cartridge.

**200**

```json
{ "activated": false, "id": "7B25E25C38F0A3A9", "manufactured": "2025-08-15", "version": "v0.2" }
```

---

### GET `/api/prescription`

Returns the `prescription` object.

**200**

```json
{
  "doctor": { "full_name": "Dr Sarah Lin", "contact": "sarah.lin@example.com" },
  "patient": { "full_name": "Mr John Smith", "dob": "1975-03-09" },
  "doctor_message": "Take omeprazole after food",
  "expiry_date": "2026-10-31",
  "expired": false,
  "schedule_repeat_days": 30,
  "start_day": null
}
```

---

### POST `/api/activate_cartridge`

Expands/prepares schedule and marks the cartridge as **activated** (if not already).

**Body**

```json
{ "offset": 0 }
```

* `offset` (int): reserved (start offset). Currently parsed but not applied to times.

**200**

```json
{ "ok": true }
```

**200 (already active)**

```json
{ "ok": false, "reason": "already activated" }
```

---

### GET `/api/time`

Server local time (ISO seconds, no TZ suffix).

**200**

```json
{ "server_time": "2025-08-29T09:42:17" }
```

---

### GET `/api/events`

Rolling event feed (in-memory). Useful for live UX.

**200**

```json
{
  "events": [
    { "ts": "2025-08-29T09:15:00", "kind": "dose_window_started", "payload": { "start": "2025-08-29T09:00:00Z", "end": "2025-08-29T10:00:00Z" } },
    { "ts": "2025-08-29T09:20:03", "kind": "pills_dispensed", "payload": { "cmd": "drpall 1 0 0 0" } },
    { "ts": "2025-08-29T09:20:04", "kind": "dose_taken", "payload": { "entry": { "day": 1, "datetime_start": "...", "datetime_end": "...", "hash": "..." } } }
  ]
}
```

**Known `kind` values**

* `dose_window_started`, `dose_missed`, `pills_dispensed`, `dose_taken`, `no_active_dose_on_ok`.

---

### POST `/api/dispense`

Helper to manually dispense by quantities (also updates cartridge counts).

**Body**

```json
{ "cmd": { "mod1": 1, "mod2": 0, "mod3": 0, "mod4": 0 } }
```

**200**

```json
{ "ok": true }
```

**400**

```json
{ "error": "cmd must be a dict with mod1..mod4 quantities" }
```

---

## Admin Endpoints (require admin login)

### POST `/api/send_cmd`

Send an arbitrary command to the serial device.

**Body**

```json
{ "cmd": "SCREEN TIME" }
```

**200**

```json
{ "ok": true }
```

**400**

```json
{ "error": "cmd required" }
```

---

### GET `/api/logs`  *(text/plain)*

Rolling serial/server log (max \~2000 lines).
**Response:** `text/plain` (one line per entry).

---

## Data Shapes (TypeScript-style)

```ts
type CartridgeMod = {
  description: string;
  med_name: string;
  expiry_date: string; // YYYY-MM-DD
  quantity: number;
  quantity_start: number;
};

type Cartridge = {
  mod1?: CartridgeMod;
  mod2?: CartridgeMod;
  mod3?: CartridgeMod;
  mod4?: CartridgeMod;
};

type Info = {
  activated: boolean;
  id: string;             // cartridge UID (hex)
  manufactured: string;   // YYYY-MM-DD
  version: "v0.2";
};

type Prescription = {
  doctor: { full_name: string; contact: string };
  patient: { full_name: string; dob: string /* YYYY-MM-DD */ };
  doctor_message?: string;
  expiry_date: string; // YYYY-MM-DD
  expired: boolean;
  schedule_repeat_days: number; // e.g., 30
  start_day: string | null;     // YYYY-MM-DD or null
};

type DoseCmd = { mod1?: number; mod2?: number; mod3?: number; mod4?: number };

type ScheduleEntry = {
  day: number;                  // 1..repeat
  time_start: string;           // "HH:MM:SS" local template
  time_end: string;             // "HH:MM:SS"
  datetime_start: string;       // ISO8601 UTC "Z"
  datetime_end: string;         // ISO8601 UTC "Z"
  cmd: DoseCmd;
  status: "DUE" | "ACTIVE" | "MISSED" | "TAKEN";
  hash: string;                 // opaque id
  taken_at?: string;            // ISO8601 "Z" when TAken
};

type AllCartridgeData = {
  cartridge: Cartridge;
  info: Info;
  prescription: Prescription;
  schedule: ScheduleEntry[];
};

type EventEnvelope = {
  ts: string;                   // ISO local seconds
  kind: string;                 // see list above
  payload: Record<string, any>;
};
```

### Status semantics (for UX)

* `DUE`: window not started yet (future).
* `ACTIVE`: now within `[datetime_start, datetime_end]`. Device shows `TAKE_PILL`.
* `TAKEN`: marked after OK button (serial) or when dispense handler sets it.
* `MISSED`: window ended without being taken (server emits `dose_missed` and sends a “Dose missed!” message to the device).

---

## Usage Examples

### Login (fetch)

```js
await fetch("{{url}}/api/login", {
  method: "POST",
  credentials: "include",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ email: "admin@example.com", password: "admin" })
});
```

### Get full cartridge data

```js
const res = await fetch("{{url}}/api/all_cartridge_data", { credentials: "include" });
const data = await res.json(); // AllCartridgeData
```

### Dispense explicitly

```js
await fetch("{{url}}/api/dispense", {
  method: "POST",
  credentials: "include",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ cmd: { mod1: 1, mod2: 0, mod3: 0, mod4: 0 } })
});
```

### Admin: send device command

```js
await fetch("{{url}}/api/send_cmd", {
  method: "POST",
  credentials: "include",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ cmd: "SCREEN TIME" })
});
```

---

## Notes & Gotchas

* **Sessions:** Cookie is HTTP and HTTPS; you can’t read it in JS. Always set `credentials: "include"`.
* **CORS:** Not configured in code; assume same-origin (serve your UI from this Flask app or enable CORS yourself).
* **/api/logs content type:** `text/plain`, not JSON.
* **Activation:** Call `/api/activate_cartridge` once per new cartridge to expand schedule and mark `info.activated=true`.
* **Events buffer:** In-memory, trimmed when >2000 events; don’t rely on it for history.
