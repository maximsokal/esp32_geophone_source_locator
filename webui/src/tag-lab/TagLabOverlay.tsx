import { useEffect, useRef } from 'react';
import { frameCornerToDisplay, type DisplayBox } from './overlayMapping';
import type { LabTagState } from './labPipeline';

export interface OverlayQuad {
  tagId: number;
  node: string;
  status: LabTagState['status'];
  points: [number, number][];
  label: string;
}

/** Pure geometry step: tag states -> display-space quads (only tags that currently hold corners). */
export function buildOverlayQuads(states: readonly LabTagState[], box: DisplayBox, mirrored: boolean): OverlayQuad[] {
  const quads: OverlayQuad[] = [];
  for (const state of states) {
    if (state.corners === null || state.status === 'NOT SEEN') continue;
    const frame = { width: state.frameWidth, height: state.frameHeight };
    quads.push({
      tagId: state.tagId, node: state.node, status: state.status,
      points: state.corners.map((corner) => frameCornerToDisplay(corner, frame, box, mirrored)),
      label: `${state.node} (id ${state.tagId})`,
    });
  }
  return quads;
}

const COLORS: Record<LabTagState['status'], string> = { 'DETECTED 2D': '#33d17a', LOST: '#8a8f98', 'NOT SEEN': '#555' };

export function drawOverlay(ctx: CanvasRenderingContext2D, quads: readonly OverlayQuad[], showCorners: boolean): void {
  ctx.lineWidth = 3;
  ctx.font = '600 16px system-ui, sans-serif';
  ctx.textBaseline = 'bottom';
  for (const quad of quads) {
    const color = COLORS[quad.status];
    ctx.strokeStyle = color; ctx.fillStyle = color;
    ctx.globalAlpha = quad.status === 'LOST' ? 0.5 : 1;
    ctx.beginPath();
    quad.points.forEach(([x, y], index) => (index === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y)));
    ctx.closePath(); ctx.stroke();
    const top = quad.points.reduce((best, point) => (point[1] < best[1] ? point : best), quad.points[0]);
    const text = quad.status === 'LOST' ? `${quad.label} LOST` : quad.label;
    const width = ctx.measureText(text).width + 10;
    ctx.fillStyle = 'rgba(0,0,0,0.65)'; ctx.fillRect(top[0] - 4, top[1] - 24, width, 22);
    ctx.fillStyle = color; ctx.fillText(text, top[0], top[1] - 5);
    if (showCorners) {
      quad.points.forEach(([x, y], index) => {
        ctx.beginPath(); ctx.arc(x, y, 4, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#ffd866'; ctx.font = '11px monospace'; ctx.textBaseline = 'top';
        ctx.fillText(`${index}:${x.toFixed(0)},${y.toFixed(0)}`, x + 6, y + 4);
        ctx.fillStyle = color; ctx.font = '600 16px system-ui, sans-serif'; ctx.textBaseline = 'bottom';
      });
    }
    ctx.globalAlpha = 1;
  }
}

interface Props {
  getStates: () => readonly LabTagState[];
  /** Size of the element the overlay sits on (CSS px). */
  width: number;
  height: number;
  mirrored: boolean;
  showCorners: boolean;
}

/** Transparent canvas redrawn every animation frame from the pipeline's current tag states. */
export function TagLabOverlay({ getStates, width, height, mirrored, showCorners }: Props) {
  const ref = useRef<HTMLCanvasElement | null>(null);
  useEffect(() => {
    let raf = 0;
    const dpr = window.devicePixelRatio || 1;
    const canvas = ref.current;
    if (canvas === null) return undefined;
    canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
    const ctx = canvas.getContext('2d');
    if (ctx === null) return undefined;
    const draw = (): void => {
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, width, height);
      drawOverlay(ctx, buildOverlayQuads(getStates(), { width, height }, mirrored), showCorners);
      raf = requestAnimationFrame(draw);
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  }, [getStates, width, height, mirrored, showCorners]);
  return <canvas ref={ref} data-testid="tag-lab-overlay" style={{ position: 'absolute', inset: 0, width, height, pointerEvents: 'none' }} />;
}
