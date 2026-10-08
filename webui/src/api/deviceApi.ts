import type { LocalizationJobStatus } from '../domain/localizationJob';
import { normalizeDeviceStatus } from './deviceStatusSchema';
import { recordApiBoundaryCopy } from '../xr/scanIdentity';
import {
  normalizeSeismicSnapshot,
  normalizeSavedSeismicSnapshot,
  type DeviceStatus,
  type DeviceWorkflowStatus,
  type PhoneLocationInput,
  type SavedSeismicSnapshot,
  type SeismicSnapshot,
  type NrfLogBadRequestReason,
  type NrfLogPage,
} from '../domain/device';

export const DEVICE_API_FAST_TIMEOUT_MS = 5000;
export const DEVICE_API_EXTENDED_TIMEOUT_MS = 12000;
export const DEVICE_API_CONTROL_TIMEOUT_MS = 12000;
export const DEVICE_API_SCAN_TIMEOUT_MS = 30000;
export const NORMAL_BEFORE_BULK_LIMIT = 4;

let normalBeforeBulkLimit = NORMAL_BEFORE_BULK_LIMIT;
let consecutiveNormalDispatches = 0;

export function setNormalBeforeBulkLimitForTests(limit: number): void {
  if (!Number.isInteger(limit) || limit < 1) throw new RangeError();
  normalBeforeBulkLimit = limit;
}

export type ApiRequestOwner = object;

export interface ApiRequestOptions {
  owner?: ApiRequestOwner;
  read?: boolean;
  preemptible?: boolean;
}

export interface ControlResult {
  ok: boolean;
  action: string;
  object_id?: string;
  bytes?: number;
  id?: string;
  current_id?: string | null;
  error_class?: string;
  error?: string;
  outcome?: 'blocked_recording';
  busy_reason?: 'recording_active' | string;
}

export interface CalcMaterialPreset {
  id: string;
  propagation_speed_mps: number;
  propagation_speed_uncertainty_mps: number;
}

export interface CalcSettingsProfile {
  material: string;
  propagation_speed_mps: number;
  propagation_speed_uncertainty_mps: number;
  [field: string]: string | number | boolean | undefined;
}

export interface CalcProcessingSettings {
  [field: string]: string | number | boolean | undefined;
}

export interface CalcSettingsResponse {
  settings: CalcSettingsProfile | null;
  presets: CalcMaterialPreset[];
  standardProcessing: CalcProcessingSettings;
}

export interface SaveCalcSettingsPayload {
  material: string;
  propagation_speed_mps?: number;
  propagation_speed_uncertainty_mps?: number;
  [field: string]: unknown;
}

export type SaveCalcSettingsResult =
  | { ok: true }
  | { ok: false; error: string; httpStatus: number };

class ApiHttpError extends Error {
  constructor(readonly httpStatus: number, message: string, readonly responseBody?: unknown) {
    super(message);
    this.name = 'ApiHttpError';
  }
}

export class ScanStorageBlockedRecordingError extends Error {
  readonly outcome = 'blocked_recording' as const;
  readonly busy_reason = 'recording_active' as const;

  constructor() {
    super('Recording in progress — scan storage is paused');
    this.name = 'ScanStorageBlockedRecordingError';
  }
}

export class BulkTransferPreemptedError extends Error {
  constructor() {
    super('Bulk transfer paused for a critical control action. Retry when ready.');
    this.name = 'BulkTransferPreemptedError';
  }
}

export type ScanWriteOutcome = 'confirmed_not_committed' | 'unknown';

export class ScanWriteOutcomeError extends Error {
  constructor(
    readonly outcome: ScanWriteOutcome,
    message: string,
    readonly httpStatus?: number,
    readonly currentId: string | null = null,
  ) {
    super(message);
    this.name = 'ScanWriteOutcomeError';
  }
}

export class ApiRequestPreemptedError extends Error {
  readonly code = 'API_REQUEST_PREEMPTED' as const;

  constructor() {
    super();
    this.name = 'ApiRequestPreemptedError';
  }
}

export class ApiRequestOwnerRetiredError extends Error {
  readonly code = 'API_REQUEST_OWNER_RETIRED' as const;

  constructor() {
    super();
    this.name = 'ApiRequestOwnerRetiredError';
  }
}

export function isRequestCancellation(error: unknown): boolean {
  return error instanceof ApiRequestPreemptedError || error instanceof ApiRequestOwnerRetiredError;
}

export async function retryPreemptedRead<T>(operation: () => Promise<T>, maxRetries = 2): Promise<T> {
  let retries = 0;
  for (;;) {
    const pending = operation();
    try {
      return await pending;
    } catch (reason) {
      if (!(reason instanceof ApiRequestPreemptedError) || retries >= maxRetries) throw reason;
      retries += 1;
    }
  }
}

function recordingBlocked(body: unknown, status: number): boolean {
  return status === 409 && typeof body === 'object' && body !== null &&
    'busy_reason' in body && (body as { busy_reason?: unknown }).busy_reason === 'recording_active';
}

function parsedResponseBody(text: string): unknown {
  if (text.length === 0) return null;
  try { return JSON.parse(text) as unknown; }
  catch { return text; }
}

function blockedControlResult(body: unknown): ControlResult {
  const action = typeof body === 'object' && body !== null && 'action' in body
    ? String((body as { action: unknown }).action)
    : 'scan_save';
  return { ok: false, action, outcome: 'blocked_recording', busy_reason: 'recording_active' };
}

const statusRequests = new Map<string, Promise<DeviceStatus>>();
let extendedStatusRequest: Promise<DeviceStatus> | null = null;
let seismicRequest: Promise<SeismicSnapshot> | null = null;
let pendingApiRequests = 0;

type ApiRequestLane = 'critical' | 'normal' | 'bulk';

interface ApiRequestContext {
  controller: AbortController;
  preempted: boolean;
  preemptionKind: 'bulk' | 'read' | null;
  ownerRetired: boolean;
  timedOut: boolean;
}

interface QueuedApiRequest {
  lane: ApiRequestLane;
  operation: (context: ApiRequestContext) => Promise<unknown>;
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  isRead: boolean;
  preemptible: boolean;
  owner?: ApiRequestOwner;
  context?: ApiRequestContext;
}

type QueuedApiRequestOptions = ApiRequestOptions;

const apiRequestQueues: Record<ApiRequestLane, QueuedApiRequest[]> = {
  critical: [],
  normal: [],
  bulk: [],
};
let activeApiRequest: QueuedApiRequest | null = null;
const retiredRequestOwners = new WeakSet<ApiRequestOwner>();

export function createRequestOwner(): ApiRequestOwner {
  return Object.freeze({});
}

export function retireRequestOwner(owner: ApiRequestOwner): void {
  if (retiredRequestOwners.has(owner)) return;
  retiredRequestOwners.add(owner);

  for (const queue of Object.values(apiRequestQueues)) {
    for (let index = queue.length - 1; index >= 0; index -= 1) {
      const request = queue[index];
      if (request !== undefined && request.owner === owner && request.isRead) {
        queue.splice(index, 1);
        pendingApiRequests = Math.max(0, pendingApiRequests - 1);
        request.reject(new ApiRequestOwnerRetiredError());
      }
    }
  }

  const active = activeApiRequest;
  if (active !== null && active.owner === owner && active.preemptible && active.context !== undefined &&
      !active.context.timedOut && !active.context.controller.signal.aborted) {
    active.context.ownerRetired = true;
    active.context.controller.abort();
  }
  startNextApiRequest();
}

/* Keep safety and calibration controls out of the bulk transfer lane. */
const CRITICAL_CONTROL_ACTIONS = new Set([
  'measurement_start',
  'measurement_stop',
  'acquisition_start',
  'acquisition_stop',
  'calibrate_heights',
  'workflow_command',
]);

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function responseError(body: unknown, response: Response): string {
  if (typeof body === 'object' && body !== null && 'error' in body) {
    if ('error_class' in body && (body as { error_class?: unknown }).error_class === 'acquisition_unavailable') {
      return `Acquisition unavailable: ${String((body as { error: unknown }).error)}`;
    }
    return String((body as { error: unknown }).error);
  }
  if (typeof body === 'string' && body.trim().length > 0) return body.trim().slice(0, 240);
  return response.statusText || 'request failed';
}

export function isStorageAvailabilityError(reason: unknown): boolean {
  if (!(reason instanceof ApiHttpError) || reason.httpStatus !== 503) return false;
  const body = reason.responseBody;
  return typeof body === 'object' && body !== null &&
    (('storage_busy' in body && (body as { storage_busy?: unknown }).storage_busy === true) ||
     ('storage_unavailable' in body &&
      (body as { storage_unavailable?: unknown }).storage_unavailable === true));
}

async function performJsonRequest<T>(
  url: string,
  context: ApiRequestContext,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
  allowConflictResponse = false,
): Promise<T> {
  const method = (init.method ?? 'GET').toUpperCase();
  const timeout = globalThis.setTimeout(() => {
    context.timedOut = true;
    context.controller.abort();
  }, timeoutMs);

  try {
    let response: Response;
    let text: string;
    try {
      response = await fetch(url, {
        cache: 'no-store',
        credentials: 'same-origin',
        ...init,
        signal: context.controller.signal,
        headers: {
          Accept: 'application/json',
          ...(init.body === undefined ? {} : { 'Content-Type': 'application/json' }),
          ...(init.headers ?? {}),
        },
      });
      text = await response.text();
    }
    catch (reason) {
      if (context.ownerRetired) throw new ApiRequestOwnerRetiredError();
      if (context.preemptionKind === 'read') throw new ApiRequestPreemptedError();
      if (context.timedOut) {
        throw new Error(`ESP API timeout (${method} ${url}, ${timeoutMs} ms)`);
      }
      throw new Error(`ESP API network error (${method} ${url}): ${errorText(reason)}`);
    }

    let body: unknown = null;
    if (text.length > 0) {
      try {
        body = JSON.parse(text);
      }
      catch {
        body = text;
      }
    }

    if (!response.ok && !(allowConflictResponse && response.status === 409)) {
      if (recordingBlocked(body, response.status)) throw new ScanStorageBlockedRecordingError();
      throw new ApiHttpError(response.status, `ESP API ${response.status} ${method} ${url}: ${responseError(body, response)}`, body);
    }

    return body as T;
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

function nextApiRequest(): QueuedApiRequest | undefined {
  if (apiRequestQueues.normal.length === 0) consecutiveNormalDispatches = 0;
  const critical = apiRequestQueues.critical.shift();
  if (critical !== undefined) return critical;

  if (apiRequestQueues.normal.length === 0) {
    return apiRequestQueues.bulk.shift();
  }
  if (apiRequestQueues.bulk.length > 0 && consecutiveNormalDispatches >= normalBeforeBulkLimit) {
    consecutiveNormalDispatches = 0;
    return apiRequestQueues.bulk.shift();
  }

  consecutiveNormalDispatches += 1;
  return apiRequestQueues.normal.shift();
}

function startNextApiRequest(): void {
  if (activeApiRequest !== null) return;
  const request = nextApiRequest();
  if (request === undefined) return;

  const context: ApiRequestContext = {
    controller: new AbortController(),
    preempted: false,
    preemptionKind: null,
    ownerRetired: false,
    timedOut: false,
  };
  request.context = context;
  activeApiRequest = request;

  const finish = (fulfilled: boolean, value: unknown): void => {
    pendingApiRequests = Math.max(0, pendingApiRequests - 1);
    activeApiRequest = null;
    if (context.ownerRetired ||
        (request.isRead && request.owner !== undefined && retiredRequestOwners.has(request.owner))) {
      request.reject(new ApiRequestOwnerRetiredError());
    } else if (context.preempted && context.preemptionKind === 'read') {
      request.reject(new ApiRequestPreemptedError());
    } else if (context.preempted) {
      request.reject(new BulkTransferPreemptedError());
    } else if (fulfilled) {
      request.resolve(value);
    } else {
      request.reject(value);
    }
    startNextApiRequest();
  };

  try {
    void request.operation(context).then(
      (value) => finish(true, value),
      (reason) => finish(false, reason),
    );
  } catch (reason) {
    finish(false, reason);
  }
}

function queuedApiRequest<T>(
  operation: (context: ApiRequestContext) => Promise<T>,
  lane: ApiRequestLane = 'normal',
  options: QueuedApiRequestOptions = {},
): Promise<T> {
  if (options.owner !== undefined && retiredRequestOwners.has(options.owner)) {
    return Promise.reject(new ApiRequestOwnerRetiredError());
  }

  /*
   * ESP-IDF's HTTPS server executes application handlers serially. Opening a
   * second fetch while the first request is doing an SD write only starts a
   * second TLS/socket timeout; it cannot make the ESP finish sooner. Keep JSON,
   * seismic and binary scan transfers behind the same browser-side gate.
   */
  pendingApiRequests += 1;

  return new Promise<T>((resolve, reject) => {
    const request: QueuedApiRequest = {
      lane,
      operation,
      resolve: (value) => resolve(value as T),
      reject,
      isRead: options.read ?? false,
      preemptible: options.preemptible ?? false,
      owner: options.owner,
    };
    apiRequestQueues[lane].push(request);

    const active = activeApiRequest;
    const context = active?.context;
    if (lane === 'critical' && active?.lane === 'bulk' && context !== undefined &&
        !context.timedOut && !context.controller.signal.aborted) {
      context.preempted = true;
      context.preemptionKind = 'bulk';
      context.controller.abort();
    } else if (lane === 'critical' && active !== null && active.lane !== 'critical' && active.preemptible &&
        context !== undefined && !context.timedOut && !context.controller.signal.aborted) {
      context.preempted = true;
      context.preemptionKind = 'read';
      context.controller.abort();
    }
    startNextApiRequest();
  });
}

function jsonRequest<T>(
  url: string,
  init: RequestInit = {},
  timeoutMs = DEVICE_API_FAST_TIMEOUT_MS,
  lane: ApiRequestLane = 'normal',
  options: ApiRequestOptions = {},
): Promise<T> {
  const method = (init.method ?? 'GET').toUpperCase();
  const read = options.read ?? method === 'GET';
  const preemptible = read && (options.preemptible ?? true);
  return queuedApiRequest((context) => performJsonRequest<T>(url, context, init, timeoutMs), lane, {
    ...options,
    read,
    preemptible,
  });
}

export type ScanStoreKind = 'scan' | 'occupancy' | 'preview' | 'registration';

function scanControlUrl(
  action: 'save' | 'load' | 'delete' | 'info',
  objectId: string,
  sessionId: string,
  kind: ScanStoreKind = 'scan',
  scanId?: string,
): string {
  const query = new URLSearchParams({ scan: action, object_id: objectId, session_id: sessionId });
  if (kind !== 'scan') query.set('kind', kind);
  if (scanId !== undefined && (kind === 'occupancy' || kind === 'preview')) query.set('scan_id', scanId);
  return `/api/control?${query.toString()}`;
}

async function performScanSave(
  objectId: string,
  sessionId: string,
  artifact: Uint8Array,
  context: ApiRequestContext,
  kind: ScanStoreKind = 'scan',
  scanId?: string,
): Promise<ControlResult> {
  const url = scanControlUrl('save', objectId, sessionId, kind, scanId);
  const timeout = globalThis.setTimeout(() => {
    context.timedOut = true;
    context.controller.abort();
  }, DEVICE_API_SCAN_TIMEOUT_MS);
  try {
    let response: Response;
    let text: string;
    try {
      const canUseArtifactBuffer = artifact.byteOffset === 0 &&
        artifact.byteLength === artifact.buffer.byteLength && artifact.buffer instanceof ArrayBuffer;
      const body: ArrayBuffer = canUseArtifactBuffer ? artifact.buffer as ArrayBuffer : artifact.slice().buffer as ArrayBuffer;
      if (!canUseArtifactBuffer) recordApiBoundaryCopy(artifact.byteLength);
      response = await fetch(url, {
        method: 'POST',
        cache: 'no-store',
        credentials: 'same-origin',
        signal: context.controller.signal,
        headers: {
          Accept: 'application/json',
          'Content-Type': kind === 'registration' ? 'application/json; charset=utf-8' : 'application/octet-stream',
        },
        body,
      });
    }
    catch (reason) {
      if (context.timedOut) {
        throw new ScanWriteOutcomeError('unknown', `ESP scan save timeout (${DEVICE_API_SCAN_TIMEOUT_MS} ms)`);
      }
      throw new ScanWriteOutcomeError('unknown', `ESP scan save network error: ${errorText(reason)}`);
    }
    try {
      text = await response.text();
    } catch (reason) {
      if (!response.ok) {
        throw new ScanWriteOutcomeError(
          'confirmed_not_committed',
          `ESP scan save ${response.status}: response body unavailable`,
          response.status,
        );
      }
      throw new ScanWriteOutcomeError('unknown', `ESP scan save response body error: ${errorText(reason)}`);
    }

    const body = parsedResponseBody(text);
    if (!response.ok) {
      if (recordingBlocked(body, response.status)) return blockedControlResult(body);
      const currentId = typeof body === 'object' && body !== null && 'current_id' in body &&
        typeof (body as { current_id?: unknown }).current_id === 'string'
        ? (body as { current_id: string }).current_id
        : null;
      throw new ScanWriteOutcomeError(
        'confirmed_not_committed',
        `ESP scan save ${response.status}: ${responseError(body, response)}`,
        response.status,
        currentId,
      );
    }
    if (typeof body !== 'object' || body === null || !('ok' in body)) {
      throw new ScanWriteOutcomeError('unknown', 'ESP scan save response was unreadable.');
    }
    if ((body as { ok?: unknown }).ok === false) {
      throw new ScanWriteOutcomeError('confirmed_not_committed', `ESP scan save was rejected: ${responseError(body, response)}`);
    }
    if ((body as { ok?: unknown }).ok !== true) {
      throw new ScanWriteOutcomeError('unknown', 'ESP scan save response did not confirm the write.');
    }
    return body as ControlResult;
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

async function performScanLoad(
  objectId: string,
  sessionId: string,
  context: ApiRequestContext,
  kind: ScanStoreKind = 'scan',
): Promise<Uint8Array | null> {
  const url = scanControlUrl('load', objectId, sessionId, kind);
  const timeout = globalThis.setTimeout(() => {
    context.timedOut = true;
    context.controller.abort();
  }, DEVICE_API_SCAN_TIMEOUT_MS);
  try {
    let response: Response;
    try {
      response = await fetch(url, {
        method: 'POST',
        cache: 'no-store',
        credentials: 'same-origin',
        signal: context.controller.signal,
        headers: { Accept: 'application/octet-stream' },
      });
    }
    catch (reason) {
      if (context.timedOut) throw new Error(`ESP scan load timeout (${DEVICE_API_SCAN_TIMEOUT_MS} ms)`);
      throw new Error(`ESP scan load network error: ${errorText(reason)}`);
    }

    if (response.status === 404) return null;
    if (!response.ok) {
      const text = await response.text();
      const body = parsedResponseBody(text);
      if (recordingBlocked(body, response.status)) throw new ScanStorageBlockedRecordingError();
      throw new Error(`ESP scan load ${response.status}: ${responseError(body, response)}`);
    }
    return new Uint8Array(await response.arrayBuffer());
  }
  finally {
    globalThis.clearTimeout(timeout);
  }
}

export function deviceApiHasPendingRequest(): boolean {
  return pendingApiRequests > 0;
}

export function fetchDeviceStatus(
  includeObjects = false,
  sessionsObjectId?: string | null,
  options: ApiRequestOptions = {},
): Promise<DeviceStatus> {
  const coalesce = options.owner === undefined && options.preemptible === undefined;
  /* An extended response is a strict superset of the fast RAM status. */
  if (coalesce && !includeObjects && !sessionsObjectId && extendedStatusRequest !== null) {
    return extendedStatusRequest;
  }

  const query = new URLSearchParams();
  if (includeObjects) query.set('objects', '1');
  if (sessionsObjectId) query.set('sessions', sessionsObjectId);
  const suffix = query.size > 0 ? `?${query.toString()}` : '';
  const url = `/api/status${suffix}`;

  const existing = coalesce ? statusRequests.get(url) : undefined;
  if (existing !== undefined) return existing;

  const extended = includeObjects || Boolean(sessionsObjectId);
  const timeoutMs = extended ? DEVICE_API_EXTENDED_TIMEOUT_MS : DEVICE_API_FAST_TIMEOUT_MS;

  const rawRequest = jsonRequest<unknown>(url, {}, timeoutMs, 'normal', options).then(normalizeDeviceStatus);
  if (!coalesce) return rawRequest;

  let request: Promise<DeviceStatus>;
  request = rawRequest.then(
    (value) => {
      statusRequests.delete(url);
      if (extendedStatusRequest === request) extendedStatusRequest = null;
      return value;
    },
    (reason) => {
      statusRequests.delete(url);
      if (extendedStatusRequest === request) extendedStatusRequest = null;
      throw reason;
    },
  );
  statusRequests.set(url, request);
  if (extended) extendedStatusRequest = request;
  return request;
}

export type FetchNrfLogsResult =
  | { ok: true; value: NrfLogPage }
  | { ok: false; kind: 'bad_request'; reason: NrfLogBadRequestReason }
  | { ok: false; kind: 'network_error'; error: string };

function isU32(value: unknown): value is number {
  return Number.isInteger(value) && (value as number) >= 0 && (value as number) <= 0xffff_ffff;
}

function isNrfLogBadRequestReason(value: unknown): value is NrfLogBadRequestReason {
  return value === 'bad_node' || value === 'bad_limit' || value === 'bad_token';
}

function isNrfLogPage(value: unknown, expectedNode: string): value is NrfLogPage {
  const expectedChannel = expectedNode === 'main' ? null : Number(expectedNode.slice('remote'.length));
  if (!isRecord(value) || value.ok !== true || value.node !== expectedNode || value.channel !== expectedChannel ||
      !(value.node === 'main' || /^remote[0-7]$/.test(String(value.node))) ||
      !(value.channel === null || (isU32(value.channel) && value.channel <= 7)) ||
      !Array.isArray(value.records) ||
      !(value.next_token === null || typeof value.next_token === 'string') ||
      typeof value.reset !== 'boolean' || typeof value.logs_unsupported !== 'boolean' ||
      !(value.boot_id === null || isU32(value.boot_id)) || !isRecord(value.counters)) return false;

  const counters = value.counters;
  if (!isU32(counters.overwritten_total) || !isU32(counters.lost_internal_total) ||
      !isU32(counters.wire_gap_total) || !isU32(counters.local_drop_total)) return false;

  return value.records.every((record: unknown) => isRecord(record) &&
    typeof record.token === 'string' && isU32(record.boot_id) && isU32(record.seq) &&
    isU32(record.uptime_ms) && isU32(record.esp_rx_uptime_ms) &&
    Number.isInteger(record.event_id) && (record.event_id as number) >= 0 && (record.event_id as number) <= 0xffff &&
    Number.isInteger(record.severity) && (record.severity as number) >= 0 && (record.severity as number) <= 4 &&
    Number.isInteger(record.subsystem) && (record.subsystem as number) >= 0 && (record.subsystem as number) <= 0xff &&
    isU32(record.arg0) && isU32(record.arg1));
}

/** The after cursor is opaque; it is only encoded as a query value and returned unchanged by the server. */
export async function fetchNrfLogs(
  node: string,
  afterToken?: string,
  limit = 50,
  options: ApiRequestOptions = {},
): Promise<FetchNrfLogsResult> {
  const query = new URLSearchParams({ node, limit: String(limit) });
  if (afterToken !== undefined) query.set('after', afterToken);

  try {
    const value = await jsonRequest<unknown>(`/api/nrf/logs?${query.toString()}`, {}, DEVICE_API_FAST_TIMEOUT_MS, 'normal', options);
    if (isRecord(value) && value.ok === false && isNrfLogBadRequestReason(value.reason)) {
      return { ok: false, kind: 'bad_request', reason: value.reason };
    }
    return isNrfLogPage(value, node)
      ? { ok: true, value }
      : { ok: false, kind: 'network_error', error: 'invalid_response' };
  } catch (reason) {
    if (reason instanceof ApiRequestPreemptedError || reason instanceof ApiRequestOwnerRetiredError) throw reason;
    if (reason instanceof ApiHttpError && reason.httpStatus === 400 &&
        isRecord(reason.responseBody) && isNrfLogBadRequestReason(reason.responseBody.reason)) {
      return { ok: false, kind: 'bad_request', reason: reason.responseBody.reason };
    }
    return { ok: false, kind: 'network_error', error: errorText(reason) };
  }
}

export function sendControl(
  action: string,
  payload: Record<string, unknown> = {},
  options: ApiRequestOptions = {},
): Promise<ControlResult> {
  return jsonRequest<ControlResult>('/api/control', {
    method: 'POST',
    body: JSON.stringify({ action, ...payload }),
  }, DEVICE_API_CONTROL_TIMEOUT_MS, CRITICAL_CONTROL_ACTIONS.has(action) ? 'critical' : 'normal', options);
}

export function getCalcSettings(objectId: string, options: ApiRequestOptions = {}): Promise<CalcSettingsResponse> {
  const query = new URLSearchParams({ job: 'settings', object_id: objectId });
  return jsonRequest<{
    ok: boolean;
    settings: CalcSettingsProfile | null;
    presets: CalcMaterialPreset[];
    standard_processing: CalcProcessingSettings;
  }>('/api/localization/runs?' + query.toString(), {}, DEVICE_API_EXTENDED_TIMEOUT_MS, 'normal', options).then((response) => ({
    settings: response.settings,
    presets: response.presets,
    standardProcessing: response.standard_processing,
  }));
}

export async function saveCalcSettings(
  objectId: string,
  payload: SaveCalcSettingsPayload,
  options: ApiRequestOptions = {},
): Promise<SaveCalcSettingsResult> {
  try {
    const result = await jsonRequest<ControlResult>('/api/control', {
      method: 'POST',
      body: JSON.stringify({ action: 'save_calc_settings', object_id: objectId, ...payload }),
    }, DEVICE_API_CONTROL_TIMEOUT_MS, 'normal', options);
    return result.ok
      ? { ok: true }
      : { ok: false, error: result.error ?? 'save_calc_settings failed', httpStatus: 200 };
  } catch (reason) {
    return {
      ok: false,
      error: errorText(reason),
      httpStatus: reason instanceof ApiHttpError ? reason.httpStatus : 0,
    };
  }
}

export type WorkflowCommandResult =
  | { ok: true; workflow: DeviceWorkflowStatus }
  | { ok: false; reason: 'stale' | 'not_allowed' }
  | { ok: false; reason: 'error'; message: string };

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

export function sendWorkflowCommand(
  command: string,
  generation: number,
  options: ApiRequestOptions = {},
): Promise<WorkflowCommandResult> {
  const request = queuedApiRequest<WorkflowCommandResult>(async (context) => {
    const body = await performJsonRequest<unknown>('/api/control', context, {
      method: 'POST',
      body: JSON.stringify({ action: 'workflow_command', command, generation }),
    }, DEVICE_API_CONTROL_TIMEOUT_MS, true);

    if (isRecord(body) && body.ok === true && isRecord(body.workflow)) {
      return { ok: true, workflow: body.workflow as unknown as DeviceWorkflowStatus };
    }
    if (isRecord(body) && body.ok === false &&
        (body.reason === 'stale' || body.reason === 'not_allowed')) {
      return { ok: false, reason: body.reason };
    }
    return { ok: false, reason: 'error', message: 'invalid_response' };
  }, 'critical', { ...options, preemptible: false });

  return request.catch((reason: unknown) => ({
    ok: false as const,
    reason: 'error' as const,
    message: errorText(reason),
  }));
}

export function saveObjectScanArtifact(objectId: string, sessionId: string, artifact: Uint8Array): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, artifact, context), 'bulk');
}

/**
 * MAX-162: the compact FREE/OCCUPIED occupancy artifact derived from the
 * session's saved scan, stored beside it (deleted together with the scan).
 */
export function saveSessionOccupancyArtifact(
  objectId: string,
  sessionId: string,
  artifact: Uint8Array,
  scanId?: string,
): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, artifact, context, 'occupancy', scanId), 'bulk');
}

/** MAX-24/MAX-187: bounded surface-voxel preview for the ESP32 TFT. */
export function saveSessionPreviewArtifact(
  objectId: string,
  sessionId: string,
  artifact: Uint8Array,
  scanId?: string,
): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, artifact, context, 'preview', scanId), 'bulk');
}

export function saveSessionRegistration(objectId: string, sessionId: string, json: string): Promise<ControlResult> {
  return queuedApiRequest((context) => performScanSave(objectId, sessionId, new TextEncoder().encode(json), context, 'registration'), 'bulk');
}

export function loadSessionRegistration(objectId: string, sessionId: string): Promise<string | null> {
  return queuedApiRequest(async (context) => {
    const bytes = await performScanLoad(objectId, sessionId, context, 'registration');
    return bytes === null ? null : new TextDecoder().decode(bytes);
  }, 'bulk');
}

export function deleteSessionRegistration(objectId: string, sessionId: string): Promise<ControlResult> {
  return queuedApiRequest((context) => performJsonRequest<ControlResult>(
    scanControlUrl('delete', objectId, sessionId, 'registration'),
    context,
    { method: 'POST' },
    DEVICE_API_EXTENDED_TIMEOUT_MS,
  ), 'bulk');
}

export function loadObjectScanArtifact(objectId: string, sessionId: string): Promise<Uint8Array | null> {
  return queuedApiRequest((context) => performScanLoad(objectId, sessionId, context), 'bulk');
}

export function loadSessionPreviewArtifact(objectId: string, sessionId: string): Promise<Uint8Array | null> {
  return queuedApiRequest((context) => performScanLoad(objectId, sessionId, context, 'preview'), 'bulk');
}

export interface ScanArtifactInfo {
  present: boolean;
  bytes: number;
  id?: string | null;
  sourceId?: string | null;
}

/** MAX-169: presence/size of a session's scan (or occupancy) without downloading it. */
export async function fetchScanInfo(objectId: string, sessionId: string, kind: ScanStoreKind = 'scan'): Promise<ScanArtifactInfo> {
  try {
    const result = await jsonRequest<{ ok: boolean; bytes?: number; id?: unknown; source_id?: unknown; error?: string }>(
      scanControlUrl('info', objectId, sessionId, kind),
      { method: 'POST' },
      DEVICE_API_EXTENDED_TIMEOUT_MS,
      'normal',
      { read: true },
    );
    if (result.ok) return {
      present: true,
      bytes: result.bytes ?? 0,
      id: typeof result.id === 'string' ? result.id : null,
      sourceId: typeof result.source_id === 'string' ? result.source_id : null,
    };
    if (result.error === 'ESP_ERR_NOT_FOUND') return { present: false, bytes: 0, id: null, sourceId: null };
    throw new Error(result.error ?? 'scan info failed');
  }
  catch (reason) {
    const text = reason instanceof Error ? reason.message : String(reason);
    if (text.includes('ESP API 404') || text.includes('ESP_ERR_NOT_FOUND')) return { present: false, bytes: 0, id: null, sourceId: null };
    throw reason;
  }
}

export function deleteObjectScanArtifact(objectId: string, sessionId: string): Promise<ControlResult> {
  return jsonRequest<ControlResult>(scanControlUrl('delete', objectId, sessionId), {
    method: 'POST',
  }, DEVICE_API_SCAN_TIMEOUT_MS, 'bulk');
}

export interface LiveSeismicRequestOptions {
  spanMs?: number;
  maxPoints?: number;
  owner?: ApiRequestOwner;
}

export function fetchSeismic(options: LiveSeismicRequestOptions = {}): Promise<SeismicSnapshot> {
  const coalesce = options.owner === undefined;
  if (coalesce && seismicRequest !== null) return seismicRequest;

  const maxPoints = Math.max(16, Math.min(1200, Math.round(options.maxPoints ?? 220)));
  const spanMs = Math.max(1, Math.min(60000, Math.round(options.spanMs ?? 440)));
  const query = new URLSearchParams({ span_ms: String(spanMs), max_points: String(maxPoints) });
  const rawRequest = jsonRequest<unknown>(
    `/api/seismic?${query.toString()}`,
    {},
    DEVICE_API_FAST_TIMEOUT_MS,
    'normal',
    options,
  ).then((value) => normalizeSeismicSnapshot(value));
  if (!coalesce) return rawRequest;

  let request: Promise<SeismicSnapshot>;
  request = rawRequest.finally(() => {
    if (seismicRequest === request) seismicRequest = null;
  });
  seismicRequest = request;
  return request;
}

export interface SavedSeismicRequestOptions {
  startFrame?: number;
  frameCount?: number;
  points?: number;
  owner?: ApiRequestOwner;
}

export function fetchSavedSeismicSession(
  objectId: string,
  sessionId: string,
  options: SavedSeismicRequestOptions = {},
): Promise<SavedSeismicSnapshot> {
  if (objectId.trim().length === 0 || sessionId.trim().length === 0) {
    return Promise.reject(new Error('Saved seismic object and session are required.'));
  }

  const query = new URLSearchParams({
    object_id: objectId,
    session_id: sessionId,
  });
  if (options.startFrame !== undefined) {
    query.set('start_frame', String(Math.max(0, Math.floor(options.startFrame))));
  }
  if (options.frameCount !== undefined) {
    query.set('frame_count', String(Math.max(0, Math.floor(options.frameCount))));
  }
  if (options.points !== undefined) {
    const bounded = Math.max(16, Math.min(1200, Math.round(options.points)));
    query.set('points', String(bounded));
  }

  return jsonRequest<unknown>(
    `/api/seismic/session?${query.toString()}`,
    {},
    DEVICE_API_EXTENDED_TIMEOUT_MS,
    'bulk',
    options,
  ).then(normalizeSavedSeismicSnapshot);
}

export interface LocalizationRunPosition {
  x_m: number;
  y_m: number;
  z_m: number;
}

/** Posterior region of a Bayesian run (sensor frame, metres). */
export interface LocalizationRunUncertainty {
  /** Per-axis posterior standard deviations. */
  std_m: LocalizationRunPosition;
  /** Probability held by the credible region, e.g. 0.9. */
  credible_level: number;
  /** Region size: volume for a 3D search, area for a planar one (the other is 0). */
  credible_volume_m3: number;
  credible_area_m2: number;
  credible_min: LocalizationRunPosition;
  credible_max: LocalizationRunPosition;
  /** >= 1: every sigma was scaled by this because the fit's chi-square showed them too small. */
  sigma_inflation?: number;
}

export type LocalizationVelocityRangeSource = 'unknown' | 'preset' | 'user' | 'scalar_derived' | 'measured_reserved';
export type LocalizationVelocityStatus = 'unknown' | 'not_applicable' | 'ok' | 'at_bound' | 'inconsistent';

export interface LocalizationVelocityRange {
  v_min_mps: number;
  v_max_mps: number;
  /** 0/0 = no Vp/Vs bound. */
  vp_vs_min: number;
  vp_vs_max: number;
  source: LocalizationVelocityRangeSource;
}

/**
 * MAX-301 decision C: a run whose method found the arrivals inconsistent with
 * the velocity range is never the primary result. It stays listed with a
 * warning. Every place that picks "the" result must use this predicate.
 */
export function runIsPrimaryCandidate(run: LocalizationRun): boolean {
  return run.solved && run.velocity_status !== 'inconsistent';
}

export interface LocalizationRun {
  schema_version: number;
  object_id: string;
  session_id: string;
  computed_at_ms: number;
  kind: 'individual_method' | 'joint_fusion';
  method_name: string;
  method_version: number;
  channel_mask_used: number;
  channel_mask_rejected: number;
  dimensionality: 'insufficient' | 'planar_2d' | 'capable_3d';
  geometry_computed_at_ms: number;
  propagation_model_name: string;
  material?: string;
  propagation_speed_mps: number;
  propagation_speed_uncertainty_mps: number;
  confidence_score: number;
  /** MAX-212: redundancy of a pick-based fit; 0 = exactly determined (unverified), -1/absent = n/a. */
  degrees_of_freedom?: number;
  /** Methods that estimate the event's own velocities (TDCAS, PSAFUVS, SUEA, PSP; eikonal and Bayesian when the speed is uncertain): P (or single-phase) and S; absent = not estimated. */
  estimated_p_velocity_mps?: number;
  estimated_s_velocity_mps?: number;
  /** MAX-301 (schema v5): the velocity range the run used; absent in v3/v4 records. */
  velocity_range?: LocalizationVelocityRange;
  /** MAX-301 (schema v5): TDCAS/PSAFUVS/SUEA verdict on that range; absent = unknown. */
  velocity_status?: LocalizationVelocityStatus;
  /** MAX-414 (schema v6): whether this run actually applied the current scan constraint. */
  scan_constraint_applied?: boolean | null;
  /** MAX-414 (schema v6): CRC identity of the scan used by a constrained run. */
  scan_frame_id?: string | null;
  /** MAX-414 (schema v6): occupancy generation consumed by the run. */
  occupancy_created_at_ms?: number | null;
  /** MAX-414 (schema v6): registration geometry generation consumed by the run. */
  registration_geometry_computed_at_ms?: number | null;
  /** Bayesian methods: the posterior's spread and credible region; absent = not computed. */
  uncertainty?: LocalizationRunUncertainty;
  solved: boolean;
  uncertainty_too_high?: boolean;
  source_position?: LocalizationRunPosition;
  emission_time_us?: number;
  rms_residual_us?: number;
  iterations?: number;
  converged?: boolean;
  rejection_reason?: string;
}

/** Decodes the optional MAX-414 fields while leaving legacy v3-v5 records valid. */
export function decodeLocalizationRun(value: unknown): LocalizationRun {
  if (!isRecord(value) || Array.isArray(value)) throw new Error('Invalid localization run record.');
  const decoded = { ...value };
  const nullableBoolean = (key: string): void => {
    const field = value[key];
    if (!Object.prototype.hasOwnProperty.call(value, key) || field === undefined) return;
    if (field !== null && typeof field !== 'boolean') throw new Error(`Invalid localization run field: ${key}.`);
  };
  const nullableString = (key: string): void => {
    const field = value[key];
    if (!Object.prototype.hasOwnProperty.call(value, key) || field === undefined) return;
    if (field !== null && typeof field !== 'string') throw new Error(`Invalid localization run field: ${key}.`);
  };
  const nullableTimestamp = (key: string): void => {
    const field = value[key];
    if (!Object.prototype.hasOwnProperty.call(value, key) || field === undefined) return;
    if (field !== null && (typeof field !== 'number' || !Number.isFinite(field) || field < 0)) {
      throw new Error(`Invalid localization run field: ${key}.`);
    }
  };
  nullableBoolean('scan_constraint_applied');
  nullableString('scan_frame_id');
  nullableTimestamp('occupancy_created_at_ms');
  nullableTimestamp('registration_geometry_computed_at_ms');
  return decoded as unknown as LocalizationRun;
}

function decodeLocalizationRunListing(value: unknown): LocalizationRunListing {
  // Only the runs are decoded here; the listing envelope keeps its previous (unvalidated) contract.
  if (isRecord(value) && Array.isArray(value.runs)) {
    return { ...value, runs: value.runs.map(decodeLocalizationRun) } as unknown as LocalizationRunListing;
  }
  return value as unknown as LocalizationRunListing;
}

export interface LocalizationSessionPodGeometry {
  pod: number;
  computed: boolean;
  leveled: boolean;
  assigned?: boolean;
  node_id?: number | null;
  hardware_id?: string | null;
  position_source?: 'none' | 'channel_sounding' | 'channel_sounding_baro';
  x_mm?: number;
  y_mm?: number;
  z_mm?: number;
  /** Largest |solved - measured| CS distance for this pod (MAX-205). */
  residual_mm?: number;
  /** MAX-144: 1-sigma position uncertainty (mm); 0 = n/a, 2147483647 = undetermined. */
  uncertainty_mm?: number;
  computed_at_ms?: number;
  source_sample_uptime_ms?: number;
  esp_receipt_uptime_ms?: number;
  source_age_ms_at_start?: number | null;
  height_reference_generation?: number;
}

export interface LocalizationGeometryProvenance {
  solver?: string;
  solver_version?: string;
  refined?: boolean;
  refine_iterations?: number;
  topology_generation?: number;
  cs_membership_generation?: number;
  main_height_reference_generation?: number;
  setup_id?: string;
  start_uptime_ms?: number;
}

export interface LocalizationSessionGeometry {
  available: boolean;
  z_policy_effective?: 'planar' | 'barometric_relative';
  z_fallback_reason?: string;
  provenance?: LocalizationGeometryProvenance;
  pods: LocalizationSessionPodGeometry[];
}

export interface LocalizationCsPair {
  node_a: number;
  node_b: number;
  valid: boolean;
  distance_mm: number;
  confidence: number;
  attempts: number;
  valid_samples: number;
  /** MAX-206: robust spread of the accepted samples (mm) and outliers dropped. */
  spread_mm?: number;
  rejected_samples?: number;
  state?: string;
  last_failure?: string;
  provider_quality_flags?: number;
  scheduler_quality_flags?: number;
}

export interface LocalizationSessionCsPairs {
  available: boolean;
  pairs: LocalizationCsPair[];
}

export interface LocalizationPickChannel {
  channel: number;
  participating: boolean;
  status: 'accepted' | 'rejected_no_trigger' | 'rejected_no_refinement' | 'rejected_low_snr' | 'rejected_inconsistent';
  method: string;
  arrival_sample_index: number;
  arrival_time_s: number;
  snr: number;
  uncertainty_samples: number;
}

export interface LocalizationPickRecord {
  schema_version: number;
  object_id: string;
  session_id: string;
  computed_at_ms: number;
  channels: LocalizationPickChannel[];
}

export interface LocalizationRunListing {
  object_id: string;
  session_id: string;
  geometry: LocalizationSessionGeometry;
  cs_pairs: LocalizationSessionCsPairs;
  picks: LocalizationPickRecord[];
  runs: LocalizationRun[];
}

export function fetchLocalizationRuns(
  objectId: string,
  sessionId: string,
  options: ApiRequestOptions = {},
): Promise<LocalizationRunListing> {
  const query = new URLSearchParams({ object_id: objectId, session_id: sessionId });
  return jsonRequest<unknown>(
    `/api/localization/runs?${query.toString()}`,
    {},
    DEVICE_API_EXTENDED_TIMEOUT_MS,
    'normal',
    options,
  ).then(decodeLocalizationRunListing);
}

/** The background "Calculate source" job: progress, measured timings and stack. */
export function fetchLocalizationJob(options: ApiRequestOptions = {}): Promise<LocalizationJobStatus> {
  return jsonRequest<LocalizationJobStatus>('/api/localization/runs?job=status', {}, DEVICE_API_CONTROL_TIMEOUT_MS, 'normal', options);
}

export function capturePhoneLocation(): Promise<PhoneLocationInput> {
  if (!('geolocation' in navigator)) {
    return Promise.reject(new Error('Geolocation is not supported by this browser.'));
  }

  return new Promise((resolve, reject) => {
    navigator.geolocation.getCurrentPosition(
      (position) => resolve({
        latitude: position.coords.latitude,
        longitude: position.coords.longitude,
        accuracy_m: position.coords.accuracy,
        altitude_m: position.coords.altitude,
        altitude_accuracy_m: position.coords.altitudeAccuracy,
        captured_at_ms: position.timestamp,
      }),
      (error) => reject(new Error(`Phone GPS unavailable: ${error.message}`)),
      {
        enableHighAccuracy: true,
        maximumAge: 0,
        timeout: 15000,
      },
    );
  });
}
