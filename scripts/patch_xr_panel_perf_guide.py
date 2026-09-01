from pathlib import Path

p = Path('webui/src/components/XrRuntimePanel.tsx')
s = p.read_text()

def once(old: str, new: str) -> None:
    global s
    count=s.count(old)
    if count != 1:
        raise SystemExit(f'Expected exactly one match, found {count}: {old[:120]!r}')
    s=s.replace(old,new,1)

once(
"""import { clientDiagnostics } from '../data/clientDiagnostics';
""",
"""import { clientDiagnostics } from '../data/clientDiagnostics';
import { StructuralGuideOverlay } from './StructuralGuideOverlay';
import { emptyXrPerformanceSnapshot } from '../xr/xrPerformanceProfiler';
""",
)

once(
"""  manualPointCount: 0,
  manualPointError: null,
""",
"""  manualPointCount: 0,
  manualEdgeCount: 0,
  manualPointError: null,
  performance: emptyXrPerformanceSnapshot,
""",
)

once(
"""            <div><dt>Manual corners</dt><dd>{runtimeState.manualPointCount.toLocaleString()}</dd></div>
            <div><dt>Linear speed</dt><dd>{runtimeState.linearSpeedMetersPerSecond.toFixed(2)} m/s</dd></div>
""",
"""            <div><dt>Manual corners</dt><dd>{runtimeState.manualPointCount.toLocaleString()}</dd></div>
            <div><dt>Confirmed edges</dt><dd>{runtimeState.manualEdgeCount.toLocaleString()}</dd></div>
            <div><dt>Frame p95</dt><dd>{runtimeState.performance.frameDeltaP95Ms.toFixed(1)} ms</dd></div>
            <div><dt>Fusion p50 / p95</dt><dd>{runtimeState.performance.fusionP50Ms.toFixed(1)} / {runtimeState.performance.fusionP95Ms.toFixed(1)} ms</dd></div>
            <div><dt>Coverage p95</dt><dd>{runtimeState.performance.coverageP95Ms.toFixed(1)} ms</dd></div>
            <div><dt>Long frames ≥33 / ≥50 ms</dt><dd>{runtimeState.performance.longFrames33Ms} / {runtimeState.performance.longFrames50Ms}</dd></div>
            <div><dt>Live render pressure</dt><dd>{runtimeState.performance.pressure.toUpperCase()} · {runtimeState.performance.adaptiveCoverageIntervalMs} ms</dd></div>
            <div><dt>Linear speed</dt><dd>{runtimeState.linearSpeedMetersPerSecond.toFixed(2)} m/s</dd></div>
""",
)

once(
"""      <div className="xr-immersive-overlay" aria-live="polite">
        {running && manualMode && (
""",
"""      <StructuralGuideOverlay
        runtime={runtime}
        runtimeState={runtimeState}
        interactionEnabled={running && !manualMode}
      />

      <div className="xr-immersive-overlay" aria-live="polite">
        {running && manualMode && (
""",
)

once(
"""          <div className="xr-manual-status">
            <span>Manual corner pins: <strong>{runtimeState.manualPointCount}</strong></span>
            <span>{manualMode ? 'Tap or drag/release = add · hold/drag = move last' : 'Optional operator geometry'}</span>
          </div>
""",
"""          <div className="xr-manual-status">
            <span>Operator geometry: <strong>{runtimeState.manualPointCount}</strong> pins · <strong>{runtimeState.manualEdgeCount}</strong> edges</span>
            <span>{manualMode ? 'Tap or drag/release = add · hold/drag = move last' : 'Yellow ? = suggested corner · LINE? = suggested edge'}</span>
          </div>
""",
)

p.write_text(s)
print('Patched',p)
