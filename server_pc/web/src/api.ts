import type { CommandRow, DeviceRow, EventRow, FaceGalleryEntry } from "./types";

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

export function faceGalleryPhotoUrl(deviceId: string, slot: number, timeoutSec = 25) {
  const qs = `timeout_sec=${encodeURIComponent(String(timeoutSec))}`;
  return `/api/devices/${encodeURIComponent(deviceId)}/face-gallery/${slot}/photo.jpg?${qs}`;
}

export function getFaceGallery(deviceId: string, timeoutSec = 25) {
  return fetchJson<{ entries: FaceGalleryEntry[] }>(
    `/api/devices/${encodeURIComponent(deviceId)}/face-gallery?timeout_sec=${encodeURIComponent(String(timeoutSec))}`,
  );
}

export type SdAttendanceLogResponse = {
  path?: string;
  date?: string;
  truncated?: boolean;
  file_size?: number;
  bytes_returned?: number;
  content: string;
};

export function getSdAttendanceLog(
  deviceId: string,
  opts?: { date?: string; maxBytes?: number; tailLines?: number; timeoutSec?: number },
) {
  const p = new URLSearchParams();
  if (opts?.date != null && opts.date !== "") {
    p.set("date", opts.date);
  }
  if (opts?.maxBytes != null) {
    p.set("max_bytes", String(opts.maxBytes));
  }
  if (opts?.tailLines != null) {
    p.set("tail_lines", String(opts.tailLines));
  }
  if (opts?.timeoutSec != null) {
    p.set("timeout_sec", String(opts.timeoutSec));
  }
  const qs = p.toString();
  const base = `/api/devices/${encodeURIComponent(deviceId)}/sd-attendance-log`;
  return fetchJson<SdAttendanceLogResponse>(base + (qs ? `?${qs}` : ""));
}
