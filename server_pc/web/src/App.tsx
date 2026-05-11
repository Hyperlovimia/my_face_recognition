import { useCallback, useEffect, useRef, useState } from "react";
import SlButton from "@shoelace-style/shoelace/dist/react/button/index.js";
import SlDialog from "@shoelace-style/shoelace/dist/react/dialog/index.js";
import type { SlRequestCloseEvent } from "@shoelace-style/shoelace/dist/react/dialog/index.js";
import SlIcon from "@shoelace-style/shoelace/dist/react/icon/index.js";
import * as api from "./api";
import type { CommandRow, DeviceRow, EventRow, FaceGalleryEntry, WsMessage } from "./types";
import type { SdAttendanceLogResponse } from "./api";

/** 设备未校时时 ts_ms 接近 1970；优先信任服务端时间 */
const MIN_SANE_DEVICE_TS_MS = 1_000_000_000_000;

/** 解析服务端 ISO8601 created_at（兼容 Safari 对高位小数秒的挑剔） */
function parseIsoToMs(iso: string): number | null {
  const s0 = iso.trim();
  if (!s0) return null;
  const withT = s0.includes("T") ? s0 : s0.replace(/^(\d{4}-\d{2}-\d{2})[ T]/, "$1T");
  let t = Date.parse(withT);
  if (!Number.isNaN(t)) return t;
  const trimmedFrac = withT.replace(/\.(\d{3})\d+/, ".$1");
  if (trimmedFrac !== withT) {
    t = Date.parse(trimmedFrac);
    if (!Number.isNaN(t)) return t;
  }
  return null;
}

function eventDisplayMs_ms(ev: EventRow): number {
  const ts = Number(ev.ts_ms);
  if (Number.isFinite(ts) && ts >= MIN_SANE_DEVICE_TS_MS) {
    return ts;
  }
  if (ev.created_at) {
    const fromCreated = parseIsoToMs(ev.created_at);
    if (fromCreated !== null) {
      return fromCreated;
    }
  }
  /* 勿用设备未校时的小 ts_ms（会显示 1970）；若服务端已归一化 ts，上一分支已命中 */
  return 0;
}

function fmtTs(ms: number) {
  if (!Number.isFinite(ms) || ms <= 0) return "—";
  return new Date(ms).toLocaleString(undefined, {
    year: "numeric",
    month: "numeric",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hour12: false,
  });
}

function escapeHtml(value: string | number | null | undefined) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function todayLocalYmd(): string {
  const d = new Date();
  const y = d.getFullYear();
  const m = String(d.getMonth() + 1).padStart(2, "0");
  const day = String(d.getDate()).padStart(2, "0");
  return `${y}-${m}-${day}`;
}

export function App() {
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [selectedDevice, setSelectedDevice] = useState<string | null>(null);
  const [deviceState, setDeviceState] = useState<{
    device: DeviceRow;
    recent_commands: CommandRow[];
  } | null>(null);
  const [events, setEvents] = useState<EventRow[]>([]);
  const [wsStatus, setWsStatus] = useState<"ok" | "bad">("bad");
  const [registerOpen, setRegisterOpen] = useState(false);
  const [regName, setRegName] = useState("");
  const [registerSessionOpen, setRegisterSessionOpen] = useState(false);
  const [registerPreviewLoading, setRegisterPreviewLoading] = useState(false);
  const [galleryEntries, setGalleryEntries] = useState<FaceGalleryEntry[]>([]);
  const [galleryErr, setGalleryErr] = useState<string | null>(null);
  const [faceBoardBusy, setFaceBoardBusy] = useState(false);
  const [sdLogData, setSdLogData] = useState<SdAttendanceLogResponse | null>(null);
  const [sdLogErr, setSdLogErr] = useState<string | null>(null);
  const [sdLogLoading, setSdLogLoading] = useState(false);
  const [sdLogDate, setSdLogDate] = useState(todayLocalYmd);
  const dismissProgrammatic = useRef(false);
  const selectedRef = useRef<string | null>(null);
  const sessionRef = useRef(false);
  const wsRef = useRef<WebSocket | null>(null);
  const connectWsRef = useRef<() => void>(() => {});

  useEffect(() => {
    selectedRef.current = selectedDevice;
  }, [selectedDevice]);

  useEffect(() => {
    sessionRef.current = registerSessionOpen;
  }, [registerSessionOpen]);

  const loadDevices = useCallback(async (): Promise<string | null> => {
    const data = await api.getDevices();
    setDevices(data.devices);
    if (data.devices.length === 0) {
      setSelectedDevice(null);
      return null;
    }
    let nextSelect: string | null = null;
    setSelectedDevice((prev) => {
      const ids = data.devices.map((d) => d.device_id);
      nextSelect = prev && ids.includes(prev) ? prev : data.devices[0].device_id;
      return nextSelect;
    });
    return nextSelect;
  }, []);

  const loadSelected = useCallback(async (deviceId: string | null) => {
    if (!deviceId) {
      setDeviceState(null);
      setEvents([]);
      return;
    }
    const [st, ev] = await Promise.all([api.getDeviceState(deviceId), api.getDeviceEvents(deviceId, 100)]);
    setDeviceState({ device: st.device, recent_commands: st.recent_commands });
    setEvents(ev.events);
  }, []);

  useEffect(() => {
    void loadDevices();
  }, [loadDevices]);

  useEffect(() => {
    if (selectedDevice) {
      void loadSelected(selectedDevice);
    } else {
      setDeviceState(null);
      setEvents([]);
    }
  }, [selectedDevice, loadSelected]);

  const applyGalleryDerivedDbCount = useCallback((deviceId: string, count: number) => {
    setDevices((prev) => prev.map((d) => (d.device_id === deviceId ? { ...d, db_count: count } : d)));
    setDeviceState((prev) =>
      prev && prev.device.device_id === deviceId ? { ...prev, device: { ...prev.device, db_count: count } } : prev,
    );
  }, []);

  const syncBoardFaceForDevice = useCallback(
    async (deviceId: string) => {
      setFaceBoardBusy(true);
      setGalleryErr(null);
      try {
        await api.postCommand(deviceId, "db-count", null);
        try {
          const gal = await api.getFaceGallery(deviceId);
          const ent = Array.isArray(gal.entries) ? gal.entries : [];
          setGalleryEntries(ent);
          applyGalleryDerivedDbCount(deviceId, ent.length);
        } catch (ge) {
          setGalleryErr((ge as Error).message);
          setGalleryEntries([]);
        }
        await loadSelected(deviceId);
      } catch (e) {
        setGalleryErr((e as Error).message);
        await loadSelected(deviceId);
      } finally {
        setFaceBoardBusy(false);
      }
    },
    [applyGalleryDerivedDbCount, loadSelected],
  );

  const loadSdAttendanceLog = useCallback(async () => {
    if (!selectedDevice) {
      alert("请先在顶栏选择设备");
      return;
    }
    setSdLogLoading(true);
    setSdLogErr(null);
    try {
      const data = await api.getSdAttendanceLog(selectedDevice, {
        date: sdLogDate,
        timeoutSec: 35,
        tailLines: 1200,
        maxBytes: 262144,
      });
      setSdLogData(data);
    } catch (e) {
      setSdLogErr((e as Error).message);
      setSdLogData(null);
    } finally {
      setSdLogLoading(false);
    }
  }, [selectedDevice, sdLogDate]);

  useEffect(() => {
    if (!selectedDevice) {
      setGalleryEntries([]);
      setGalleryErr(null);
      return;
    }
    void syncBoardFaceForDevice(selectedDevice);
  }, [selectedDevice, syncBoardFaceForDevice]);

  useEffect(() => {
    setSdLogData(null);
    setSdLogErr(null);
    setSdLogDate(todayLocalYmd());
  }, [selectedDevice]);

  const refreshAll = useCallback(async () => {
    const dev = await loadDevices();
    if (dev) await syncBoardFaceForDevice(dev);
  }, [loadDevices, syncBoardFaceForDevice]);

  const connectWs = useCallback(() => {
    const protocol = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${protocol}://${location.host}/ws`);
    wsRef.current = ws;
    ws.onopen = () => {
      setWsStatus("ok");
    };
    ws.onclose = () => {
      setWsStatus("bad");
      setTimeout(() => connectWsRef.current(), 1000);
    };
    ws.onmessage = async (event) => {
      const msg = JSON.parse(event.data) as WsMessage;
      if (msg.type === "snapshot") {
        setDevices(msg.devices);
        if (msg.devices.length === 0) {
          setSelectedDevice(null);
        } else {
          setSelectedDevice((prev) => {
            const ids = msg.devices.map((d) => d.device_id);
            if (prev && ids.includes(prev)) {
              return prev;
            }
            return msg.devices[0].device_id;
          });
        }
        if (selectedRef.current) {
          await syncBoardFaceForDevice(selectedRef.current);
        }
        return;
      }
      if (msg.device_id !== selectedRef.current) {
        await loadDevices();
        return;
      }
      if (msg.type === "event") {
        setEvents((prev) => {
          const next = [msg.payload, ...prev];
          return next.slice(0, 100);
        });
      } else if (msg.type === "status" && msg.device) {
        setDevices((prev) => prev.map((d) => (d.device_id === msg.device_id ? msg.device! : d)));
        setDeviceState((prev) =>
          prev && prev.device.device_id === msg.device_id ? { ...prev, device: msg.device! } : prev,
        );
      } else if (msg.type === "reply" || msg.type === "status") {
        if (msg.type === "reply") {
          const p = msg.payload as Record<string, unknown>;
          const cmd = typeof p.cmd === "string" ? p.cmd : "";
          if (cmd === "db_reset") {
            await syncBoardFaceForDevice(msg.device_id);
            return;
          }
        }
        await loadSelected(msg.device_id);
      }
    };
  }, [loadDevices, loadSelected, syncBoardFaceForDevice]);

  useEffect(() => {
    connectWsRef.current = connectWs;
  }, [connectWs]);

  useEffect(() => {
    void refreshAll().catch((e) => {
      console.error(e);
      alert((e as Error).message);
    });
    connectWs();
    return () => {
      wsRef.current?.close();
    };
  }, [connectWs, refreshAll]);

  const onDeviceChange = (e: React.ChangeEvent<HTMLSelectElement>) => {
    setSelectedDevice(e.target.value || null);
  };

  const onClearWeb = async () => {
    if (!confirm("确定要清空当前网页保存的本地设备、命令和事件记录吗？这不会清空开发板的人脸库。")) {
      return;
    }
    try {
      const result = await api.postWebDataClear();
      const cleared = result.cleared ?? {};
      alert(
        `已清空：设备 ${cleared.devices ?? 0} 条、命令 ${cleared.commands ?? 0} 条、事件 ${cleared.events ?? 0} 条`,
      );
      await refreshAll();
    } catch (e) {
      console.error(e);
      alert((e as Error).message);
    }
  };

  const postCmd = async (path: string, body: Record<string, string> | null = null) => {
    if (!selectedDevice) {
      alert("请先在顶栏选择设备");
      return;
    }
    await api.postCommand(selectedDevice, path, body);
    if (path === "db-reset") {
      await syncBoardFaceForDevice(selectedDevice);
    } else {
      await loadSelected(selectedDevice);
    }
  };

  const doCloseRegisterMqtt = useCallback(async () => {
    if (!selectedRef.current) {
      return;
    }
    if (sessionRef.current) {
      await api.postCommand(selectedRef.current, "register-cancel", null);
    }
  }, []);

  const closeDialogUi = useCallback((programmatic: boolean) => {
    if (programmatic) {
      dismissProgrammatic.current = true;
    }
    setRegisterOpen(false);
    setRegName("");
    setRegisterSessionOpen(false);
  }, []);

  const startRegister = async () => {
    if (registerSessionOpen || !selectedDevice) {
      if (!selectedDevice) {
        alert("请先在顶栏选择设备");
      }
      return;
    }
    setRegisterPreviewLoading(true);
    try {
      await api.postCommand(selectedDevice, "register-preview", null);
      setRegisterSessionOpen(true);
      setRegName("");
      setRegisterOpen(true);
    } catch (e) {
      console.error(e);
      alert((e as Error).message);
    } finally {
      setRegisterPreviewLoading(false);
    }
  };

  const onDialogRequestClose = useCallback(
    async (e: SlRequestCloseEvent) => {
      if (dismissProgrammatic.current) {
        dismissProgrammatic.current = false;
        return;
      }
      e.preventDefault();
      try {
        await doCloseRegisterMqtt();
        closeDialogUi(true);
        if (selectedRef.current) {
          await loadSelected(selectedRef.current);
        }
      } catch (err) {
        console.error(err);
        alert((err as Error).message);
      }
    },
    [closeDialogUi, doCloseRegisterMqtt, loadSelected],
  );

  const onRegisterCancel = useCallback(() => {
    void (async () => {
      try {
        await doCloseRegisterMqtt();
        dismissProgrammatic.current = true;
        setRegisterOpen(false);
        setRegName("");
        setRegisterSessionOpen(false);
        if (selectedRef.current) {
          await loadSelected(selectedRef.current);
        }
      } catch (err) {
        console.error(err);
        alert((err as Error).message);
      }
    })();
  }, [doCloseRegisterMqtt, loadSelected]);

  const onRegisterOk = useCallback(() => {
    const name = regName.trim();
    if (!name) {
      alert("请输入注册姓名");
      return;
    }
    const dev = selectedDevice;
    if (!dev) {
      return;
    }
    void (async () => {
      try {
        await api.postCommand(dev, "register-commit", { name });
        dismissProgrammatic.current = true;
        setRegisterOpen(false);
        setRegName("");
        setRegisterSessionOpen(false);
        await syncBoardFaceForDevice(dev);
      } catch (err) {
        console.error(err);
        alert((err as Error).message);
      }
    })();
  }, [regName, selectedDevice, syncBoardFaceForDevice]);

  const d = deviceState?.device;

  return (
    <div className="app">
      <header className="app-header">
        <div className="app-header__brand">
          <div>
            <h1 className="app-title">K230 人脸设备网管</h1>
            <p className="app-subtitle">设备遥测 · 事件审计 · 远程控制（MQTT / face_netd）</p>
          </div>
        </div>
        <div className="app-header__tools">
          <span
            className={`pill ${wsStatus === "ok" ? "pill--ok" : "pill--bad"}`}
            title="与后端 WebSocket 长连接"
          >
            <SlIcon name={wsStatus === "ok" ? "wifi" : "wifi-off"} style={{ fontSize: "0.9em" }} />
            实时连接 {wsStatus === "ok" ? "正常" : "断开"}
          </span>
          <div className="app-header__device">
            <span className="app-header__device-label" id="hdr-device-lbl">
              设备
            </span>
            <select
              className="app-header__select"
              value={selectedDevice ?? ""}
              onChange={onDeviceChange}
              aria-labelledby="hdr-device-lbl"
            >
              {devices.length === 0 && <option value="">未上报设备</option>}
              {devices.map((dev) => (
                <option key={dev.device_id} value={dev.device_id}>
                  {dev.device_id}
                </option>
              ))}
            </select>
          </div>
          <button type="button" className="app-header__btn" onClick={() => void refreshAll()}>
            同步数据
          </button>
          <button type="button" className="app-header__btn app-header__btn--danger" onClick={() => void onClearWeb()}>
            清空本地记录
          </button>
        </div>
      </header>

      <main className="app-main">
        <section className="app-section">
          {!d && <div className="panel empty-state">尚未选择设备或暂无设备数据，请确认板端已连 MQTT 并上报状态。</div>}
          {d && (
            <div className="dashboard-toolbar">
              <div className="dashboard-toolbar__top">
                <section className="panel panel--control-only dashboard-toolbar__controls">
                  <div className="panel__head">
                    <h2 className="panel__title">控制指令</h2>
                  </div>
                  <div className="panel__body">
                    <div className="controls-stack">
                      <div className="btn-row">
                        <button
                          type="button"
                          className="btn--primary"
                          disabled={!selectedDevice || faceBoardBusy}
                          onClick={() => selectedDevice && void syncBoardFaceForDevice(selectedDevice)}
                        >
                          {faceBoardBusy ? "同步中…" : "同步人脸库（人数 · 快照）"}
                        </button>
                        <button
                          type="button"
                          className="btn--danger"
                          onClick={() => {
                            if (confirm("确定清空当前设备上的人脸库？此操作在板端执行，不可从网页撤销。")) {
                              void postCmd("db-reset");
                            }
                          }}
                        >
                          清库
                        </button>
                      </div>

                      <SlButton
                        className="register-face-btn"
                        variant="primary"
                        size="large"
                        disabled={!selectedDevice}
                        loading={registerPreviewLoading}
                        onClick={() => void startRegister()}
                      >
                        <SlIcon name="person-plus" slot="prefix" />
                        注册人脸（抓拍预览）
                      </SlButton>

                      <button
                        type="button"
                        className="btn--danger"
                        style={{ width: "100%" }}
                        onClick={() => {
                          if (confirm("将向板端发送关闭指令，确定继续？")) {
                            void postCmd("shutdown");
                          }
                        }}
                      >
                        关闭板端三进程
                      </button>
                    </div>
                  </div>
                </section>

                <div className="metric-row metric-row--2x2 dashboard-toolbar__metrics" aria-label="运行状态">
                  <div className="metric-card">
                    <span className="metric-card__label">小核在线</span>
                    <span className={`metric-card__value ${d.online ? "ok" : "bad"}`}>{d.online ? "在线" : "离线"}</span>
                  </div>
                  <div className="metric-card">
                    <span className="metric-card__label">RT 桥接</span>
                    <span
                      className={`metric-card__value ${d.online ? (d.rt_connected ? "ok" : "bad") : "dim"}`}
                      title={d.online ? undefined : "小核离线时不展示 RT 桥接实时状态"}
                    >
                      {d.online ? (d.rt_connected ? "已连接" : "未连接") : "未知"}
                    </span>
                  </div>
                  <div className="metric-card">
                    <span className="metric-card__label">库内人数</span>
                    <span className="metric-card__value">{d.db_count >= 0 ? d.db_count : "—"}</span>
                  </div>
                  <div className="metric-card">
                    <span className="metric-card__label">最后通信</span>
                    <span className="metric-card__value dim" title="服务端收到最近一条状态的时间">
                      {fmtTs(d.last_seen_ms)}
                    </span>
                  </div>
                </div>
              </div>

              {selectedDevice && (
                <div className="overview-gallery dashboard-toolbar__gallery">
                  <div className="overview-gallery__head">
                    <h3 className="overview-gallery__title">人脸注册库</h3>
                    <span className="overview-gallery__badge">
                      {faceBoardBusy ? "同步中…" : `已列出 ${galleryEntries.length} 人`}
                    </span>
                  </div>
                  {galleryErr && <p className="face-gallery-error">{escapeHtml(galleryErr)}</p>}
                  {!galleryErr && galleryEntries.length === 0 && !faceBoardBusy && (
                    <p className="overview-gallery__hint">
                      暂无条目或仅有旧版库（无 `.jpg`）。完成一次注册后会写入与板端预览同源的全景快照；请确认 face_netd.ini 的{" "}
                      <code>face_db_dir</code> 与 face_ai 的 db 路径一致。
                    </p>
                  )}
                  {galleryEntries.length > 0 && (
                    <div className="face-gallery-grid face-gallery-grid--in-overview">
                      {galleryEntries.map((en) => (
                        <div className="face-gallery-card" key={en.slot}>
                          <div className="face-gallery-thumb-wrap">
                            {en.has_image ? (
                              <img
                                src={api.faceGalleryPhotoUrl(selectedDevice, en.slot)}
                                alt=""
                                loading="lazy"
                                decoding="async"
                              />
                            ) : (
                              <div className="face-gallery-thumb-missing">无快照</div>
                            )}
                          </div>
                          <div className="face-gallery-meta">
                            <div className="face-gallery-slot">#{escapeHtml(en.slot)}</div>
                            <div className="face-gallery-name">{escapeHtml(en.name || "—")}</div>
                          </div>
                        </div>
                      ))}
                    </div>
                  )}
                </div>
              )}
            </div>
          )}
        </section>

        <div className="app-grid-2">
          <section className="panel panel--flush">
            <div className="panel__head">
              <h2 className="panel__title">指令流水</h2>
            </div>
            {(deviceState?.recent_commands ?? []).length === 0 ? (
              <p className="empty-state">无记录（下发命令后在此追踪状态）</p>
            ) : (
              <div className="data-table">
                <div className="data-table__thead">
                  <span>命令 / 说明</span>
                  <span>状态</span>
                </div>
                {(deviceState?.recent_commands ?? []).map((row) => (
                  <div className="data-table__row" key={row.request_id}>
                    <div>
                      <div className="data-table__primary">{escapeHtml(row.cmd)}</div>
                      <p className="data-table__secondary">{escapeHtml(row.message || "—")}</p>
                    </div>
                    <div className="data-table__meta">
                      <span
                        className={`badge ${row.ok === 1 ? "ok" : row.ok === 0 ? "bad" : ""}`}
                        title="服务端记录的状态"
                      >
                        {escapeHtml(row.status)}
                      </span>
                      <span className="data-table__rid">{escapeHtml(row.request_id)}</span>
                    </div>
                  </div>
                ))}
              </div>
            )}
          </section>

          <section className="panel panel--flush">
            <div className="panel__head">
              <h2 className="panel__title">识别事件</h2>
            </div>
            {events.length === 0 ? (
              <p className="empty-state">无最近事件</p>
            ) : (
              <div className="data-table data-table--events">
                <div className="data-table__thead">
                  <span>类型 / 人员</span>
                  <span>分数</span>
                  <span>时间</span>
                </div>
                {events.map((ev, idx) => (
                  <div className="data-table__row" key={`${ev.ts_ms}-${idx}`}>
                    <div>
                      <div className="data-table__primary" style={{ fontFamily: "inherit" }}>
                        {escapeHtml(ev.evt_kind)}
                      </div>
                      <p className="data-table__secondary">
                        {escapeHtml(ev.name || "unknown")} / id {escapeHtml(ev.face_id)}
                      </p>
                    </div>
                    <div className="data-table__secondary">{Number(ev.score).toFixed(3)}</div>
                    <div className="data-table__meta">
                      {escapeHtml(fmtTs(eventDisplayMs_ms(ev)))}
                    </div>
                  </div>
                ))}
              </div>
            )}
          </section>
        </div>

        <section className="panel panel--flush attendance-sd-panel">
          <div className="panel__head attendance-sd-panel__head">
            <h2 className="panel__title">TF 卡考勤日志（按日 JSONL）</h2>
            <div className="attendance-sd-panel__controls">
              <label className="attendance-sd-date-field">
                <span className="attendance-sd-date-field__lbl">日期</span>
                <input
                  className="attendance-sd-date-field__input"
                  type="date"
                  value={sdLogDate}
                  max={todayLocalYmd()}
                  onChange={(e) => setSdLogDate(e.target.value)}
                  disabled={!selectedDevice || sdLogLoading}
                />
              </label>
              <button
                type="button"
                className="btn--primary attendance-sd-panel__btn"
                disabled={!selectedDevice || sdLogLoading}
                onClick={() => void loadSdAttendanceLog()}
              >
                {sdLogLoading ? "读取中…" : "读取当日日志"}
              </button>
            </div>
          </div>
          {sdLogErr && <p className="face-gallery-error attendance-sd-panel__hint">{escapeHtml(sdLogErr)}</p>}
          {!sdLogErr && !sdLogData && (
            <p className="empty-state attendance-sd-panel__hint">
              考勤 JSONL 路径为 <code>&lt;attendance_log_base&gt;/&lt;YYYY-MM-DD&gt;.jsonl</code>
              （例如 TF 上 <code>/mnt/tf/face_logs/2026-05-06.jsonl</code>）。挂载{" "}
              <code>/dev/mmcblk1p1</code> 后，将 <code>face_netd.ini</code> 中{" "}
              <code>attendance_log_base</code> 指到根目录（如 <code>/mnt/tf/face_logs</code>
              ）。以下为 MQTT <code>attendance_log_fetch</code> 拉取的当日结构化日志。
            </p>
          )}
          {sdLogData && (
            <div className="attendance-sd-panel__meta">
              <span title="板端日期">{escapeHtml(sdLogData.date ?? sdLogDate)}</span>
              <span title="板端返回的路径">{escapeHtml(sdLogData.path ?? "—")}</span>
              <span className="attendance-sd-meta-sub">
                {sdLogData.file_size != null ? `文件约 ${sdLogData.file_size} 字节` : ""}
                {sdLogData.bytes_returned != null ? ` · 本次返回 ${sdLogData.bytes_returned} 字节` : ""}
                {sdLogData.truncated ? " · 仅末尾片段" : ""}
              </span>
            </div>
          )}
          {sdLogData && (
            <pre className="attendance-sd-pre" dir="ltr">
              {sdLogData.content || "（空文件）"}
            </pre>
          )}
        </section>
      </main>

      <SlDialog
        className="register-dialog"
        label="完成注册"
        open={registerOpen}
        onSlRequestClose={onDialogRequestClose}
      >
        <div className="register-dialog__body">
          <p className="register-dialog__lead">
            已抓拍当前画面。请输入与终端第二行相同的姓名，提交后由板端执行入库（受活体等策略影响）。
          </p>
          <label className="field">
            <span>注册姓名</span>
            <input
              className="register-name-input"
              value={regName}
              onChange={(e) => setRegName(e.target.value)}
              maxLength={63}
              placeholder="例如：张三"
              autoComplete="off"
            />
          </label>
        </div>
        <div slot="footer" className="register-dialog__footer">
          <SlButton variant="neutral" onClick={onRegisterCancel}>
            取消
          </SlButton>
          <SlButton variant="primary" onClick={onRegisterOk}>
            <SlIcon name="check-lg" slot="prefix" />
            提交注册
          </SlButton>
        </div>
      </SlDialog>
    </div>
  );
}
