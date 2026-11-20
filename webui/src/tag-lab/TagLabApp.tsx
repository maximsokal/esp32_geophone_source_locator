import { useCallback, useEffect, useRef, useState } from 'react';
import { TagDetectorClient } from '../xr/fiducial/tagDetectorClient';
import { FrameGrabber, type FrameSource } from './frameSource';
import { LabPipeline, type LabTagState } from './labPipeline';
import { ReplayFrameSource } from './ReplayFrameSource';
import { TagLabDiagnostics, type LabSourceType } from './TagLabDiagnostics';
import { TagLabOverlay } from './TagLabOverlay';
import { classifyCameraError, listVideoDevices, WebcamFrameSource } from './WebcamFrameSource';

type SourceKind = 'webcam' | 'image' | 'video' | 'synthetic';
const RESOLUTIONS = ['640x480', '1280x720', '1920x1080'] as const;
const RATES: { label: string; ms: number }[] = [
  { label: 'every camera frame', ms: 0 }, { label: '10 / s (100 ms)', ms: 100 }, { label: '5 / s (200 ms)', ms: 200 },
  { label: '3 / s (330 ms, scanner default)', ms: 330 }, { label: '2 / s (500 ms)', ms: 500 },
];

interface Snapshot { [key: string]: number | null; }
const panel: React.CSSProperties = { background: '#1b2028', border: '1px solid #2c3440', borderRadius: 6, padding: 10, marginBottom: 10 };
const button: React.CSSProperties = { padding: '6px 12px', marginRight: 6, cursor: 'pointer' };

export function TagLabApp() {
  const query = new URLSearchParams(window.location.search);
  const [kind, setKind] = useState<SourceKind>((query.get('source') as SourceKind | null) ?? 'webcam');
  const [devices, setDevices] = useState<{ deviceId: string; label: string }[]>([]);
  const [deviceId, setDeviceId] = useState('');
  const [resolution, setResolution] = useState<(typeof RESOLUTIONS)[number]>('1280x720');
  const [rateMs, setRateMs] = useState(330);
  const [mirrored, setMirrored] = useState(false);
  const [showCorners, setShowCorners] = useState(true);
  const [running, setRunning] = useState(false);
  const [logging, setLogging] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [snap, setSnap] = useState<Snapshot>({});
  const [tags, setTags] = useState<LabTagState[]>([]);
  const [lastTag, setLastTag] = useState<string>('—');
  const [runInfo, setRunInfo] = useState<string>('log receiver: checking…');
  const [box, setBox] = useState({ width: 640, height: 360 });

  const videoRef = useRef<HTMLVideoElement | null>(null);
  const imageRef = useRef<HTMLImageElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const stageRef = useRef<HTMLDivElement | null>(null);
  const diag = useRef(new TagLabDiagnostics()).current;
  const pipelineRef = useRef<LabPipeline | null>(null);
  const clientRef = useRef<TagDetectorClient | null>(null);
  const sourceRef = useRef<FrameSource | null>(null);
  const unsubscribeRef = useRef<(() => void) | null>(null);
  const grabber = useRef(new FrameGrabber()).current;
  const rateRef = useRef(rateMs); rateRef.current = rateMs;
  const resolutionRef = useRef(resolution); resolutionRef.current = resolution;
  const lastSubmitRef = useRef(0);
  const fileRef = useRef<File | null>(null);

  const getStates = useCallback((): readonly LabTagState[] => pipelineRef.current?.tagStates() ?? [], []);

  useEffect(() => {
    void fetch('/lab-log/health').then((r) => r.json()).then((j: { runId: string }) => setRunInfo(`log receiver OK — run ${j.runId} → logs/tag-lab/${j.runId}/`))
      .catch(() => setRunInfo('log receiver NOT reachable (events are kept in memory only)'));
    if (navigator.mediaDevices !== undefined) void listVideoDevices(navigator.mediaDevices).then(setDevices).catch(() => undefined);
  }, []);

  useEffect(() => {
    const element = stageRef.current;
    if (element === null) return undefined;
    const observer = new ResizeObserver(() => setBox({ width: element.clientWidth, height: element.clientHeight }));
    observer.observe(element);
    setBox({ width: element.clientWidth, height: element.clientHeight });
    return () => observer.disconnect();
  }, []);

  const teardown = useCallback(async (reason: string) => {
    unsubscribeRef.current?.(); unsubscribeRef.current = null;
    sourceRef.current?.stop(); sourceRef.current = null;
    clientRef.current?.dispose(); clientRef.current = null;
    await diag.stopSession((kind as LabSourceType), { reason, ...(pipelineRef.current?.counters ?? {}) });
    setRunning(false);
  }, [diag, kind]);

  const start = useCallback(async () => {
    setError(null);
    await teardown('restart');
    const client = new TagDetectorClient();
    if (!client.workerBacked) { setError('AprilTag detector worker could not start.'); client.dispose(); return; }
    clientRef.current = client;
    const pipeline = new LabPipeline({ detector: client, diagnostics: diag });
    pipelineRef.current = pipeline;
    pipeline.newSession();
    if (logging) diag.startSession(kind, { kind, resolution, rateMs, userAgent: navigator.userAgent });
    try {
      let source: FrameSource;
      if (kind === 'webcam') {
        const webcam = new WebcamFrameSource(videoRef.current!);
        const [w, h] = resolution.split('x').map(Number);
        diag.emit('CAMERA_PERMISSION', 'webcam', { state: 'requesting', deviceId: deviceId || 'default' });
        const info = await webcam.start({ deviceId, width: w, height: h });
        diag.emit('CAMERA_PERMISSION', 'webcam', { state: 'granted' });
        diag.emit('CAMERA_READY', 'webcam', { label: info.label, width: info.width, height: info.height });
        void listVideoDevices(navigator.mediaDevices).then(setDevices);
        source = webcam;
      } else if (kind === 'synthetic') {
        source = ReplayFrameSource.synthetic(canvasRef.current!);
        diag.emit('CAMERA_READY', 'synthetic', { width: 1280, height: 720 });
      } else if (fileRef.current === null) {
        throw Object.assign(new Error(`Choose a ${kind} file first.`), { name: 'LabInput' });
      } else if (kind === 'image') {
        source = await ReplayFrameSource.fromImageFile(imageRef.current!, fileRef.current);
        diag.emit('CAMERA_READY', 'image', { width: imageRef.current!.naturalWidth, height: imageRef.current!.naturalHeight });
      } else {
        source = await ReplayFrameSource.fromVideoFile(videoRef.current!, fileRef.current);
        diag.emit('CAMERA_READY', 'video', { width: videoRef.current!.videoWidth, height: videoRef.current!.videoHeight });
      }
      sourceRef.current = source;
      lastSubmitRef.current = 0;
      unsubscribeRef.current = source.onFrame((nowMs) => {
        const size = source.size();
        if (size === null || pipelineRef.current !== pipeline) return;
        if (rateRef.current > 0 && nowMs - lastSubmitRef.current < rateRef.current) return;
        lastSubmitRef.current = nowMs;
        if (pipeline.busy) { pipeline.recordBusySkip(source.type); return; }
        /* Analyse at the selected size, never upscaling a smaller source. */
        const [rw, rh] = resolutionRef.current.split('x').map(Number);
        const scale = Math.min(1, rw / size.width, rh / size.height);
        const frame = grabber.grab(source.element, Math.round(size.width * scale), Math.round(size.height * scale), source.type, pipeline.sessionEpoch, performance.now());
        if (frame !== null) void pipeline.submit(frame);
      });
      setRunning(true);
    } catch (reason) {
      const failure = classifyCameraError(reason);
      const text = (reason as { name?: string }).name === 'LabInput' ? (reason as Error).message : failure.message;
      setError(text);
      diag.emit(kind === 'webcam' ? 'CAMERA_PERMISSION' : 'LAB_ERROR', kind, { state: failure.kind, message: text });
      diag.emit('LAB_ERROR', kind, { kind: failure.kind, message: text });
      await teardown('start-failed');
    }
  }, [deviceId, diag, grabber, kind, logging, rateMs, resolution, teardown]);

  const stop = useCallback(() => { void teardown('user-stop'); }, [teardown]);

  useEffect(() => {
    if (query.get('autostart') === '1') void start();
    return () => { void teardown('unmount'); diag.dispose(); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const timer = setInterval(() => {
      const pipeline = pipelineRef.current;
      if (pipeline === null) return;
      pipeline.tick();
      const aggregate = pipeline.aggregate();
      setSnap(aggregate as Snapshot);
      setTags(pipeline.tagStates().map((state) => ({ ...state })));
      setLastTag(pipeline.lastDetectedTagId === null ? '—' : `${pipeline.tagStates().find((s) => s.tagId === pipeline.lastDetectedTagId)?.node} (id ${pipeline.lastDetectedTagId})`);
      if (pipeline.lastError !== null) setError(`Detector: ${pipeline.lastError}`);
      const memory = (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory;
      if (diag.logging) void diag.pushPerformance({ ...aggregate, heapUsedBytes: memory?.usedJSHeapSize ?? null, logsDelivered: diag.delivered, logsFailed: diag.failed, logsDropped: diag.dropped });
    }, 500);
    return () => clearInterval(timer);
  }, [diag]);

  const showVideo = kind === 'webcam' || kind === 'video';
  const stageStyle = (visible: boolean): React.CSSProperties => ({ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'contain', display: visible ? 'block' : 'none', transform: mirrored && kind === 'webcam' ? 'scaleX(-1)' : undefined });

  return (
    <div style={{ display: 'flex', gap: 12, padding: 12, alignItems: 'flex-start' }}>
      <div style={{ flex: '1 1 640px', minWidth: 320 }}>
        <div ref={stageRef} data-testid="tag-lab-stage" style={{ position: 'relative', width: '100%', aspectRatio: '16 / 9', background: '#000', borderRadius: 6, overflow: 'hidden' }}>
          <video ref={videoRef} data-testid="tag-lab-video" style={stageStyle(showVideo)} playsInline muted />
          <img ref={imageRef} alt="" style={stageStyle(kind === 'image')} />
          <canvas ref={canvasRef} data-testid="tag-lab-synthetic" style={stageStyle(kind === 'synthetic')} />
          <TagLabOverlay getStates={getStates} width={box.width} height={box.height} mirrored={mirrored && kind === 'webcam'} showCorners={showCorners} />
        </div>
        <div style={{ ...panel, marginTop: 10 }} data-testid="tag-lab-errors" role="status">
          <b>Errors</b>: {error ?? 'none'}
        </div>
        <div style={{ ...panel, fontSize: 12, color: '#9aa4b2' }}>{runInfo}</div>
      </div>
      <div style={{ flex: '0 0 380px' }}>
        <div style={panel}>
          <div><label>Source{' '}
            <select value={kind} disabled={running} onChange={(e) => setKind(e.target.value as SourceKind)} data-testid="tag-lab-source">
              <option value="webcam">Webcam</option><option value="image">Image</option><option value="video">Recorded video</option><option value="synthetic">Synthetic replay</option>
            </select></label></div>
          {kind === 'webcam' && <div style={{ marginTop: 6 }}><label>Camera{' '}
            <select value={deviceId} disabled={running} onChange={(e) => setDeviceId(e.target.value)} data-testid="tag-lab-device">
              <option value="">default</option>{devices.map((d) => <option key={d.deviceId} value={d.deviceId}>{d.label}</option>)}
            </select></label></div>}
          {(kind === 'image' || kind === 'video') && <div style={{ marginTop: 6 }}><input type="file" accept={kind === 'image' ? 'image/*' : 'video/*'} disabled={running} onChange={(e) => { fileRef.current = e.target.files?.[0] ?? null; }} /></div>}
          <div style={{ marginTop: 6 }}><label>Resolution{' '}
            <select value={resolution} onChange={(e) => setResolution(e.target.value as (typeof RESOLUTIONS)[number])} data-testid="tag-lab-resolution">{RESOLUTIONS.map((r) => <option key={r}>{r}</option>)}</select></label></div>
          <div style={{ marginTop: 6 }}><label>Rate{' '}
            <select value={rateMs} onChange={(e) => setRateMs(Number(e.target.value))} data-testid="tag-lab-rate">{RATES.map((r) => <option key={r.ms} value={r.ms}>{r.label}</option>)}</select></label></div>
          <div style={{ marginTop: 6 }}>
            <label title="Added in the contrast-gate phase"><input type="checkbox" disabled /> contrast candidate gate (not implemented yet)</label>
          </div>
          <div style={{ marginTop: 6 }}>
            <label><input type="checkbox" checked={mirrored} onChange={(e) => setMirrored(e.target.checked)} /> mirror preview</label>{' '}
            <label><input type="checkbox" checked={showCorners} onChange={(e) => setShowCorners(e.target.checked)} /> corner coordinates</label>
          </div>
          <div style={{ marginTop: 6 }}>
            <label><input type="checkbox" checked={logging} disabled={running} onChange={(e) => setLogging(e.target.checked)} data-testid="tag-lab-logging" /> write log session (logs/tag-lab)</label>
          </div>
          <div style={{ marginTop: 10 }}>
            <button style={button} disabled={running} onClick={() => void start()} data-testid="tag-lab-start">Start capture</button>
            <button style={button} disabled={!running} onClick={stop} data-testid="tag-lab-stop">Stop capture</button>
          </div>
        </div>
        <div style={panel} data-testid="tag-lab-counters">
          <b>Counters</b>
          <div>decoded detections: <span data-testid="tag-lab-decoded">{snap.decoded ?? 0}</span></div>
          <div>frames submitted/processed: {snap.framesSubmitted ?? 0} / {snap.framesProcessed ?? 0}</div>
          <div>dropped frames: <span data-testid="tag-lab-dropped">{snap.framesDropped ?? 0}</span></div>
          <div>candidate regions: — (gate not implemented) · rejected: — (not counted yet)</div>
          <div>worker errors: {snap.workerErrors ?? 0}</div>
        </div>
        <div style={panel} data-testid="tag-lab-perf">
          <b>Performance</b>
          <div>capture FPS {snap.captureFps ?? '—'} · detector FPS {snap.detectorFps ?? '—'}</div>
          <div>latency P50/P95 {snap.latencyP50Ms ?? '—'} / {snap.latencyP95Ms ?? '—'} ms</div>
          <div>processing P50/P95 {snap.processingP50Ms ?? '—'} / {snap.processingP95Ms ?? '—'} ms</div>
        </div>
        <div style={panel}>
          <b>Tags</b> (last: <span data-testid="tag-lab-last">{lastTag}</span>)
          {tags.map((tag) => (
            <div key={tag.tagId} data-testid={`tag-lab-status-${tag.tagId}`} data-status={tag.status}>
              {tag.node} (id {tag.tagId}): {tag.status}{tag.hammingDistance === null ? '' : ` · ham ${tag.hammingDistance} · ×${tag.detectionCount}`}
            </div>
          ))}
        </div>
        <div style={{ ...panel, fontSize: 12, color: '#9aa4b2' }}>
          Webcam mode proves 2D camera acquisition, decoding, overlay and performance only. It gives no metric depth;
          3D localization needs the phone WebXR scanner.
        </div>
      </div>
    </div>
  );
}
