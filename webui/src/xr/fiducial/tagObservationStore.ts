/**
 * Deterministic bounded storage for tag observations. Pose buckets use 15-cm
 * camera-position cells and 15-degree view-direction bins; exact-pose repeats
 * keep the highest-quality, then oldest stable sample. The capture pipeline
 * must attach pose from the frame that captured the image, never the later XR
 * pose observed when an asynchronous detector result completes.
 */
import {
  TAG_OBSERVATION_DIRECTION_BUCKET_DEGREES,
  TAG_OBSERVATION_POSITION_BUCKET_METERS,
  tagObservationViewBucketKey,
} from './tagObservationFusion';
import type { TagObservation, TagObservationVector3 } from './tagObservationFusion';

export const TAG_OBSERVATION_STORE_DEFAULT_PER_TAG_BUDGET = 48;
export const TAG_OBSERVATION_STORE_DEFAULT_TOTAL_BUDGET = 256;

export interface TagObservationStoreOptions {
  perTagBudget?: number;
  totalBudget?: number;
}

interface PoseDescriptor {
  position: TagObservationVector3;
  forward: TagObservationVector3;
}

interface StoredObservation {
  observation: TagObservation;
  bucketKey: string;
  sequence: number;
  pose: PoseDescriptor | null;
}

function finiteVector3(value: unknown): value is TagObservationVector3 {
  return Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);
}

function normalizeBudget(value: number | undefined, fallback: number): number {
  return value === undefined || !Number.isFinite(value) ? fallback : Math.max(1, Math.floor(value));
}

function cloneObservation(source: TagObservation): TagObservation {
  const result: TagObservation = {
    tagId: source.tagId,
    positionScan: [source.positionScan[0], source.positionScan[1], source.positionScan[2]],
  };
  if (Number.isFinite(source.capturedAtMs)) result.capturedAtMs = source.capturedAtMs;
  if (finiteVector3(source.cameraPosition)) {
    result.cameraPosition = [source.cameraPosition[0], source.cameraPosition[1], source.cameraPosition[2]];
  }
  if (finiteVector3(source.cameraForward)) {
    result.cameraForward = [source.cameraForward[0], source.cameraForward[1], source.cameraForward[2]];
  }
  if (Number.isFinite(source.quality)) result.quality = source.quality;
  return result;
}

function poseFor(observation: TagObservation): PoseDescriptor | null {
  if (!finiteVector3(observation.cameraPosition) || !finiteVector3(observation.cameraForward)) return null;
  const length = Math.hypot(observation.cameraForward[0], observation.cameraForward[1], observation.cameraForward[2]);
  if (!(length > 1e-9)) return null;
  return {
    position: observation.cameraPosition,
    forward: [observation.cameraForward[0] / length, observation.cameraForward[1] / length, observation.cameraForward[2] / length],
  };
}

function retentionPriority(left: StoredObservation, right: StoredObservation): number {
  const leftQuality = Number.isFinite(left.observation.quality) ? left.observation.quality! : Number.NEGATIVE_INFINITY;
  const rightQuality = Number.isFinite(right.observation.quality) ? right.observation.quality! : Number.NEGATIVE_INFINITY;
  if (leftQuality !== rightQuality) return leftQuality > rightQuality ? 1 : -1;

  const leftTime = left.observation.capturedAtMs;
  const rightTime = right.observation.capturedAtMs;
  if (Number.isFinite(leftTime) && Number.isFinite(rightTime) && leftTime !== rightTime) {
    return leftTime! < rightTime! ? 1 : -1;
  }
  if (left.sequence !== right.sequence) return left.sequence < right.sequence ? 1 : -1;
  return 0;
}

function poseDistance(left: StoredObservation, right: StoredObservation): number {
  if (left.pose === null || right.pose === null) return Number.POSITIVE_INFINITY;
  const dx = left.pose.position[0] - right.pose.position[0];
  const dy = left.pose.position[1] - right.pose.position[1];
  const dz = left.pose.position[2] - right.pose.position[2];
  const positionUnits = Math.hypot(dx, dy, dz) / TAG_OBSERVATION_POSITION_BUCKET_METERS;
  const forwardDot = Math.max(-1, Math.min(1,
    left.pose.forward[0] * right.pose.forward[0] +
    left.pose.forward[1] * right.pose.forward[1] +
    left.pose.forward[2] * right.pose.forward[2]));
  const directionUnits = Math.acos(forwardDot) / (TAG_OBSERVATION_DIRECTION_BUCKET_DEGREES * Math.PI / 180);
  return Math.hypot(positionUnits, directionUnits);
}

function sameDistance(left: number, right: number): boolean {
  return left === right || Math.abs(left - right) <= 1e-9;
}

function mostRedundant(entries: readonly StoredObservation[]): { entry: StoredObservation; distance: number } {
  let victim = entries[0];
  let victimDistance = Number.POSITIVE_INFINITY;
  for (let index = 0; index < entries.length; index += 1) {
    const entry = entries[index];
    let nearestDistance = Number.POSITIVE_INFINITY;
    for (let other = 0; other < entries.length; other += 1) {
      if (other === index) continue;
      nearestDistance = Math.min(nearestDistance, poseDistance(entry, entries[other]));
    }
    if (nearestDistance < victimDistance - 1e-9 ||
        (sameDistance(nearestDistance, victimDistance) && retentionPriority(entry, victim) < 0)) {
      victim = entry;
      victimDistance = nearestDistance;
    }
  }
  return { entry: victim, distance: victimDistance };
}

/** Pose-diverse bounded reservoir; rejected samples and outlier reports are counters only. */
export class TagObservationStore {
  readonly perTagBudget: number;
  readonly totalBudget: number;
  private readonly groups = new Map<number, StoredObservation[]>([[0, []], [1, []], [2, []], [3, []], [4, []]]);
  private sequence = 0;
  private rejected = 0;
  private outliers = 0;
  private evicted = 0;

  constructor(options: TagObservationStoreOptions = {}) {
    this.perTagBudget = normalizeBudget(options.perTagBudget, TAG_OBSERVATION_STORE_DEFAULT_PER_TAG_BUDGET);
    this.totalBudget = normalizeBudget(options.totalBudget, TAG_OBSERVATION_STORE_DEFAULT_TOTAL_BUDGET);
  }

  get retainedCount(): number {
    let count = 0;
    for (const entries of this.groups.values()) count += entries.length;
    return count;
  }

  get rejectedCount(): number { return this.rejected; }
  get outlierCount(): number { return this.outliers; }
  get evictedCount(): number { return this.evicted; }

  retainedCountForTag(tagId: number): number {
    return this.groups.get(tagId)?.length ?? 0;
  }

  /** Insert a sample; returns true only when that sample remains in the bounded store. */
  insert(source: TagObservation): boolean {
    if (!Number.isInteger(source.tagId) || source.tagId < 0 || source.tagId > 4 || !finiteVector3(source.positionScan)) {
      this.rejected += 1;
      return false;
    }

    const observation = cloneObservation(source);
    const candidate: StoredObservation = {
      observation,
      bucketKey: tagObservationViewBucketKey(observation),
      sequence: this.sequence++,
      pose: poseFor(observation),
    };
    const entries = this.groups.get(observation.tagId)!;
    const duplicateIndex = entries.findIndex((entry) => entry.bucketKey === candidate.bucketKey);
    if (duplicateIndex >= 0) {
      const existing = entries[duplicateIndex];
      if (retentionPriority(candidate, existing) > 0) entries[duplicateIndex] = candidate;
      this.rejected += 1;
      return entries[duplicateIndex] === candidate;
    }

    entries.push(candidate);
    if (entries.length > this.perTagBudget) {
      const victim = mostRedundant(entries).entry;
      entries.splice(entries.indexOf(victim), 1);
      this.evicted += 1;
      if (victim === candidate) this.rejected += 1;
    }
    if (this.retainedCount > this.totalBudget) {
      const victim = this.mostRedundantAcrossTags();
      const victimEntries = this.groups.get(victim.entry.observation.tagId)!;
      victimEntries.splice(victimEntries.indexOf(victim.entry), 1);
      this.evicted += 1;
      if (victim.entry === candidate) this.rejected += 1;
    }
    return this.groups.get(candidate.observation.tagId)!.includes(candidate);
  }

  /** Record robustly rejected samples without retaining their data. */
  recordOutliers(count = 1): void {
    const acceptedCount = Number.isFinite(count) ? Math.max(0, Math.floor(count)) : 0;
    this.outliers += acceptedCount;
    this.rejected += acceptedCount;
  }

  /** Record other rejected input without retaining the sample. */
  recordRejected(count = 1): void {
    const acceptedCount = Number.isFinite(count) ? Math.max(0, Math.floor(count)) : 0;
    this.rejected += acceptedCount;
  }

  /** Return defensive copies in deterministic insertion order. */
  getObservations(tagId?: number): TagObservation[] {
    const entries: StoredObservation[] = [];
    if (tagId === undefined) {
      for (const group of this.groups.values()) entries.push(...group);
    } else {
      entries.push(...(this.groups.get(tagId) ?? []));
    }
    entries.sort((left, right) => left.sequence - right.sequence);
    return entries.map((entry) => cloneObservation(entry.observation));
  }

  clear(): void {
    for (const entries of this.groups.values()) entries.length = 0;
    this.sequence = 0;
    this.rejected = 0;
    this.outliers = 0;
    this.evicted = 0;
  }

  private mostRedundantAcrossTags(): { entry: StoredObservation; distance: number } {
    let victim: { entry: StoredObservation; distance: number } | null = null;
    for (const entries of this.groups.values()) {
      if (entries.length === 0) continue;
      const candidate = entries.length === 1
        ? { entry: entries[0], distance: Number.POSITIVE_INFINITY }
        : mostRedundant(entries);
      if (victim === null || candidate.distance < victim.distance - 1e-9 ||
          (sameDistance(candidate.distance, victim.distance) &&
           retentionPriority(candidate.entry, victim.entry) < 0)) {
        victim = candidate;
      }
    }
    return victim!;
  }
}
