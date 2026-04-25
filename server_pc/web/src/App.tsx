import { useCallback, useEffect, useRef, useState } from "react";
import SlButton from "@shoelace-style/shoelace/dist/react/button/index.js";
import SlDialog from "@shoelace-style/shoelace/dist/react/dialog/index.js";
import type { SlRequestCloseEvent } from "@shoelace-style/shoelace/dist/react/dialog/index.js";
import SlIcon from "@shoelace-style/shoelace/dist/react/icon/index.js";
import * as api from "./api";
import type { CommandRow, DeviceRow, EventRow, WsMessage } from "./types";

function fmtTs(ms: number) {
  if (!ms) return "—";
  return new Date(ms).toLocaleString();
}

function escapeHtml(value: string | number | null | undefined) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
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

  const loadDevices = useCallback(async () => {
    const data = await api.getDevices();
    setDevices(data.devices);
    if (data.devices.length === 0) {
      setSelectedDevice(null);
      return;
    }
    setSelectedDevice((prev) => {
      const ids = data.devices.map((d) => d.device_id);
      if (prev && ids.includes(prev)) {
        return prev;
      }
      return data.devices[0].device_id;
    });
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

  const refreshAll = useCallback(async () => {
    await loadDevices();
    if (selectedRef.current) {
      await loadSelected(selectedRef.current);
    }
  }, [loadDevices, loadSelected]);

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
          await loadSelected(selectedRef.current);
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
        await loadSelected(msg.device_id);
      }
    };
  }, [loadDevices, loadSelected]);

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
    await loadSelected(selectedDevice);
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
        await loadSelected(dev);
      } catch (err) {
        console.error(err);
        alert((err as Error).message);
      }
    })();
  }, [regName, selectedDevice, loadSelected]);

  const d = deviceState?.device;

  return (
    <div className="app">
      <header className="app-header">
        <div className="app-header__brand">
          <div className="app-logo" aria-hidden>
            <SlIcon name="router" />
          </div>
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
          <div className="app-section__head">
            <h2 className="app-section__title">运行概览</h2>
            <p className="app-section__desc">与板端 `face_netd` 上报的状态同步，用于判断在线与算力侧连接情况。</p>
          </div>
          {!d && <div className="panel empty-state">尚未选择设备或暂无设备数据，请确认板端已连 MQTT 并上报状态。</div>}
          {d && (
            <div className="metric-row">
              <div className="metric-card">
                <span className="metric-card__label">小核在线</span>
                <span className={`metric-card__value ${d.online ? "ok" : "bad"}`}>{d.online ? "在线" : "离线"}</span>
              </div>
              <div className="metric-card">
                <span className="metric-card__label">RT 桥接</span>
                <span className={`metric-card__value ${d.rt_connected ? "ok" : "bad"}`}>
                  {d.rt_connected ? "已连接" : "未连接"}
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
          )}
        </section>

        <section className="panel panel--control-only">
          <div className="panel__head">
            <h2 className="panel__title">控制指令</h2>
          </div>
          <div className="panel__body">
            <div className="controls-stack">
              <div className="btn-row">
                <button type="button" className="btn--primary" onClick={() => void postCmd("db-count")}>
                  查询人数
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
                      {escapeHtml(fmtTs(ev.ts_ms))}
                    </div>
                  </div>
                ))}
              </div>
            )}
          </section>
        </div>
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
