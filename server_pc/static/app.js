const state = {
  devices: [],
  selectedDevice: null,
  deviceState: null,
  events: [],
  ws: null,
};

const els = {
  deviceSelect: document.getElementById("deviceSelect"),
  refreshAllBtn: document.getElementById("refreshAllBtn"),
  clearWebDataBtn: document.getElementById("clearWebDataBtn"),
  deviceStats: document.getElementById("deviceStats"),
  commandList: document.getElementById("commandList"),
  eventList: document.getElementById("eventList"),
  wsStatus: document.getElementById("wsStatus"),
  dbCountBtn: document.getElementById("dbCountBtn"),
  dbResetBtn: document.getElementById("dbResetBtn"),
  registerBtn: document.getElementById("registerBtn"),
  shutdownBtn: document.getElementById("shutdownBtn"),
  registerName: document.getElementById("registerName"),
};

function fmtTs(ms) {
  if (!ms) return "-";
  return new Date(ms).toLocaleString();
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

async function fetchJson(url, options) {
  const resp = await fetch(url, options);
  const data = await resp.json();
  if (!resp.ok) {
    throw new Error(data.detail || `HTTP ${resp.status}`);
  }
  return data;
}

function renderDeviceOptions() {
  const previous = state.selectedDevice;
  els.deviceSelect.innerHTML = state.devices
    .map((device) => `<option value="${device.device_id}">${device.device_id}</option>`)
    .join("");
  if (state.devices.length === 0) {
    state.selectedDevice = null;
    els.deviceSelect.innerHTML = '<option value="">暂无设备</option>';
    return;
  }
  const available = state.devices.map((device) => device.device_id);
  state.selectedDevice = available.includes(previous) ? previous : available[0];
  els.deviceSelect.value = state.selectedDevice;
}

function renderStats() {
  if (!state.deviceState?.device) {
    els.deviceStats.innerHTML = '<p class="empty">暂无设备状态</p>';
    return;
  }
  const device = state.deviceState.device;
  els.deviceStats.innerHTML = `
    <div class="stat">
      <span>Online</span>
      <strong class="${device.online ? "ok" : "bad"}">${device.online ? "YES" : "NO"}</strong>
    </div>
    <div class="stat">
      <span>RT Bridge</span>
      <strong class="${device.rt_connected ? "ok" : "bad"}">${device.rt_connected ? "CONNECTED" : "DISCONNECTED"}</strong>
    </div>
    <div class="stat">
      <span>DB Count</span>
      <strong>${device.db_count}</strong>
    </div>
    <div class="stat wide">
      <span>Last Seen</span>
      <strong>${fmtTs(device.last_seen_ms)}</strong>
    </div>
  `;
}

function renderCommands() {
  const rows = state.deviceState?.recent_commands || [];
  if (rows.length === 0) {
    els.commandList.innerHTML = '<p class="empty">暂无命令记录</p>';
    return;
  }
  els.commandList.innerHTML = rows
    .map((row) => `
      <div class="row">
        <div>
          <strong>${escapeHtml(row.cmd)}</strong>
          <p>${escapeHtml(row.message || row.status)}</p>
        </div>
        <div class="meta">
          <span class="badge ${row.ok === 1 ? "ok" : row.ok === 0 ? "bad" : ""}">${escapeHtml(row.status)}</span>
          <small>${escapeHtml(row.request_id)}</small>
        </div>
      </div>
    `)
    .join("");
}

function renderEvents() {
  if (state.events.length === 0) {
    els.eventList.innerHTML = '<p class="empty">暂无事件</p>';
    return;
  }
  els.eventList.innerHTML = state.events
    .map((event) => `
      <div class="row">
        <div>
          <strong>${escapeHtml(event.evt_kind)}</strong>
          <p>${escapeHtml(event.name || "unknown")} / face_id=${escapeHtml(event.face_id)}</p>
        </div>
        <div class="meta">
          <span>${escapeHtml(Number(event.score).toFixed(3))}</span>
          <small>${escapeHtml(fmtTs(event.ts_ms))}</small>
        </div>
      </div>
    `)
    .join("");
}

async function loadDevices() {
  const data = await fetchJson("/api/devices");
  state.devices = data.devices;
  renderDeviceOptions();
}

async function loadSelectedDevice() {
  if (!state.selectedDevice) {
    state.deviceState = null;
    state.events = [];
    renderStats();
    renderCommands();
    renderEvents();
    return;
  }
  state.deviceState = await fetchJson(`/api/devices/${encodeURIComponent(state.selectedDevice)}/state`);
  const eventData = await fetchJson(`/api/devices/${encodeURIComponent(state.selectedDevice)}/events?limit=100`);
  state.events = eventData.events;
  renderStats();
  renderCommands();
  renderEvents();
}

async function refreshAll() {
  await loadDevices();
  await loadSelectedDevice();
}

async function postCommand(path, body) {
  if (!state.selectedDevice) {
    alert("暂无设备");
    return;
  }
  const url = `/api/devices/${encodeURIComponent(state.selectedDevice)}/commands/${path}`;
  const options = {
    method: "POST",
    headers: { "Content-Type": "application/json" },
  };
  if (body) {
    options.body = JSON.stringify(body);
  }
  const result = await fetchJson(url, options);
  await loadSelectedDevice();
  renderCommands();
  return result;
}

async function clearWebData() {
  const result = await fetchJson("/api/web-data/clear", { method: "POST" });
  await refreshAll();
  return result;
}

function connectWs() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  state.ws = new WebSocket(`${protocol}://${location.host}/ws`);

  state.ws.onopen = () => {
    els.wsStatus.textContent = "WS connected";
    els.wsStatus.className = "badge ok";
  };

  state.ws.onclose = () => {
    els.wsStatus.textContent = "WS disconnected";
    els.wsStatus.className = "badge offline";
    setTimeout(connectWs, 1000);
  };

  state.ws.onmessage = async (event) => {
    const msg = JSON.parse(event.data);
    if (msg.type === "snapshot") {
      state.devices = msg.devices;
      renderDeviceOptions();
      await loadSelectedDevice();
      return;
    }
    if (msg.device_id !== state.selectedDevice) {
      await loadDevices();
      return;
    }
    if (msg.type === "event") {
      state.events.unshift(msg.payload);
      state.events = state.events.slice(0, 100);
      renderEvents();
    } else if (msg.type === "status" || msg.type === "reply") {
      await loadSelectedDevice();
    }
  };
}

els.deviceSelect.addEventListener("change", async (event) => {
  state.selectedDevice = event.target.value;
  await loadSelectedDevice();
});

els.refreshAllBtn.addEventListener("click", refreshAll);
els.clearWebDataBtn.addEventListener("click", async () => {
  if (!confirm("确定要清空当前网页保存的本地设备、命令和事件记录吗？这不会清空开发板的人脸库。")) {
    return;
  }
  try {
    const result = await clearWebData();
    const cleared = result.cleared || {};
    alert(`已清空网页本地数据：设备 ${cleared.devices || 0} 条，命令 ${cleared.commands || 0} 条，事件 ${cleared.events || 0} 条`);
  } catch (err) {
    console.error(err);
    alert(err.message);
  }
});
els.dbCountBtn.addEventListener("click", () => postCommand("db-count"));
els.dbResetBtn.addEventListener("click", () => {
  if (confirm("确定要清空当前设备的人脸数据库吗？")) {
    postCommand("db-reset");
  }
});
els.registerBtn.addEventListener("click", () => {
  const name = els.registerName.value.trim();
  if (!name) {
    alert("请输入注册姓名");
    return;
  }
  postCommand("register-current", { name });
});
els.shutdownBtn.addEventListener("click", () => {
  if (confirm("确定要关闭开发板三进程吗？")) {
    postCommand("shutdown");
  }
});

refreshAll()
  .catch((err) => {
    console.error(err);
    alert(err.message);
  })
  .finally(() => {
    connectWs();
  });
