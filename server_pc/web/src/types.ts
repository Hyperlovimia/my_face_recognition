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
  /** 板端原始时间戳（未校时时很小）；MQTT 展示用 ts_ms 已由服务端归一化 */
  device_ts_ms?: number;
  /** ISO8601，服务端入库时间；设备 ts_ms 不可信时用于展示 */
  created_at?: string;
};

export type WsMessage =
  | { type: "snapshot"; devices: DeviceRow[] }
  | { type: "status"; device_id: string; payload: unknown; device?: DeviceRow }
  | { type: "reply"; device_id: string; payload: unknown }
  | { type: "event"; device_id: string; payload: EventRow & Record<string, unknown> };
