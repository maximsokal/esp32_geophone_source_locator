import { scheduleVideoFrames, type FrameSource, type FrameSourceInfo } from './frameSource';

export interface MediaDevicesLike {
  getUserMedia(constraints: MediaStreamConstraints): Promise<MediaStream>;
  enumerateDevices(): Promise<MediaDeviceInfo[]>;
}

export type CameraFailureKind = 'permission-denied' | 'no-camera' | 'camera-busy' | 'constraints' | 'insecure-context' | 'unknown';

export function classifyCameraError(error: unknown): { kind: CameraFailureKind; message: string } {
  const name = typeof error === 'object' && error !== null && 'name' in error ? String((error as { name: unknown }).name) : '';
  const message = error instanceof Error ? error.message : String(error);
  if (name === 'NotAllowedError' || name === 'SecurityError') return { kind: 'permission-denied', message: `Camera permission denied: ${message}` };
  if (name === 'NotFoundError' || name === 'DevicesNotFoundError') return { kind: 'no-camera', message: `No camera found: ${message}` };
  if (name === 'NotReadableError' || name === 'TrackStartError') return { kind: 'camera-busy', message: `Camera is in use or unreadable: ${message}` };
  if (name === 'OverconstrainedError') return { kind: 'constraints', message: `Camera cannot satisfy the requested resolution: ${message}` };
  if (name === 'TypeError') return { kind: 'insecure-context', message: `Camera API unavailable (use http://127.0.0.1 or localhost): ${message}` };
  return { kind: 'unknown', message };
}

export async function listVideoDevices(media: Pick<MediaDevicesLike, 'enumerateDevices'>): Promise<{ deviceId: string; label: string }[]> {
  const devices = await media.enumerateDevices();
  return devices.filter((d) => d.kind === 'videoinput').map((d, index) => ({ deviceId: d.deviceId, label: d.label || `Camera ${index + 1}` }));
}

export interface WebcamStartOptions { deviceId?: string; width: number; height: number; }

export class WebcamFrameSource implements FrameSource {
  readonly type = 'webcam' as const;
  readonly element: HTMLVideoElement;
  private stream: MediaStream | null = null;
  private readonly cancelers = new Set<() => void>();

  constructor(video: HTMLVideoElement, private readonly media: MediaDevicesLike = navigator.mediaDevices) {
    this.element = video;
  }

  get active(): boolean { return this.stream !== null; }

  async start(options: WebcamStartOptions): Promise<FrameSourceInfo> {
    this.stop();
    if (this.media === undefined || typeof this.media.getUserMedia !== 'function') {
      throw Object.assign(new TypeError('navigator.mediaDevices is unavailable'), { name: 'TypeError' });
    }
    const video: MediaTrackConstraints = { width: { ideal: options.width }, height: { ideal: options.height } };
    if (options.deviceId !== undefined && options.deviceId !== '') video.deviceId = { exact: options.deviceId };
    const stream = await this.media.getUserMedia({ video, audio: false });
    this.stream = stream;
    this.element.srcObject = stream;
    this.element.muted = true;
    this.element.playsInline = true;
    try {
      await this.element.play();
    } catch (error) {
      this.stop();
      throw error;
    }
    const track = stream.getVideoTracks()[0];
    const settings = track?.getSettings?.() ?? {};
    return { label: track?.label ?? 'camera', width: settings.width ?? this.element.videoWidth, height: settings.height ?? this.element.videoHeight };
  }

  size(): { width: number; height: number } | null {
    return this.element.videoWidth > 0 && this.element.videoHeight > 0 ? { width: this.element.videoWidth, height: this.element.videoHeight } : null;
  }

  onFrame(callback: (nowMs: number) => void): () => void {
    const cancel = scheduleVideoFrames(this.element, callback);
    const wrapped = (): void => { cancel(); this.cancelers.delete(wrapped); };
    this.cancelers.add(wrapped);
    return wrapped;
  }

  /** Always stops every MediaStream track, detaches the stream and cancels frame callbacks. */
  stop(): void {
    for (const cancel of [...this.cancelers]) cancel();
    if (this.stream !== null) {
      for (const track of this.stream.getTracks()) track.stop();
      this.stream = null;
    }
    this.element.pause?.();
    this.element.srcObject = null;
  }
}
