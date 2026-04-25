export type DeviceRow = {
  device_id: string;
  online: number;
  rt_connected: number;
  db_count: number;
  last_seen_ms: number;
  updated_at: string;
};

export type CommandRow = {
  request_id: string;
  cmd: string;
  status: string;
  ok: number | null;
  count: number | null;
  message: string | null;
};

export type EventRow = {
  evt_kind: string;
  face_id: number;
  name: string;
  score: number;
  ts_ms: number;
};

export type WsMessage =
  | { type: "snapshot"; devices: DeviceRow[] }
  | { type: "status"; device_id: string; payload: unknown; device?: DeviceRow }
  | { type: "reply"; device_id: string; payload: unknown }
  | { type: "event"; device_id: string; payload: EventRow & Record<string, unknown> };
