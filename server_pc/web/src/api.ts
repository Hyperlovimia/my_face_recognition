import type { CommandRow, DeviceRow, EventRow } from "./types";

export async function fetchJson<T>(url: string, options?: RequestInit): Promise<T> {
  const resp = await fetch(url, options);
  const data = (await resp.json()) as T & { detail?: string };
  if (!resp.ok) {
    const msg = (data as { detail?: string }).detail ?? `HTTP ${resp.status}`;
    throw new Error(msg);
  }
  return data as T;
}

export function getDevices() {
  return fetchJson<{ devices: DeviceRow[] }>("/api/devices");
}

export function getDeviceState(deviceId: string) {
  return fetchJson<{
    device: DeviceRow;
    recent_commands: CommandRow[];
  }>(`/api/devices/${encodeURIComponent(deviceId)}/state`);
}

export function getDeviceEvents(deviceId: string, limit = 100) {
  return fetchJson<{ events: EventRow[] }>(`/api/devices/${encodeURIComponent(deviceId)}/events?limit=${limit}`);
}

export function postWebDataClear() {
  return fetchJson<{ status: string; cleared: Record<string, number> }>("/api/web-data/clear", { method: "POST" });
}

export function postCommand(deviceId: string, path: string, body: Record<string, string> | null) {
  const url = `/api/devices/${encodeURIComponent(deviceId)}/commands/${path}`;
  const init: RequestInit = { method: "POST" };
  if (body) {
    init.headers = { "Content-Type": "application/json" };
    init.body = JSON.stringify(body);
  }
  return fetchJson<{ request_id: string; status: string }>(url, init);
}
