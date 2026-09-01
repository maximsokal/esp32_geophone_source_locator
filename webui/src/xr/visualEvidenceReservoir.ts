import type {
  NormalizedPoint2,
  VisualEvidenceFrame,
} from './visualLineEvidence';

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function cloneFrame(frame: VisualEvidenceFrame): VisualEvidenceFrame {
  return {
    ...frame,
    projectionMatrix: frame.projectionMatrix.slice(),
    worldFromView: frame.worldFromView.slice(),
    lines: frame.lines.map((line) => ({
      ...line,
      start: [...line.start] as NormalizedPoint2,
      end: [...line.end] as NormalizedPoint2,
      direction: [...line.direction] as NormalizedPoint2,
    })),
    junctions: frame.junctions.map((junction) => ({
      ...junction,
      position: [...junction.position] as NormalizedPoint2,
    })),
  };
}

function cameraPosition(frame: VisualEvidenceFrame): readonly [number, number, number] {
  return [frame.worldFromView[12] ?? 0, frame.worldFromView[13] ?? 0, frame.worldFromView[14] ?? 0];
}

function cameraForward(frame: VisualEvidenceFrame): readonly [number, number, number] | null {
  const x = -(frame.worldFromView[8] ?? 0);
  const y = -(frame.worldFromView[9] ?? 0);
  const z = -(frame.worldFromView[10] ?? 0);
  const length = Math.hypot(x, y, z);
  return length > 1e-6 ? [x / length, y / length, z / length] : null;
}

function poseDistance(a: VisualEvidenceFrame, b: VisualEvidenceFrame): number {
  const ap = cameraPosition(a), bp = cameraPosition(b);
  const translation = Math.hypot(ap[0] - bp[0], ap[1] - bp[1], ap[2] - bp[2]);
  const af = cameraForward(a), bf = cameraForward(b);
  let angle = 0;
  if (af !== null && bf !== null) {
    const dot = clamp(af[0] * bf[0] + af[1] * bf[1] + af[2] * bf[2], -1, 1);
    angle = Math.acos(dot);
  }
  // One unit roughly means a useful independent-view change.
  return translation / 0.08 + angle / ((10 * Math.PI) / 180);
}

function evidenceQuality(frame: VisualEvidenceFrame): number {
  let strongest = 0;
  for (const line of frame.lines) strongest = Math.max(strongest, line.strength);
  for (const junction of frame.junctions) strongest = Math.max(strongest, junction.strength);
  return strongest + Math.min(0.35, frame.lines.length * 0.012 + frame.junctions.length * 0.02);
}

function isolationScore(frames: readonly VisualEvidenceFrame[], index: number): number {
  let nearest = Number.POSITIVE_INFINITY;
  let second = Number.POSITIVE_INFINITY;
  for (let other = 0; other < frames.length; other += 1) {
    if (other === index) continue;
    const distance = poseDistance(frames[index], frames[other]);
    if (distance < nearest) { second = nearest; nearest = distance; }
    else if (distance < second) second = distance;
  }
  if (!Number.isFinite(nearest)) return Number.POSITIVE_INFINITY;
  if (!Number.isFinite(second)) second = nearest;
  // Endpoints of a scan path have only one close neighbour and therefore score
  // higher than a redundant interior sample with two close neighbours.
  return nearest + second * 0.65;
}

/**
 * Scan-wide, bounded RGB evidence reservoir. Unlike FIFO it preserves camera
 * baseline/orientation diversity across a long scan. Eviction uses a maximin-
 * style isolation score and visual evidence quality, so redundant middle poses
 * disappear before unique early/late viewpoints.
 */
export class PoseDiverseVisualEvidenceStore {
  private frames: VisualEvidenceFrame[] = [];

  constructor(private readonly maxFrames = 64) {}

  reset(): void { this.frames = []; }

  add(frame: VisualEvidenceFrame): void {
    this.frames = this.frames.filter((item) => item.keyframeId !== frame.keyframeId);
    this.frames.push(cloneFrame(frame));

    while (this.frames.length > Math.max(1, this.maxFrames)) {
      let removeIndex = -1;
      let weakestRetention = Number.POSITIVE_INFINITY;
      const newestKeyframe = Math.max(...this.frames.map((item) => item.keyframeId));
      for (let index = 0; index < this.frames.length; index += 1) {
        const item = this.frames[index];
        // Always give the just-arrived observation one eviction cycle to prove
        // its usefulness; otherwise a new view near an old one could disappear
        // before later motion establishes a new baseline.
        if (item.keyframeId === newestKeyframe && this.frames.length > 2) continue;
        const isolation = isolationScore(this.frames, index);
        const quality = evidenceQuality(item);
        const retention = isolation * (0.72 + Math.min(1.25, quality) * 0.28);
        if (
          retention < weakestRetention - 1e-9 ||
          (Math.abs(retention - weakestRetention) <= 1e-9 &&
            (removeIndex < 0 || item.keyframeId < this.frames[removeIndex].keyframeId))
        ) {
          weakestRetention = retention;
          removeIndex = index;
        }
      }
      if (removeIndex < 0) removeIndex = 0;
      this.frames.splice(removeIndex, 1);
    }

    this.frames.sort((a, b) => a.keyframeId - b.keyframeId);
  }

  snapshot(): readonly VisualEvidenceFrame[] { return this.frames.map(cloneFrame); }
  get size(): number { return this.frames.length; }
}