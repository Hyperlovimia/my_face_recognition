import asyncio
import json
import os
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

    def start(self, loop: asyncio.AbstractEventLoop) -> None:
        self.loop = loop
        self._init_db()
        self.mqtt.connect_async(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)
        self.mqtt.loop_start()

    def stop(self) -> None:
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
            self.mqtt_connected.set()
            client.subscribe("k230/+/up/#", qos=1)
        else:
            self.mqtt_connected.clear()

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        self.mqtt_connected.clear()

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
        with self.db_lock:
            self.db.execute(
                """
                INSERT INTO events(device_id, evt_kind, face_id, name, score, ts_ms, payload_json, created_at)
                VALUES(?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (device_id, evt_kind, face_id, name, score, ts_ms, json_dumps(payload), now_iso()),
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
        return {"type": "event", "device_id": device_id, "payload": payload}

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
        db_count = count if ok and cmd == "db_count" else (0 if ok and cmd == "db_reset" else (int(row["db_count"]) if row else -1))
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
        return [dict(row) for row in rows]

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

    def publish_command(self, device_id: str, cmd: str, *, name: str | None = None) -> dict[str, Any]:
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
