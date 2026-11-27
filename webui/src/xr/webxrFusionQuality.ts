export interface FusionPoseLike {
  position: {
    x: number;
    y: number;
    z: number;
  };
  orientation: {
    x: number;
    y: number;
    z: number;
    w: number;
  };
}

export type FusionDecisionKind = 'keyframe' | 'polish' | 'skip';

export type FusionMotionState =
  | 'initializing'
  | 'keyframe'
  | 'polish'
  | 'stationary'
  | 'redundant'
  | 'too-fast';

export interface FusionFrameDecision {
  kind: FusionDecisionKind;
  state: FusionMotionState;
  frameWeight: number;
  keyframeId: number;
  translationFromKeyframeMeters: number;
  rotationFromKeyframeRadians: number;
  linearSpeedMetersPerSecond: number;
  angularSpeedRadiansPerSecond: number;
  linearAccelerationMetersPerSecond2: number;
  angularAccelerationRadiansPerSecond2: number;
  motionQuality: number;
}

interface PoseSample {
  timeMs: number;
  px: number;
  py: number;
  pz: number;
  qx: number;
  qy: number;
  qz: number;
  qw: number;
}

const MIN_KEYFRAME_INTERVAL_MS = 260;
const POLISH_INTERVAL_MS = 480;
const KEYFRAME_TRANSLATION_METERS = 0.055;
const KEYFRAME_ROTATION_TRANSLATION_METERS = 0.025;
const KEYFRAME_ROTATION_RADIANS = (10 * Math.PI) / 180;

// Motion beyond these soft limits is increasingly down-weighted. The hard
// limits deliberately sit well below "shake the phone" motion: ARCore itself
// reports excessive motion as a tracking failure mode, and motion blur / depth
// reprojection should not become high-confidence map evidence.
const LINEAR_SPEED_SOFT = 0.32;
const LINEAR_SPEED_HARD = 0.90;
const ANGULAR_SPEED_SOFT = 0.95;
const ANGULAR_SPEED_HARD = 2.30;
const LINEAR_ACCEL_SOFT = 1.5;
const LINEAR_ACCEL_HARD = 5.0;
const ANGULAR_ACCEL_SOFT = 4.0;
const ANGULAR_ACCEL_HARD = 12.0;
const MIN_INTEGRATION_QUALITY = 0.25;

function clamp01(value: number): number {
  return Math.min(1, Math.max(0, value));
}

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function distance(a: PoseSample, b: PoseSample): number {
  return Math.hypot(a.px - b.px, a.py - b.py, a.pz - b.pz);
}

function quaternionAngularDistance(a: PoseSample, b: PoseSample): number {
  const dot = Math.abs(
    a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw,
  );
  return 2 * Math.acos(Math.min(1, Math.max(-1, dot)));
}

function falloff(value: number, soft: number, hard: number): number {
  if (!finite(value) || value <= soft) {
    return 1;
  }
  if (value >= hard) {
    return 0;
  }
  const t = (value - soft) / (hard - soft);
  // Smoothstep-shaped falloff avoids abrupt confidence discontinuities.
  const smooth = t * t * (3 - 2 * t);
  return 1 - smooth;
}

function toSample(timeMs: number, pose: FusionPoseLike): PoseSample | null {
  const values = [
    pose.position.x,
    pose.position.y,
    pose.position.z,
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w,
  ];
  if (!finite(timeMs) || !values.every(finite)) {
    return null;
  }

  const qLength = Math.hypot(
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w,
  );
  if (qLength < 1e-6) {
    return null;
  }

  return {
    timeMs,
    px: pose.position.x,
    py: pose.position.y,
    pz: pose.position.z,
    qx: pose.orientation.x / qLength,
    qy: pose.orientation.y / qLength,
    qz: pose.orientation.z / qLength,
    qw: pose.orientation.w / qLength,
  };
}

/** What {@link WebXrFusionKeyframeSelector.withdraw} puts back: the keyframe bookkeeping, not the motion estimate. */
export interface FusionSelectorMark {
  hadSample: boolean;
  lastKeyframe: PoseSample | null;
  lastKeyframeTimeMs: number;
  lastPolishTimeMs: number;
  keyframeId: number;
}

/**
 * Selects independent integration keyframes from ARCore/WebXR 6DoF poses.
 *
 * The important distinction is between "another depth frame" and "new map
 * evidence". A stationary phone may provide useful low-weight samples for
 * polishing an already-established surface, but it cannot increase spatial
 * coverage confidence. New confidence requires a geometrically novel pose.
 *
 * ARCore's WebXR pose is already camera + IMU sensor fusion and is timestamped
 * in the XR frame loop. Deriving velocity/acceleration here is therefore more
 * synchronized and portable than mixing in a second DeviceMotion clock.
 */
export class WebXrFusionKeyframeSelector {
  private previousSample: PoseSample | null = null;
  private lastKeyframe: PoseSample | null = null;
  private lastKeyframeTimeMs = Number.NEGATIVE_INFINITY;
  private lastPolishTimeMs = Number.NEGATIVE_INFINITY;
  private filteredLinearSpeed = 0;
  private filteredAngularSpeed = 0;
  private keyframeId = 0;

  reset(): void {
    this.previousSample = null;
    this.lastKeyframe = null;
    this.lastKeyframeTimeMs = Number.NEGATIVE_INFINITY;
    this.lastPolishTimeMs = Number.NEGATIVE_INFINITY;
    this.filteredLinearSpeed = 0;
    this.filteredAngularSpeed = 0;
    this.keyframeId = 0;
  }

  mark(): FusionSelectorMark {
    return { hadSample: this.previousSample !== null, lastKeyframe: this.lastKeyframe, lastKeyframeTimeMs: this.lastKeyframeTimeMs, lastPolishTimeMs: this.lastPolishTimeMs, keyframeId: this.keyframeId };
  }

  /** The frame of the last evaluate() was not captured: its viewpoint is not taken. Motion tracking keeps what it learnt from that pose. */
  withdraw(mark: FusionSelectorMark): void {
    this.lastKeyframe = mark.lastKeyframe;
    this.lastKeyframeTimeMs = mark.lastKeyframeTimeMs;
    this.lastPolishTimeMs = mark.lastPolishTimeMs;
    this.keyframeId = mark.keyframeId;
    if (!mark.hadSample) this.previousSample = null;
  }

  evaluate(timeMs: number, pose: FusionPoseLike): FusionFrameDecision {
    const sample = toSample(timeMs, pose);
    if (sample === null) {
      return this.skip('initializing', 0);
    }

    if (this.previousSample === null) {
      this.previousSample = sample;
      this.lastKeyframe = sample;
      this.lastKeyframeTimeMs = timeMs;
      this.lastPolishTimeMs = timeMs;
      this.keyframeId = 1;
      return {
        kind: 'keyframe',
        state: 'keyframe',
        frameWeight: 1,
        keyframeId: this.keyframeId,
        translationFromKeyframeMeters: 0,
        rotationFromKeyframeRadians: 0,
        linearSpeedMetersPerSecond: 0,
        angularSpeedRadiansPerSecond: 0,
        linearAccelerationMetersPerSecond2: 0,
        angularAccelerationRadiansPerSecond2: 0,
        motionQuality: 1,
      };
    }

    const previous = this.previousSample;
    const dtSeconds = Math.max(0.01, (timeMs - previous.timeMs) / 1000);
    const rawLinearSpeed = distance(sample, previous) / dtSeconds;
    const rawAngularSpeed = quaternionAngularDistance(sample, previous) / dtSeconds;

    const previousFilteredLinear = this.filteredLinearSpeed;
    const previousFilteredAngular = this.filteredAngularSpeed;
    const alpha = 0.42;
    this.filteredLinearSpeed += (rawLinearSpeed - this.filteredLinearSpeed) * alpha;
    this.filteredAngularSpeed += (rawAngularSpeed - this.filteredAngularSpeed) * alpha;

    const linearAcceleration =
      Math.abs(this.filteredLinearSpeed - previousFilteredLinear) / dtSeconds;
    const angularAcceleration =
      Math.abs(this.filteredAngularSpeed - previousFilteredAngular) / dtSeconds;

    this.previousSample = sample;

    const speedQuality = falloff(
      this.filteredLinearSpeed,
      LINEAR_SPEED_SOFT,
      LINEAR_SPEED_HARD,
    );
    const angularQuality = falloff(
      this.filteredAngularSpeed,
      ANGULAR_SPEED_SOFT,
      ANGULAR_SPEED_HARD,
    );
    const accelerationQuality = falloff(
      linearAcceleration,
      LINEAR_ACCEL_SOFT,
      LINEAR_ACCEL_HARD,
    );
    const angularAccelerationQuality = falloff(
      angularAcceleration,
      ANGULAR_ACCEL_SOFT,
      ANGULAR_ACCEL_HARD,
    );
    const motionQuality = clamp01(
      Math.min(
        speedQuality,
        angularQuality,
        accelerationQuality,
        angularAccelerationQuality,
      ),
    );

    const keyframe = this.lastKeyframe ?? sample;
    const translationFromKeyframe = distance(sample, keyframe);
    const rotationFromKeyframe = quaternionAngularDistance(sample, keyframe);
    const keyframeInterval = timeMs - this.lastKeyframeTimeMs;

    const common = {
      keyframeId: this.keyframeId,
      translationFromKeyframeMeters: translationFromKeyframe,
      rotationFromKeyframeRadians: rotationFromKeyframe,
      linearSpeedMetersPerSecond: this.filteredLinearSpeed,
      angularSpeedRadiansPerSecond: this.filteredAngularSpeed,
      linearAccelerationMetersPerSecond2: linearAcceleration,
      angularAccelerationRadiansPerSecond2: angularAcceleration,
      motionQuality,
    };

    if (motionQuality < MIN_INTEGRATION_QUALITY) {
      return {
        kind: 'skip',
        state: 'too-fast',
        frameWeight: 0,
        ...common,
      };
    }

    const novelTranslation =
      translationFromKeyframe >= KEYFRAME_TRANSLATION_METERS;
    const novelCombinedView =
      translationFromKeyframe >= KEYFRAME_ROTATION_TRANSLATION_METERS &&
      rotationFromKeyframe >= KEYFRAME_ROTATION_RADIANS;

    if (
      keyframeInterval >= MIN_KEYFRAME_INTERVAL_MS &&
      (novelTranslation || novelCombinedView)
    ) {
      this.keyframeId += 1;
      this.lastKeyframe = sample;
      this.lastKeyframeTimeMs = timeMs;
      this.lastPolishTimeMs = timeMs;
      return {
        kind: 'keyframe',
        state: 'keyframe',
        frameWeight: Math.max(0.35, motionQuality),
        ...common,
        keyframeId: this.keyframeId,
      };
    }

    if (timeMs - this.lastPolishTimeMs >= POLISH_INTERVAL_MS) {
      this.lastPolishTimeMs = timeMs;
      const essentiallyStationary =
        translationFromKeyframe < 0.015 && rotationFromKeyframe < (3 * Math.PI) / 180;
      return {
        kind: 'polish',
        state: essentiallyStationary ? 'stationary' : 'polish',
        // A polish observation is deliberately weak. It may refine an already
        // matched voxel but never creates a voxel or increases coverage evidence.
        frameWeight: Math.max(0.04, motionQuality * 0.18),
        ...common,
      };
    }

    return {
      kind: 'skip',
      state: translationFromKeyframe < 0.015 ? 'stationary' : 'redundant',
      frameWeight: 0,
      ...common,
    };
  }

  private skip(state: FusionMotionState, motionQuality: number): FusionFrameDecision {
    return {
      kind: 'skip',
      state,
      frameWeight: 0,
      keyframeId: this.keyframeId,
      translationFromKeyframeMeters: 0,
      rotationFromKeyframeRadians: 0,
      linearSpeedMetersPerSecond: 0,
      angularSpeedRadiansPerSecond: 0,
      linearAccelerationMetersPerSecond2: 0,
      angularAccelerationRadiansPerSecond2: 0,
      motionQuality,
    };
  }
}
