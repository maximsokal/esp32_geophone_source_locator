import { useEffect, useRef, useState } from 'react';

import { liveTagOverlayQuads, type LiveTagTracker, type OverlayQuad } from '../xr/fiducial/liveTagTracker';
import type { WebXrRuntime } from '../xr/webxrRuntime';

interface Props {
  runtime: WebXrRuntime;
  tracker: LiveTagTracker;
  active: boolean;
}

const STYLES: Record<OverlayQuad['style'], { stroke: string; dash: string | undefined; width: number; opacity: number }> = {
  provisional: { stroke: '#f3b84b', dash: '1.2 1', width: 3, opacity: 0.95 },
  anchored: { stroke: '#4be08f', dash: undefined, width: 3, opacity: 1 },
  confirmed: { stroke: '#36ff9a', dash: undefined, width: 5, opacity: 1 },
  lost: { stroke: '#9aa4b2', dash: '1.2 1.6', width: 2, opacity: 0.55 },
};

/**
 * Live tag outlines drawn over the AR view. Symbols only (device name, check mark), no translatable text: dashed amber =
 * decoded, no pose yet; solid green = anchored in the world and re-projected every frame; thick + check = confirmed from
 * several viewpoints; dim dashed = not decoded for a moment (the last anchored position is kept).
 */
export function LiveTagOverlay({ runtime, tracker, active }: Props) {
  const [quads, setQuads] = useState<OverlayQuad[]>([]);
  const lastKey = useRef('');
  useEffect(() => {
    if (!active) { setQuads([]); lastKey.current = ''; return undefined; }
    let raf = 0;
    let lastDraw = 0;
    const frame = (now: number): void => {
      raf = requestAnimationFrame(frame);
      if (now - lastDraw < 33) return;
      lastDraw = now;
      const next = liveTagOverlayQuads(tracker.states(), (point) => runtime.projectProjectPointToNormalized(point));
      const key = next.map((q) => `${q.tagId}${q.style}${q.points.map((p) => `${p[0].toFixed(3)},${p[1].toFixed(3)}`).join(';')}`).join('|');
      if (key !== lastKey.current) { lastKey.current = key; setQuads(next); }
    };
    raf = requestAnimationFrame(frame);
    return () => cancelAnimationFrame(raf);
  }, [active, runtime, tracker]);

  if (!active || quads.length === 0) return null;
  return (
    <div className="xr-live-tags" data-testid="xr-live-tags" style={{ position: 'fixed', inset: 0, zIndex: 47, pointerEvents: 'none' }} aria-hidden="true">
      <svg viewBox="0 0 100 100" preserveAspectRatio="none" style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
        {quads.map((quad) => {
          const style = STYLES[quad.style];
          return (
            <polygon
              key={quad.tagId}
              data-tag-id={quad.tagId}
              data-style={quad.style}
              points={quad.points.map(([u, v]) => `${(u * 100).toFixed(2)},${(v * 100).toFixed(2)}`).join(' ')}
              fill="rgba(0,0,0,0)"
              stroke={style.stroke}
              strokeWidth={style.width}
              strokeDasharray={style.dash}
              strokeOpacity={style.opacity}
              strokeLinejoin="round"
              vectorEffect="non-scaling-stroke"
            />
          );
        })}
      </svg>
      {quads.map((quad) => {
        const top = quad.points.reduce((best, point) => (point[1] < best[1] ? point : best), quad.points[0]);
        const style = STYLES[quad.style];
        return (
          <div
            key={quad.tagId}
            data-tag-label={quad.node}
            style={{
              position: 'absolute', left: `${top[0] * 100}%`, top: `${top[1] * 100}%`, transform: 'translate(-50%, calc(-100% - 4px))',
              padding: '1px 6px', borderRadius: 8, background: 'rgba(8,14,24,0.72)', color: style.stroke, opacity: style.opacity,
              font: '700 13px system-ui, sans-serif', whiteSpace: 'nowrap',
            }}
          >
            {quad.node}{quad.style === 'confirmed' ? ' ✓' : ''}
          </div>
        );
      })}
    </div>
  );
}
