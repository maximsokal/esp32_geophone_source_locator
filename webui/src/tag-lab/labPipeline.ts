import type { DetectedAprilTag } from '../xr/fiducial/tagDetector';
import type { DetectedAprilTagFrame, TagCameraFrame } from '../xr/fiducial/tagDetectorClient';
import { TAG_BINDINGS, tagNodeForId, type TagNode } from '../xr/fiducial/tagBindings.gen';
import { RateCounter, RollingSamples } from './labStats';
import type { LabSourceType, TagLabDiagnostics } from './TagLabDiagnostics';

export interface LabFrame {
  frameId: number;
  sourceType: LabSourceType;
  capturedAtMs: number;
  width: number;
  height: number;
  rgba: Uint8Array;
  sessionEpoch: number;
}

export interface LabDetector {
  readonly busy: boolean;
  detect(frame: TagCameraFrame): Promise<DetectedAprilTagFrame | null>;
}

export type TagLabStatus = 'NOT SEEN' | 'DETECTED 2D' | 'LOST';

export interface LabTagState {
  tagId: number;
  node: TagNode;
  status: TagLabStatus;
  lastSeenMs: number | null;
  corners: DetectedAprilTag['corners'] | null;
  frameWidth: number;
  frameHeight: number;
  hammingDistance: number | null;
  detectionCount: number;
}

export interface LabCounters { decoded: number; framesSubmitted: number; framesProcessed: number; framesDropped: number; workerErrors: number; staleResults: number; }
export type SubmitOutcome = 'processed' | 'dropped-busy' | 'dropped-stale-epoch' | 'error';

const IDENTITY = Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

export interface LabPipelineOptions {
  detector: LabDetector;
  diagnostics: TagLabDiagnostics;
  now?: () => number;
  /** A tag not decoded for this long is shown as LOST. */
  staleMs?: number;
}

/**
 * Source-independent live detection core of the Tag Lab: one pending decode at a time (no backlog), stale
 * results from an older session epoch are discarded, and a tag that stops decoding becomes LOST instead of
 * lingering. The camera matrices passed to the detector client are LAB PLACEHOLDERS (identity): the lab shows
 * 2D detections only and never derives a metric pose from them.
 */
export class LabPipeline {
  private readonly detector: LabDetector;
  private readonly diag: TagLabDiagnostics;
  private readonly now: () => number;
  private readonly staleMs: number;
  private epoch = 1;
  private readonly tags = new Map<number, LabTagState>();
  private lastIdSignature = '';
  private lastBusyEventMs = Number.NEGATIVE_INFINITY;
  private lastAggregateMs = Number.NEGATIVE_INFINITY;
  private sourceType: LabSourceType = 'lab';
  private firstDecodeLogged = false;
  private closed = false;
  readonly counters: LabCounters = { decoded: 0, framesSubmitted: 0, framesProcessed: 0, framesDropped: 0, workerErrors: 0, staleResults: 0 };
  readonly latencyMs = new RollingSamples();
  readonly processingMs = new RollingSamples();
  readonly captureRate = new RateCounter();
  readonly detectorRate = new RateCounter();
  lastError: string | null = null;
  lastDetectedTagId: number | null = null;

  constructor(options: LabPipelineOptions) {
    this.detector = options.detector;
    this.diag = options.diagnostics;
    this.now = options.now ?? (() => performance.now());
    this.staleMs = options.staleMs ?? 1000;
    this.resetTags();
  }

  get sessionEpoch(): number { return this.epoch; }
  get busy(): boolean { return this.detector.busy; }

  /** Count a camera frame that was not even grabbed because a decode is still pending (no backlog). */
  recordBusySkip(sourceType: LabSourceType, frameId: number | null = null): void {
    const nowMs = this.now();
    this.sourceType = sourceType;
    this.counters.framesSubmitted += 1; this.counters.framesDropped += 1;
    this.captureRate.mark(nowMs);
    this.noteBusy(nowMs, frameId);
  }

  private noteBusy(nowMs: number, frameId: number | null): void {
    if (nowMs - this.lastBusyEventMs < 1000) return;
    this.lastBusyEventMs = nowMs;
    this.diag.emit('WORKER_BUSY', this.sourceType, { droppedTotal: this.counters.framesDropped }, { sessionEpoch: this.epoch, ...(frameId === null ? {} : { frameId }) });
  }

  /**
   * Capture stopped: in-flight decodes belong to the old epoch (a disposed detector rejecting them is not a fault) and
   * the last tag states are kept for inspection instead of decaying to LOST.
   */
  close(): void {
    this.epoch += 1;
    this.closed = true;
  }

  /** A new capture session: old results must not come back. */
  newSession(): number {
    this.epoch += 1;
    this.closed = false;
    this.resetTags();
    this.latencyMs.clear(); this.processingMs.clear(); this.captureRate.clear(); this.detectorRate.clear();
    Object.assign(this.counters, { decoded: 0, framesSubmitted: 0, framesProcessed: 0, framesDropped: 0, workerErrors: 0, staleResults: 0 });
    this.lastError = null; this.lastDetectedTagId = null; this.lastIdSignature = ''; this.firstDecodeLogged = false;
    return this.epoch;
  }

  tagStates(): LabTagState[] { return [...this.tags.values()].sort((a, b) => a.tagId - b.tagId); }

  async submit(frame: LabFrame): Promise<SubmitOutcome> {
    this.sourceType = frame.sourceType;
    const started = this.now();
    this.counters.framesSubmitted += 1;
    this.captureRate.mark(started);
    if (frame.sessionEpoch !== this.epoch) { this.counters.staleResults += 1; return 'dropped-stale-epoch'; }
    if (this.detector.busy) {
      this.counters.framesDropped += 1;
      this.noteBusy(started, frame.frameId);
      return 'dropped-busy';
    }
    if (!this.firstDecodeLogged) {
      this.firstDecodeLogged = true;
      this.diag.emit('DECODE_STARTED', frame.sourceType, { width: frame.width, height: frame.height }, { sessionEpoch: frame.sessionEpoch, frameId: frame.frameId });
    }
    try {
      const result = await this.detector.detect({
        width: frame.width, height: frame.height, rgba: frame.rgba, capturedAtMs: frame.capturedAtMs,
        cameraPosition: [0, 0, 0], cameraForward: [0, 0, -1], projectionMatrix: IDENTITY, worldFromView: IDENTITY,
        rgbaOwnership: 'owned',
      });
      if (result === null) { this.counters.framesDropped += 1; return 'dropped-busy'; }
      const finished = this.now();
      if (frame.sessionEpoch !== this.epoch) { this.counters.staleResults += 1; return 'dropped-stale-epoch'; }
      this.counters.framesProcessed += 1;
      this.latencyMs.push(finished - frame.capturedAtMs);
      this.processingMs.push(finished - started);
      this.detectorRate.mark(finished);
      this.lastError = null;
      this.applyDetections(result.detections, frame, finished);
      return 'processed';
    } catch (error) {
      /* A reset/dispose of an older session rejects its pending decode; that is not a fault of the current session. */
      if (frame.sessionEpoch !== this.epoch) { this.counters.staleResults += 1; return 'dropped-stale-epoch'; }
      this.counters.workerErrors += 1;
      this.lastError = error instanceof Error ? error.message : String(error);
      this.diag.emit('WORKER_ERROR', frame.sourceType, { message: this.lastError }, { sessionEpoch: frame.sessionEpoch, frameId: frame.frameId });
      return 'error';
    }
  }

  /** Mark tags that have not decoded recently as LOST. Call regularly (e.g. per animation frame). */
  tick(nowMs: number = this.now()): void {
    if (this.closed) return;
    for (const state of this.tags.values()) {
      if (state.status === 'DETECTED 2D' && state.lastSeenMs !== null && nowMs - state.lastSeenMs > this.staleMs) {
        state.status = 'LOST';
        this.diag.emit('TAG_LOST', this.sourceType, { tagId: state.tagId, node: state.node, silentMs: Math.round(nowMs - state.lastSeenMs) }, { sessionEpoch: this.epoch });
      }
    }
  }

  /** Aggregated per-second metrics (never one event per camera frame) and a performance snapshot. */
  aggregate(nowMs: number = this.now()): Record<string, unknown> {
    const snapshot = {
      captureFps: round(this.captureRate.perSecond(nowMs)), detectorFps: round(this.detectorRate.perSecond(nowMs)),
      latencyP50Ms: round(this.latencyMs.percentile(50)), latencyP95Ms: round(this.latencyMs.percentile(95)),
      processingP50Ms: round(this.processingMs.percentile(50)), processingP95Ms: round(this.processingMs.percentile(95)),
      ...this.counters,
    };
    if (nowMs - this.lastAggregateMs >= 1000) {
      this.lastAggregateMs = nowMs;
      this.diag.emit('CAMERA_FRAME', this.sourceType, snapshot, { sessionEpoch: this.epoch });
    }
    return snapshot;
  }

  private applyDetections(detections: DetectedAprilTag[], frame: LabFrame, nowMs: number): void {
    const signature = detections.map((d) => d.tagId).sort().join(',');
    for (const detection of detections) {
      const state = this.tags.get(detection.tagId);
      if (state === undefined) continue;
      const first = state.status !== 'DETECTED 2D';
      state.status = 'DETECTED 2D'; state.lastSeenMs = nowMs; state.corners = detection.corners;
      state.frameWidth = frame.width; state.frameHeight = frame.height;
      state.hammingDistance = detection.hammingDistance; state.detectionCount += 1;
      this.counters.decoded += 1; this.lastDetectedTagId = detection.tagId;
      if (first) {
        this.diag.emit('TAG_DETECTED', frame.sourceType, {
          tagId: detection.tagId, node: state.node, hamming: detection.hammingDistance, width: frame.width, height: frame.height,
          corners: detection.corners.map(([x, y]) => [round(x, 4), round(y, 4)]),
        }, { sessionEpoch: frame.sessionEpoch, frameId: frame.frameId });
      }
    }
    if (signature !== this.lastIdSignature) {
      this.lastIdSignature = signature;
      this.diag.emit('DECODE_FINISHED', frame.sourceType, { tagIds: detections.map((d) => d.tagId), count: detections.length }, { sessionEpoch: frame.sessionEpoch, frameId: frame.frameId });
    }
  }

  private resetTags(): void {
    this.tags.clear();
    for (const binding of TAG_BINDINGS) {
      this.tags.set(binding.tag_id, {
        tagId: binding.tag_id, node: tagNodeForId(binding.tag_id), status: 'NOT SEEN', lastSeenMs: null, corners: null,
        frameWidth: 0, frameHeight: 0, hammingDistance: null, detectionCount: 0,
      });
    }
  }
}

function round(value: number | null, digits = 1): number | null {
  if (value === null) return null;
  const k = 10 ** digits;
  return Math.round(value * k) / k;
}
