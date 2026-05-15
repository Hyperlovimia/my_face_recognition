import { useCallback, useEffect, useRef, useState } from "react";
import SlButton from "@shoelace-style/shoelace/dist/react/button/index.js";
import SlDialog from "@shoelace-style/shoelace/dist/react/dialog/index.js";
import type { SlRequestCloseEvent } from "@shoelace-style/shoelace/dist/react/dialog/index.js";
import SlIcon from "@shoelace-style/shoelace/dist/react/icon/index.js";
import * as api from "./api";
import type { SdAttendanceLogResponse } from "./api";
import type { CommandRow, DeviceRow, EventRow, FaceGalleryEntry, WsMessage } from "./types";

/** 设备未校时时 ts_ms 接近 1970；优先信任服务端时间 */
const MIN_SANE_DEVICE_TS_MS = 1_000_000_000_000;
const PAGE_SIZE = 20;

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

function totalPages(total: number): number {
  return Math.max(1, Math.ceil(total / PAGE_SIZE));
}

function PaginationControls({
  page,
  total,
  onPageChange,
}: {
  page: number;
  total: number;
  onPageChange: (page: number) => void;
}) {
  const pages = totalPages(total);
  if (total <= PAGE_SIZE) {
    return null;
  }
  return (
    <div className="list-pagination" aria-label="分页">
      <span className="list-pagination__summary">
        第 {page} / {pages} 页 · 共 {total} 条
      </span>
      <div className="list-pagination__actions">
        <button type="button" className="list-pagination__btn" disabled={page <= 1} onClick={() => onPageChange(page - 1)}>
          上一页
        </button>
        <button
          type="button"
          className="list-pagination__btn"
          disabled={page >= pages}
          onClick={() => onPageChange(page + 1)}
        >
          下一页
        </button>
      </div>
    </div>
  );
}

export function App() {
  const [authenticated, setAuthenticated] = useState(false);
  const [authChecking, setAuthChecking] = useState(true);
  const [authSubmitting, setAuthSubmitting] = useState(false);
  const [authError, setAuthError] = useState<string | null>(null);
  const [loginPassword, setLoginPassword] = useState("");
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [selectedDevice, setSelectedDevice] = useState<string | null>(null);
  const [deviceState, setDeviceState] = useState<{ device: DeviceRow } | null>(null);
  const [commandRows, setCommandRows] = useState<CommandRow[]>([]);
  const [commandTotal, setCommandTotal] = useState(0);
  const [commandPage, setCommandPage] = useState(1);
  const [eventRows, setEventRows] = useState<EventRow[]>([]);
  const [eventTotal, setEventTotal] = useState(0);
  const [eventPage, setEventPage] = useState(1);
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
  const commandPageRef = useRef(1);
  const eventPageRef = useRef(1);
  const wsRef = useRef<WebSocket | null>(null);
  const connectWsRef = useRef<() => void>(() => {});
  const wsReconnectTimerRef = useRef<number | null>(null);
  const authRef = useRef(false);

  useEffect(() => {
    selectedRef.current = selectedDevice;
  }, [selectedDevice]);

  useEffect(() => {
    sessionRef.current = registerSessionOpen;
  }, [registerSessionOpen]);

  useEffect(() => {
    commandPageRef.current = commandPage;
  }, [commandPage]);

  useEffect(() => {
    eventPageRef.current = eventPage;
  }, [eventPage]);

  useEffect(() => {
    authRef.current = authenticated;
  }, [authenticated]);

  const clearWsReconnectTimer = useCallback(() => {
    if (wsReconnectTimerRef.current !== null) {
      window.clearTimeout(wsReconnectTimerRef.current);
      wsReconnectTimerRef.current = null;
    }
  }, []);

  const closeWs = useCallback(() => {
    clearWsReconnectTimer();
    const ws = wsRef.current;
    wsRef.current = null;
    if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) {
      ws.close();
    }
  }, [clearWsReconnectTimer]);

  const resetDashboardState = useCallback(() => {
    dismissProgrammatic.current = false;
    selectedRef.current = null;
    sessionRef.current = false;
    commandPageRef.current = 1;
    eventPageRef.current = 1;
    setDevices([]);
    setSelectedDevice(null);
    setDeviceState(null);
    setCommandRows([]);
    setCommandTotal(0);
    setCommandPage(1);
    setEventRows([]);
    setEventTotal(0);
    setEventPage(1);
    setWsStatus("bad");
    setRegisterOpen(false);
    setRegName("");
    setRegisterSessionOpen(false);
    setRegisterPreviewLoading(false);
    setGalleryEntries([]);
    setGalleryErr(null);
    setFaceBoardBusy(false);
    setSdLogData(null);
    setSdLogErr(null);
    setSdLogLoading(false);
    setSdLogDate(todayLocalYmd());
  }, []);

  const enterLoggedOutState = useCallback(
    (message: string | null) => {
      authRef.current = false;
      closeWs();
      resetDashboardState();
      setAuthenticated(false);
      setAuthChecking(false);
      setAuthSubmitting(false);
      setAuthError(message);
    },
    [closeWs, resetDashboardState],
  );

  const handleUnauthorized = useCallback(
    (error: unknown, message = "登录已失效，请重新输入管理员密码。") => {
      if (!api.isUnauthorizedError(error)) {
        return false;
      }
      enterLoggedOutState(message);
      return true;
    },
    [enterLoggedOutState],
  );

  const showError = useCallback((error: unknown) => {
    console.error(error);
    alert((error as Error).message);
  }, []);

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

  const loadDeviceState = useCallback(async (deviceId: string | null) => {
    if (!deviceId) {
      setDeviceState(null);
      return;
    }
    const st = await api.getDeviceState(deviceId);
    setDeviceState({ device: st.device });
  }, []);

  const loadCommandPage = useCallback(async (deviceId: string, page: number) => {
    const requestedPage = Math.max(1, page);
    let data = await api.getDeviceCommands(deviceId, PAGE_SIZE, (requestedPage - 1) * PAGE_SIZE);
    const safePage = Math.min(requestedPage, totalPages(data.total));
    if (safePage !== requestedPage) {
      data = await api.getDeviceCommands(deviceId, PAGE_SIZE, (safePage - 1) * PAGE_SIZE);
      setCommandPage(safePage);
    }
    setCommandRows(data.items);
    setCommandTotal(data.total);
  }, []);

  const loadEventPage = useCallback(async (deviceId: string, page: number) => {
    const requestedPage = Math.max(1, page);
    let data = await api.getDeviceEvents(deviceId, PAGE_SIZE, (requestedPage - 1) * PAGE_SIZE);
    const safePage = Math.min(requestedPage, totalPages(data.total));
    if (safePage !== requestedPage) {
      data = await api.getDeviceEvents(deviceId, PAGE_SIZE, (safePage - 1) * PAGE_SIZE);
      setEventPage(safePage);
    }
    setEventRows(data.items);
    setEventTotal(data.total);
  }, []);

  const loadSelected = useCallback(
    async (
      deviceId: string | null,
      pages?: {
        commandPage?: number;
        eventPage?: number;
      },
    ) => {
      if (!deviceId) {
        setDeviceState(null);
        setCommandRows([]);
        setCommandTotal(0);
        setEventRows([]);
        setEventTotal(0);
        return;
      }
      await Promise.all([
        loadDeviceState(deviceId),
        loadCommandPage(deviceId, pages?.commandPage ?? commandPageRef.current),
        loadEventPage(deviceId, pages?.eventPage ?? eventPageRef.current),
      ]);
    },
    [loadCommandPage, loadDeviceState, loadEventPage],
  );

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
        } catch (error) {
          if (handleUnauthorized(error)) {
            return;
          }
          setGalleryErr((error as Error).message);
          setGalleryEntries([]);
        }
        await loadSelected(deviceId);
      } catch (error) {
        if (handleUnauthorized(error)) {
          return;
        }
        setGalleryErr((error as Error).message);
        try {
          await loadSelected(deviceId);
        } catch (loadError) {
          if (!handleUnauthorized(loadError)) {
            throw loadError;
          }
        }
      } finally {
        setFaceBoardBusy(false);
      }
    },
    [applyGalleryDerivedDbCount, handleUnauthorized, loadSelected],
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
    } catch (error) {
      if (handleUnauthorized(error)) {
        return;
      }
      setSdLogErr((error as Error).message);
      setSdLogData(null);
    } finally {
      setSdLogLoading(false);
    }
  }, [handleUnauthorized, sdLogDate, selectedDevice]);

  const refreshAll = useCallback(async () => {
    const dev = await loadDevices();
    if (dev) {
      await syncBoardFaceForDevice(dev);
    }
  }, [loadDevices, syncBoardFaceForDevice]);

  const scheduleWsReconnect = useCallback(() => {
    clearWsReconnectTimer();
    if (!authRef.current) {
      return;
    }
    wsReconnectTimerRef.current = window.setTimeout(() => {
      void (async () => {
        if (!authRef.current) {
          return;
        }
        try {
          await api.getSession();
          connectWsRef.current();
        } catch (error) {
          if (handleUnauthorized(error)) {
            return;
          }
          scheduleWsReconnect();
        }
      })();
    }, 1000);
  }, [clearWsReconnectTimer, handleUnauthorized]);

  const connectWs = useCallback(() => {
    if (!authRef.current) {
      return;
    }
    const current = wsRef.current;
    if (current && (current.readyState === WebSocket.CONNECTING || current.readyState === WebSocket.OPEN)) {
      return;
    }
    clearWsReconnectTimer();
    const protocol = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${protocol}://${location.host}/ws`);
    wsRef.current = ws;
    ws.onopen = () => {
      setWsStatus("ok");
    };
    ws.onclose = () => {
      if (wsRef.current === ws) {
        wsRef.current = null;
      }
      setWsStatus("bad");
      if (!authRef.current) {
        return;
      }
      scheduleWsReconnect();
    };
    ws.onmessage = (event) => {
      void (async () => {
        try {
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
            await loadEventPage(msg.device_id, eventPageRef.current);
          } else if (msg.type === "status" && msg.device) {
            setDevices((prev) => prev.map((d) => (d.device_id === msg.device_id ? msg.device! : d)));
            setDeviceState((prev) =>
              prev && prev.device.device_id === msg.device_id ? { ...prev, device: msg.device! } : prev,
            );
          } else if (msg.type === "reply" || msg.type === "status") {
            if (msg.type === "reply") {
              const p = msg.payload as Record<string, unknown>;
              const cmd = typeof p.cmd === "string" ? p.cmd : "";
              if (cmd === "db_reset" || cmd === "import_faces") {
                await syncBoardFaceForDevice(msg.device_id);
                return;
              }
            }
            await loadSelected(msg.device_id);
          }
        } catch (error) {
          if (!handleUnauthorized(error)) {
            console.error(error);
          }
        }
      })();
    };
  }, [clearWsReconnectTimer, handleUnauthorized, loadDevices, loadEventPage, loadSelected, scheduleWsReconnect, syncBoardFaceForDevice]);

  useEffect(() => {
    connectWsRef.current = connectWs;
  }, [connectWs]);

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      setAuthChecking(true);
      try {
        await api.getSession();
        if (cancelled) {
          return;
        }
        authRef.current = true;
        setAuthenticated(true);
        setAuthError(null);
      } catch (error) {
        if (cancelled) {
          return;
        }
        if (api.isUnauthorizedError(error)) {
          enterLoggedOutState(null);
        } else {
          enterLoggedOutState((error as Error).message);
        }
      } finally {
        if (!cancelled) {
          setAuthChecking(false);
        }
      }
    })();
    return () => {
      cancelled = true;
      authRef.current = false;
      closeWs();
    };
  }, [closeWs, enterLoggedOutState]);

  useEffect(() => {
    if (!authenticated) {
      return;
    }
    void refreshAll().catch((error) => {
      if (!handleUnauthorized(error)) {
        showError(error);
      }
    });
    connectWs();
    return () => {
      closeWs();
    };
  }, [authenticated, closeWs, connectWs, handleUnauthorized, refreshAll, showError]);

  useEffect(() => {
    if (!authenticated) {
      return;
    }
    if (selectedDevice) {
      setCommandPage(1);
      setEventPage(1);
      void loadSelected(selectedDevice, { commandPage: 1, eventPage: 1 }).catch((error) => {
        if (!handleUnauthorized(error)) {
          showError(error);
        }
      });
    } else {
      void loadSelected(null);
    }
  }, [authenticated, handleUnauthorized, loadSelected, selectedDevice, showError]);

  useEffect(() => {
    if (!authenticated) {
      return;
    }
    if (!selectedDevice) {
      setGalleryEntries([]);
      setGalleryErr(null);
      return;
    }
    void syncBoardFaceForDevice(selectedDevice).catch((error) => {
      if (!handleUnauthorized(error)) {
        showError(error);
      }
    });
  }, [authenticated, handleUnauthorized, selectedDevice, showError, syncBoardFaceForDevice]);

  useEffect(() => {
    setSdLogData(null);
    setSdLogErr(null);
    setSdLogDate(todayLocalYmd());
  }, [selectedDevice]);

  const onDeviceChange = (e: React.ChangeEvent<HTMLSelectElement>) => {
    setSelectedDevice(e.target.value || null);
  };

  const onCommandPageChange = useCallback(
    (page: number) => {
      if (!selectedDevice) return;
      setCommandPage(page);
      void loadCommandPage(selectedDevice, page).catch((error) => {
        if (!handleUnauthorized(error)) {
          showError(error);
        }
      });
    },
    [handleUnauthorized, loadCommandPage, selectedDevice, showError],
  );

  const onEventPageChange = useCallback(
    (page: number) => {
      if (!selectedDevice) return;
      setEventPage(page);
      void loadEventPage(selectedDevice, page).catch((error) => {
        if (!handleUnauthorized(error)) {
          showError(error);
        }
      });
    },
    [handleUnauthorized, loadEventPage, selectedDevice, showError],
  );

  const onClearWeb = useCallback(async () => {
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
    } catch (error) {
      if (!handleUnauthorized(error)) {
        showError(error);
      }
    }
  }, [handleUnauthorized, refreshAll, showError]);

  const postCmd = useCallback(
    async (path: string, body: Record<string, string> | null = null) => {
      if (!selectedDevice) {
        alert("请先在顶栏选择设备");
        return;
      }
      try {
        await api.postCommand(selectedDevice, path, body);
        if (path === "db-reset") {
          await syncBoardFaceForDevice(selectedDevice);
        } else {
          await loadSelected(selectedDevice);
        }
      } catch (error) {
        if (handleUnauthorized(error)) {
          return;
        }
        throw error;
      }
    },
    [handleUnauthorized, loadSelected, selectedDevice, syncBoardFaceForDevice],
  );

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

  const startRegister = useCallback(async () => {
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
    } catch (error) {
      if (!handleUnauthorized(error)) {
        showError(error);
      }
    } finally {
      setRegisterPreviewLoading(false);
    }
  }, [handleUnauthorized, registerSessionOpen, selectedDevice, showError]);

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
      } catch (error) {
        if (!handleUnauthorized(error)) {
          showError(error);
        }
      }
    },
    [closeDialogUi, doCloseRegisterMqtt, handleUnauthorized, loadSelected, showError],
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
      } catch (error) {
        if (!handleUnauthorized(error)) {
          showError(error);
        }
      }
    })();
  }, [doCloseRegisterMqtt, handleUnauthorized, loadSelected, showError]);

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
      } catch (error) {
        if (!handleUnauthorized(error)) {
          showError(error);
        }
      }
    })();
  }, [handleUnauthorized, regName, selectedDevice, showError, syncBoardFaceForDevice]);

  const onLoginSubmit = useCallback(
    (event: React.FormEvent<HTMLFormElement>) => {
      event.preventDefault();
      if (!loginPassword) {
        setAuthError("请输入管理员密码。");
        return;
      }
      setAuthSubmitting(true);
      setAuthError(null);
      void (async () => {
        try {
          await api.login(loginPassword);
          authRef.current = true;
          setAuthenticated(true);
          setLoginPassword("");
        } catch (error) {
          if (api.isUnauthorizedError(error)) {
            authRef.current = false;
            setAuthenticated(false);
            setAuthError("密码错误，请重试。");
          } else {
            setAuthError((error as Error).message);
          }
        } finally {
          setAuthChecking(false);
          setAuthSubmitting(false);
        }
      })();
    },
    [loginPassword],
  );

  const onLogout = useCallback(() => {
    void (async () => {
      try {
        await doCloseRegisterMqtt();
      } catch (error) {
        if (!handleUnauthorized(error)) {
          console.error(error);
        } else {
          return;
        }
      }
      try {
        await api.logout();
      } catch (error) {
        if (!api.isUnauthorizedError(error)) {
          console.error(error);
        }
      }
      setLoginPassword("");
      enterLoggedOutState(null);
    })();
  }, [doCloseRegisterMqtt, enterLoggedOutState, handleUnauthorized]);

  const d = deviceState?.device;

  if (authChecking) {
    return (
      <div className="login-shell">
        <div className="login-card login-card--loading">
          <p className="login-eyebrow">K230 Face Console</p>
          <h1 className="login-title">正在检查登录状态</h1>
          <p className="login-copy">请稍候，正在与 face-web 服务确认当前会话。</p>
        </div>
      </div>
    );
  }

  if (!authenticated) {
    return (
      <div className="login-shell">
        <form className="login-card" onSubmit={onLoginSubmit}>
          <p className="login-eyebrow">K230 Face Console</p>
          <h1 className="login-title">管理员登录</h1>
          <p className="login-copy">输入管理员密码后，才能进入设备管理面板并执行远程控制。</p>
          <label className="login-field">
            <span className="login-field__label">管理员密码</span>
            <input
              className="login-field__input"
              type="password"
              value={loginPassword}
              onChange={(e) => setLoginPassword(e.target.value)}
              placeholder="请输入 FACE_WEB_ADMIN_PASSWORD"
              autoComplete="current-password"
              disabled={authSubmitting}
            />
          </label>
          {authError && <p className="login-error">{authError}</p>}
          <button type="submit" className="login-submit" disabled={authSubmitting}>
            {authSubmitting ? "登录中…" : "进入管理面板"}
          </button>
        </form>
      </div>
    );
  }

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
          <button
            type="button"
            className="app-header__btn"
            onClick={() => {
              void refreshAll().catch((error) => {
                if (!handleUnauthorized(error)) {
                  showError(error);
                }
              });
            }}
          >
            同步数据
          </button>
          <button type="button" className="app-header__btn app-header__btn--danger" onClick={() => void onClearWeb()}>
            清空本地记录
          </button>
          <button type="button" className="app-header__btn" onClick={() => void onLogout()}>
            退出登录
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
                          onClick={() => {
                            if (!selectedDevice) {
                              return;
                            }
                            void syncBoardFaceForDevice(selectedDevice).catch((error) => {
                              if (!handleUnauthorized(error)) {
                                showError(error);
                              }
                            });
                          }}
                        >
                          {faceBoardBusy ? "同步中…" : "同步人脸库（人数 · 快照）"}
                        </button>
                        <button
                          type="button"
                          className="btn--danger"
                          onClick={() => {
                            if (confirm("确定清空当前设备上的人脸库？此操作在板端执行，不可从网页撤销。")) {
                              void postCmd("db-reset").catch(showError);
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
                        className="btn--primary"
                        style={{ width: "100%" }}
                        disabled={!selectedDevice}
                        onClick={() => {
                          void postCmd("import-faces").catch(showError);
                        }}
                      >
                        导入 TF 人脸
                      </button>

                      <button
                        type="button"
                        className="btn--danger"
                        style={{ width: "100%" }}
                        onClick={() => {
                          if (confirm("将向板端发送关闭指令，确定继续？")) {
                            void postCmd("shutdown").catch(showError);
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
            {commandRows.length === 0 ? (
              <p className="empty-state">无记录（下发命令后在此追踪状态）</p>
            ) : (
              <>
                <div className="data-table">
                  <div className="data-table__thead">
                    <span>命令 / 说明</span>
                    <span>状态</span>
                  </div>
                  {commandRows.map((row) => (
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
                <PaginationControls page={commandPage} total={commandTotal} onPageChange={onCommandPageChange} />
              </>
            )}
          </section>

          <section className="panel panel--flush">
            <div className="panel__head">
              <h2 className="panel__title">识别事件</h2>
            </div>
            {eventRows.length === 0 ? (
              <p className="empty-state">无最近事件</p>
            ) : (
              <>
                <div className="data-table data-table--events">
                  <div className="data-table__thead">
                    <span>类型 / 人员</span>
                    <span>分数</span>
                    <span>时间</span>
                  </div>
                  {eventRows.map((ev, idx) => (
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
                      <div className="data-table__meta">{escapeHtml(fmtTs(eventDisplayMs_ms(ev)))}</div>
                    </div>
                  ))}
                </div>
                <PaginationControls page={eventPage} total={eventTotal} onPageChange={onEventPageChange} />
              </>
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
              （例如 TF 上 <code>/mnt/tf/face_logs/2026-05-06.jsonl</code>）。挂载 <code>/dev/mmcblk1p1</code> 后，将{" "}
              <code>face_netd.ini</code> 中 <code>attendance_log_base</code> 指到根目录（如 <code>/mnt/tf/face_logs</code>
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
