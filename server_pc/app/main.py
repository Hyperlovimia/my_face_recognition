import asyncio
import base64
import json
import os
import re
import sqlite3
import time
import threading
import uuid
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field


BASE_DIR = Path(__file__).resolve().parents[1]
STATIC_DIR = BASE_DIR / "static"
DB_PATH = Path(os.getenv("FACE_WEB_DB_PATH", "/app/data/face_web.db"))
MQTT_HOST = os.getenv("FACE_MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("FACE_MQTT_PORT", "1883"))
MQTT_KEEPALIVE = int(os.getenv("FACE_MQTT_KEEPALIVE", "30"))
SCHEMA = "k230.face.bridge.v1"


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def server_now_ms() -> int:
    """Wall-clock ms on the PC; use for Last seen so UI matches local time even if device clock is unset."""
    return int(time.time() * 1000)


def pc_utc_offset_minutes() -> int:
    """Minutes east of UTC for TF attendance wall_time (should match your desk clock)."""
    raw = os.getenv("FACE_ATTENDANCE_UTC_OFFSET_MINUTES", "").strip()
    if raw:
        try:
            v = int(raw)
            if -840 <= v <= 840:
                return v
        except ValueError:
            pass
    off = datetime.now().astimezone().utcoffset()
    if off is None:
        return 0
    return int(off.total_seconds() // 60)


# 小于此值的 ts_ms 视为设备未校时（约在 2001-09 之前）；网页展示改用服务端入库时间。
_MIN_SANE_DEVICE_EPOCH_MS = 1_000_000_000_000


def _parse_created_at_to_epoch_ms(created_at_iso: str) -> int | None:
    """把服务端写入的 created_at 转为 epoch ms；兼容 ISO8601 / 空格日期 / 无 Z 后缀。"""
    s = created_at_iso.strip()
    if not s:
        return None
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    try:
        dt = datetime.fromisoformat(s.replace("Z", "+00:00"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return int(dt.timestamp() * 1000)
    except ValueError:
        pass
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S"):
        try:
            head = s[:19] if len(s) >= 19 else s
            dt = datetime.strptime(head, fmt).replace(tzinfo=timezone.utc)
            return int(dt.timestamp() * 1000)
        except ValueError:
            continue
    return None


def event_ts_ms_for_ui(ts_ms: int, created_at_iso: str | None) -> int:
    if ts_ms >= _MIN_SANE_DEVICE_EPOCH_MS:
        return ts_ms
    if created_at_iso:
        parsed = _parse_created_at_to_epoch_ms(created_at_iso)
        if parsed is not None:
            return parsed
    return server_now_ms()


def json_dumps(data: Any) -> str:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


class RegisterCurrentBody(BaseModel):
    name: str = Field(..., min_length=1, max_length=63)


class ConnectionManager:
    def __init__(self) -> None:
        self._connections: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, websocket: WebSocket) -> None:
        await websocket.accept()
        async with self._lock:
            self._connections.add(websocket)

    async def disconnect(self, websocket: WebSocket) -> None:
        async with self._lock:
            self._connections.discard(websocket)

    async def broadcast(self, message: dict[str, Any]) -> None:
        payload = json_dumps(message)
        async with self._lock:
            connections = list(self._connections)
        stale: list[WebSocket] = []
        for websocket in connections:
            try:
                await websocket.send_text(payload)
            except Exception:
                stale.append(websocket)
        if stale:
            async with self._lock:
                for websocket in stale:
                    self._connections.discard(websocket)


class FaceWebState:
    def __init__(self) -> None:
        DB_PATH.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(DB_PATH, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db_lock = threading.Lock()
        self.mqtt_connected = threading.Event()
        self.loop: asyncio.AbstractEventLoop | None = None
        self.ws = ConnectionManager()
        self.mqtt = mqtt.Client(client_id=f"face-web-{uuid.uuid4().hex[:8]}", clean_session=True)
        self.mqtt.enable_logger()
        self.mqtt.on_connect = self._on_connect
        self.mqtt.on_disconnect = self._on_disconnect
        self.mqtt.on_message = self._on_message
        self.mqtt.reconnect_delay_set(min_delay=1, max_delay=10)
        self._time_sync_timer: threading.Timer | None = None

    def _cancel_time_sync_timer(self) -> None:
        if self._time_sync_timer is not None:
            self._time_sync_timer.cancel()
            self._time_sync_timer = None

    def _publish_time_sync(self) -> None:
        if not self.mqtt_connected.is_set():
            return
        payload = json_dumps(
            {
                "schema": SCHEMA,
                "server_ms": server_now_ms(),
                "iso": now_iso(),
                "utc_offset_minutes": pc_utc_offset_minutes(),
            }
        )
        self.mqtt.publish("k230/time_sync", payload, qos=0, retain=True)

    def _schedule_next_time_sync_broadcast(self) -> None:
        self._cancel_time_sync_timer()

        def fire() -> None:
            self._publish_time_sync()
            self._schedule_next_time_sync_broadcast()

        self._time_sync_timer = threading.Timer(120.0, fire)
        self._time_sync_timer.daemon = True
        self._time_sync_timer.start()

    def start(self, loop: asyncio.AbstractEventLoop) -> None:
        self.loop = loop
        self._init_db()
        self.mqtt.connect_async(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)
        self.mqtt.loop_start()

    def stop(self) -> None:
        self._cancel_time_sync_timer()
        self.mqtt.loop_stop()
        try:
            self.mqtt.disconnect()
        except Exception:
            pass
        self.db.close()

    def _init_db(self) -> None:
        DB_PATH.parent.mkdir(parents=True, exist_ok=True)
        with self.db_lock:
            cur = self.db.cursor()
            cur.executescript(
                """
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    online INTEGER NOT NULL DEFAULT 0,
                    rt_connected INTEGER NOT NULL DEFAULT 0,
                    db_count INTEGER NOT NULL DEFAULT -1,
                    last_seen_ms INTEGER NOT NULL DEFAULT 0,
                    last_status_json TEXT,
                    updated_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT NOT NULL,
                    evt_kind TEXT NOT NULL,
                    face_id INTEGER NOT NULL,
                    name TEXT NOT NULL,
                    score REAL NOT NULL,
                    ts_ms INTEGER NOT NULL,
                    payload_json TEXT NOT NULL,
                    created_at TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS commands (
                    request_id TEXT PRIMARY KEY,
                    device_id TEXT NOT NULL,
                    cmd TEXT NOT NULL,
                    status TEXT NOT NULL,
                    name TEXT,
                    ok INTEGER,
                    count INTEGER,
                    message TEXT,
                    payload_json TEXT,
                    reply_json TEXT,
                    requested_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                """
            )
            self.db.commit()

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: dict[str, Any], rc: int) -> None:
        if rc == 0:
            self._cancel_time_sync_timer()
            self.mqtt_connected.set()
            client.subscribe("k230/+/up/#", qos=1)
            self._publish_time_sync()
            self._schedule_next_time_sync_broadcast()
        else:
            self.mqtt_connected.clear()

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        self.mqtt_connected.clear()
        self._cancel_time_sync_timer()

    def _on_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except json.JSONDecodeError:
            return

        parts = msg.topic.split("/")
        if len(parts) != 4 or parts[0] != "k230" or parts[2] != "up":
            return

        device_id = parts[1]
        kind = parts[3]
        if kind == "status":
            event = self._handle_status(device_id, payload)
        elif kind == "event":
            event = self._handle_event(device_id, payload)
        elif kind == "reply":
            event = self._handle_reply(device_id, payload)
        else:
            return

        if event and self.loop:
            asyncio.run_coroutine_threadsafe(self.ws.broadcast(event), self.loop)

    def _upsert_device(
        self,
        device_id: str,
        *,
        online: int,
        rt_connected: int,
        db_count: int,
        last_seen_ms: int,
        last_status_json: str | None,
        updated_at: str | None = None,
    ) -> None:
        ts = updated_at if updated_at is not None else now_iso()
        with self.db_lock:
            self.db.execute(
                """
                INSERT INTO devices(device_id, online, rt_connected, db_count, last_seen_ms, last_status_json, updated_at)
                VALUES(?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    online=excluded.online,
                    rt_connected=excluded.rt_connected,
                    db_count=excluded.db_count,
                    last_seen_ms=excluded.last_seen_ms,
                    last_status_json=COALESCE(excluded.last_status_json, devices.last_status_json),
                    updated_at=excluded.updated_at
                """,
                (
                    device_id,
                    online,
                    rt_connected,
                    db_count,
                    last_seen_ms,
                    last_status_json,
                    ts,
                ),
            )
            self.db.commit()

    def _handle_status(self, device_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        online = 1 if payload.get("online") else 0
        rt_connected = 1 if payload.get("rt_connected") else 0
        db_count = int(payload.get("db_count", -1))
        last_seen_ms = server_now_ms()
        updated = now_iso()
        self._upsert_device(
            device_id,
            online=online,
            rt_connected=rt_connected,
            db_count=db_count,
            last_seen_ms=last_seen_ms,
            last_status_json=json_dumps(payload),
            updated_at=updated,
        )
        device_row: dict[str, Any] = {
            "device_id": device_id,
            "online": online,
            "rt_connected": rt_connected,
            "db_count": db_count,
            "last_seen_ms": last_seen_ms,
            "updated_at": updated,
        }
        return {
            "type": "status",
            "device_id": device_id,
            "payload": payload,
            "device": device_row,
        }

    def _handle_event(self, device_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        evt_kind = str(payload.get("evt_kind", "unknown"))
        face_id = int(payload.get("face_id", -1))
        name = str(payload.get("name", ""))
        score = float(payload.get("score", 0.0))
        ts_ms = int(payload.get("ts_ms", 0))
        created = now_iso()
        ts_ms_ui = event_ts_ms_for_ui(ts_ms, created)
        with self.db_lock:
            self.db.execute(
                """
                INSERT INTO events(device_id, evt_kind, face_id, name, score, ts_ms, payload_json, created_at)
                VALUES(?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (device_id, evt_kind, face_id, name, score, ts_ms_ui, json_dumps(payload), created),
            )
            self.db.commit()
        row = self.get_device_state(device_id)
        online = int(bool(row["online"])) if row else 1
        rt_connected = int(bool(row["rt_connected"])) if row else 1
        db_count = int(row["db_count"]) if row else -1
        self._upsert_device(
            device_id,
            online=online,
            rt_connected=rt_connected,
            db_count=db_count,
            last_seen_ms=server_now_ms(),
            last_status_json=None,
        )
        payload_out = dict(payload)
        payload_out["ts_ms"] = ts_ms_ui
        payload_out["device_ts_ms"] = ts_ms
        payload_out["created_at"] = created
        return {"type": "event", "device_id": device_id, "payload": payload_out}

    def _handle_reply(self, device_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        request_id = str(payload.get("request_id", ""))
        cmd = str(payload.get("cmd", ""))
        ok = 1 if payload.get("ok") else 0
        count = int(payload.get("count", -1))
        message = str(payload.get("message", ""))
        with self.db_lock:
            self.db.execute(
                """
                INSERT INTO commands(request_id, device_id, cmd, status, name, ok, count, message, payload_json, reply_json, requested_at, updated_at)
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(request_id) DO UPDATE SET
                    status=excluded.status,
                    ok=excluded.ok,
                    count=excluded.count,
                    message=excluded.message,
                    reply_json=excluded.reply_json,
                    updated_at=excluded.updated_at
                """,
                (
                    request_id,
                    device_id,
                    cmd,
                    "done" if ok else "failed",
                    None,
                    ok,
                    count,
                    message,
                    None,
                    json_dumps(payload),
                    now_iso(),
                    now_iso(),
                ),
            )
            self.db.commit()

        row = self.get_device_state(device_id)
        online = int(bool(row["online"])) if row else 1
        rt_connected = int(bool(row["rt_connected"])) if row else 1
        if ok and cmd == "db_count":
            db_count = count
        elif ok and cmd == "db_reset":
            db_count = 0
        elif ok and cmd == "db_face_list":
            db_count = count
        elif cmd == "import_faces":
            db_count = int(payload.get("db_count_after", row["db_count"] if row else -1))
        else:
            db_count = int(row["db_count"]) if row else -1
        self._upsert_device(
            device_id,
            online=online,
            rt_connected=rt_connected,
            db_count=db_count,
            last_seen_ms=server_now_ms(),
            last_status_json=None,
        )
        return {"type": "reply", "device_id": device_id, "payload": payload}

    def list_devices(self) -> list[dict[str, Any]]:
        with self.db_lock:
            rows = self.db.execute(
                "SELECT device_id, online, rt_connected, db_count, last_seen_ms, updated_at FROM devices ORDER BY device_id"
            ).fetchall()
        return [dict(row) for row in rows]

    def get_device_state(self, device_id: str) -> sqlite3.Row | None:
        with self.db_lock:
            return self.db.execute(
                "SELECT device_id, online, rt_connected, db_count, last_seen_ms, updated_at FROM devices WHERE device_id = ?",
                (device_id,),
            ).fetchone()

    def get_recent_commands(self, device_id: str, limit: int = 20) -> list[dict[str, Any]]:
        with self.db_lock:
            rows = self.db.execute(
                """
                SELECT request_id, cmd, status, ok, count, message, requested_at, updated_at
                FROM commands
                WHERE device_id = ?
                ORDER BY updated_at DESC
                LIMIT ?
                """,
                (device_id, limit),
            ).fetchall()
        return [dict(row) for row in rows]

    def get_command_by_request_id(self, request_id: str) -> dict[str, Any] | None:
        with self.db_lock:
            row = self.db.execute(
                """
                SELECT request_id, device_id, cmd, status, ok, count, message, reply_json, updated_at
                FROM commands
                WHERE request_id = ?
                """,
                (request_id,),
            ).fetchone()
        return dict(row) if row else None

    def get_events(self, device_id: str, limit: int) -> list[dict[str, Any]]:
        with self.db_lock:
            rows = self.db.execute(
                """
                SELECT evt_kind, face_id, name, score, ts_ms, created_at
                FROM events
                WHERE device_id = ?
                ORDER BY id DESC
                LIMIT ?
                """,
                (device_id, limit),
            ).fetchall()
        out: list[dict[str, Any]] = []
        for r in rows:
            d = dict(r)
            d["ts_ms"] = event_ts_ms_for_ui(int(d.get("ts_ms") or 0), d.get("created_at"))
            out.append(d)
        return out

    def clear_web_data(self) -> dict[str, int]:
        with self.db_lock:
            cur = self.db.cursor()
            event_count = int(cur.execute("SELECT COUNT(*) FROM events").fetchone()[0])
            command_count = int(cur.execute("SELECT COUNT(*) FROM commands").fetchone()[0])
            device_count = int(cur.execute("SELECT COUNT(*) FROM devices").fetchone()[0])
            cur.executescript(
                """
                DELETE FROM events;
                DELETE FROM commands;
                DELETE FROM devices;
                DELETE FROM sqlite_sequence WHERE name IN ('events', 'commands');
                """
            )
            self.db.commit()
        return {
            "devices": device_count,
            "events": event_count,
            "commands": command_count,
        }

    def publish_command(
        self,
        device_id: str,
        cmd: str,
        *,
        name: str | None = None,
        slot: int | None = None,
        mqtt_extra: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        if not self.mqtt_connected.is_set():
            raise HTTPException(status_code=503, detail="MQTT broker is not connected")

        request_id = uuid.uuid4().hex
        payload: dict[str, Any] = {
            "schema": SCHEMA,
            "request_id": request_id,
            "cmd": cmd,
        }
        if name is not None:
            payload["name"] = name
        if slot is not None:
            payload["slot"] = slot
        if mqtt_extra:
            payload.update(mqtt_extra)

        with self.db_lock:
            self.db.execute(
                """
                INSERT INTO commands(request_id, device_id, cmd, status, name, ok, count, message, payload_json, reply_json, requested_at, updated_at)
                VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    request_id,
                    device_id,
                    cmd,
                    "pending",
                    name,
                    None,
                    None,
                    None,
                    json_dumps(payload),
                    None,
                    now_iso(),
                    now_iso(),
                ),
            )
            self.db.commit()

        topic = f"k230/{device_id}/down/cmd"
        info = self.mqtt.publish(topic, json_dumps(payload), qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise HTTPException(status_code=503, detail="Failed to publish MQTT command")
        return {"request_id": request_id, "status": "accepted"}


state = FaceWebState()


async def await_mqtt_command_done(request_id: str, timeout_s: float) -> dict[str, Any] | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        row = state.get_command_by_request_id(request_id)
        if row and row.get("status") in ("done", "failed"):
            return row
        await asyncio.sleep(0.05)
    return None


def mqtt_reply_body(reply_json: str | None) -> dict[str, Any]:
    if not reply_json:
        return {}
    try:
        return json.loads(reply_json)
    except json.JSONDecodeError:
        return {}


@asynccontextmanager
async def lifespan(app: FastAPI):
    state.start(asyncio.get_running_loop())
    yield
    state.stop()


app = FastAPI(title="face-web", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/", include_in_schema=False)
async def root() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/favicon.ico", include_in_schema=False)
async def favicon() -> Response:
    """避免浏览器自动请求时刷 404 日志；无资源时 204 即可。"""
    return Response(status_code=204)


@app.get("/api/server-time")
async def api_server_time() -> dict[str, Any]:
    """板端 Linux 小核未校时时，face_netd 用此接口推算考勤目录日期（与 DB 里 server_now_ms / created_at 思路一致）。"""
    return {
        "server_ms": server_now_ms(),
        "iso": now_iso(),
        "utc_offset_minutes": pc_utc_offset_minutes(),
    }


@app.get("/api/devices")
async def api_devices() -> dict[str, Any]:
    return {"devices": state.list_devices()}


@app.get("/api/devices/{device_id}/state")
async def api_device_state(device_id: str) -> dict[str, Any]:
    row = state.get_device_state(device_id)
    if row is None:
        raise HTTPException(status_code=404, detail="device not found")
    return {"device": dict(row), "recent_commands": state.get_recent_commands(device_id)}


@app.get("/api/devices/{device_id}/events")
async def api_device_events(device_id: str, limit: int = Query(default=100, ge=1, le=500)) -> dict[str, Any]:
    if state.get_device_state(device_id) is None:
        raise HTTPException(status_code=404, detail="device not found")
    return {"events": state.get_events(device_id, limit)}

_YMD_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


@app.get("/api/devices/{device_id}/sd-attendance-log")
async def api_sd_attendance_log(
    device_id: str,
    timeout_sec: float = Query(default=25.0, ge=3.0, le=120.0),
    max_bytes: int = Query(default=262144, ge=4096, le=1048576),
    tail_lines: int = Query(default=800, ge=1, le=20000),
    date: str | None = Query(default=None, description="YYYY-MM-DD on device; omit for device local today"),
) -> dict[str, Any]:
    """由 Linux 小核 face_netd 读取 TF 上按日文件 <YYYY-MM-DD>.jsonl（MQTT）。"""
    if date is not None and not _YMD_RE.match(date):
        raise HTTPException(status_code=400, detail="date must be YYYY-MM-DD")
    if state.get_device_state(device_id) is None:
        raise HTTPException(status_code=404, detail="device not found")
    mqtt_extra: dict[str, Any] = {"max_bytes": max_bytes, "tail_lines": tail_lines}
    if date:
        mqtt_extra["date"] = date
    accepted = state.publish_command(device_id, "attendance_log_fetch", mqtt_extra=mqtt_extra)
    request_id = str(accepted["request_id"])
    row = await await_mqtt_command_done(request_id, timeout_sec)
    if row is None:
        raise HTTPException(status_code=504, detail="Timed out waiting for device reply")
    body = mqtt_reply_body(row.get("reply_json"))
    if row.get("status") != "done" or not body.get("ok"):
        detail = str(body.get("message") or row.get("message") or "attendance log fetch failed")
        raise HTTPException(status_code=502, detail=detail)
    b64 = body.get("log_b64")
    if not isinstance(b64, str):
        raise HTTPException(status_code=502, detail="no log payload in reply")
    try:
        raw = base64.b64decode(b64, validate=False)
    except Exception:
        raise HTTPException(status_code=502, detail="invalid base64 log payload") from None
    text = raw.decode("utf-8", errors="replace")
    return {
        "path": body.get("path"),
        "date": body.get("date"),
        "truncated": bool(body.get("truncated")),
        "file_size": body.get("file_size"),
        "bytes_returned": body.get("bytes_returned"),
        "content": text,
    }


@app.get("/api/devices/{device_id}/face-gallery")
async def api_face_gallery(
    device_id: str,
    timeout_sec: float = Query(default=20.0, ge=3.0, le=120.0),
) -> dict[str, Any]:
    """列出板端 face_db 中已注册条目（由 Linux face_netd 读目录并通过 MQTT 应答）。"""
    accepted = state.publish_command(device_id, "db_face_list")
    request_id = str(accepted["request_id"])
    row = await await_mqtt_command_done(request_id, timeout_sec)
    if row is None:
        raise HTTPException(status_code=504, detail="Timed out waiting for device reply")
    body = mqtt_reply_body(row.get("reply_json"))
    if row.get("status") != "done" or not body.get("ok"):
        detail = str(body.get("message") or row.get("message") or "face gallery list failed")
        raise HTTPException(status_code=502, detail=detail)
    entries = body.get("entries")
    if not isinstance(entries, list):
        entries = []
    return {"entries": entries}


@app.get("/api/devices/{device_id}/face-gallery/{slot}/photo.jpg")
async def api_face_gallery_photo(
    device_id: str,
    slot: int,
    timeout_sec: float = Query(default=25.0, ge=3.0, le=120.0),
) -> Response:
    """拉取单张注册抓拍图（MQTT 回传 base64，由服务端解码）。"""
    if slot < 1 or slot > 4096:
        raise HTTPException(status_code=400, detail="invalid slot")
    accepted = state.publish_command(device_id, "db_face_image", slot=slot)
    request_id = str(accepted["request_id"])
    row = await await_mqtt_command_done(request_id, timeout_sec)
    if row is None:
        raise HTTPException(status_code=504, detail="Timed out waiting for device reply")
    body = mqtt_reply_body(row.get("reply_json"))
    if row.get("status") != "done" or not body.get("ok"):
        detail = str(body.get("message") or row.get("message") or "photo fetch failed")
        raise HTTPException(status_code=404, detail=detail)
    b64 = body.get("image_b64")
    if not isinstance(b64, str) or not b64:
        raise HTTPException(status_code=404, detail="no image in reply")
    try:
        raw = base64.b64decode(b64, validate=False)
    except Exception:
        raise HTTPException(status_code=502, detail="invalid base64 image payload") from None
    return Response(content=raw, media_type="image/jpeg")


@app.post("/api/web-data/clear")
async def api_clear_web_data() -> dict[str, Any]:
    cleared = state.clear_web_data()
    if state.loop:
        await state.ws.broadcast({"type": "snapshot", "devices": state.list_devices()})
    return {"status": "ok", "cleared": cleared}


@app.post("/api/devices/{device_id}/commands/db-count", status_code=202)
async def api_cmd_db_count(device_id: str) -> dict[str, Any]:
    return state.publish_command(device_id, "db_count")


@app.post("/api/devices/{device_id}/commands/db-reset", status_code=202)
async def api_cmd_db_reset(device_id: str) -> dict[str, Any]:
    return state.publish_command(device_id, "db_reset")


@app.post("/api/devices/{device_id}/commands/register-current", status_code=202)
async def api_cmd_register_current(device_id: str, body: RegisterCurrentBody) -> dict[str, Any]:
    return state.publish_command(device_id, "register_current", name=body.name.strip())


@app.post("/api/devices/{device_id}/commands/register-preview", status_code=202)
async def api_cmd_register_preview(device_id: str) -> dict[str, Any]:
    return state.publish_command(device_id, "register_preview")


@app.post("/api/devices/{device_id}/commands/register-commit", status_code=202)
async def api_cmd_register_commit(device_id: str, body: RegisterCurrentBody) -> dict[str, Any]:
    return state.publish_command(device_id, "register_commit", name=body.name.strip())


@app.post("/api/devices/{device_id}/commands/register-cancel", status_code=202)
async def api_cmd_register_cancel(device_id: str) -> dict[str, Any]:
    return state.publish_command(device_id, "register_cancel")


@app.post("/api/devices/{device_id}/commands/import-faces", status_code=202)
async def api_cmd_import_faces(device_id: str) -> dict[str, Any]:
    return state.publish_command(device_id, "import_faces")


@app.post("/api/devices/{device_id}/commands/shutdown", status_code=202)
async def api_cmd_shutdown(device_id: str) -> dict[str, Any]:
    return state.publish_command(device_id, "shutdown")


@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket) -> None:
    await state.ws.connect(websocket)
    try:
        await websocket.send_text(json_dumps({"type": "snapshot", "devices": state.list_devices()}))
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        await state.ws.disconnect(websocket)
    except Exception:
        await state.ws.disconnect(websocket)
