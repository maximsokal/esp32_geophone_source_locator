import type { ProjectPoint } from './manualControlPoints';

export interface CertifiedGuideCorner {
  id: string;
  position: ProjectPoint;
  confidence: number;
  support: number;
  source: 'three-plane' | 'multi-view-edge-endpoint' | 'multi-edge-vertex';
}

export interface CertifiedGuideEdge {
  id: string;
  start: ProjectPoint;
  end: ProjectPoint;
  confidence: number;
  support: number;
  observations: number;
  source: 'robust-two-plane';
}

let active = false;
let corners: CertifiedGuideCorner[] = [];
let edges: CertifiedGuideEdge[] = [];

function clonePoint(point: ProjectPoint): ProjectPoint { return [point[0], point[1], point[2]]; }
function cloneCorner(corner: CertifiedGuideCorner): CertifiedGuideCorner { return { ...corner, position: clonePoint(corner.position) }; }
function cloneEdge(edge: CertifiedGuideEdge): CertifiedGuideEdge { return { ...edge, start: clonePoint(edge.start), end: clonePoint(edge.end) }; }

function pointSegmentDistance(point: ProjectPoint, edge: CertifiedGuideEdge): number {
  const dx = edge.end[0] - edge.start[0], dy = edge.end[1] - edge.start[1], dz = edge.end[2] - edge.start[2];
  const squared = dx * dx + dy * dy + dz * dz;
  if (squared < 1e-12) return Math.hypot(point[0] - edge.start[0], point[1] - edge.start[1], point[2] - edge.start[2]);
  const t = Math.max(0, Math.min(1,
    ((point[0] - edge.start[0]) * dx + (point[1] - edge.start[1]) * dy + (point[2] - edge.start[2]) * dz) / squared,
  ));
  return Math.hypot(
    point[0] - (edge.start[0] + dx * t),
    point[1] - (edge.start[1] + dy * t),
    point[2] - (edge.start[2] + dz * t),
  );
}

function updateManualButtonVisibility(): void {
  if (typeof document === 'undefined') return;
  const hasTargets = corners.length > 0 || edges.length > 0;
  document.documentElement.classList.toggle('xr-has-certified-corners', active && corners.length > 0);
  document.documentElement.classList.toggle('xr-has-certified-geometry', active && hasTargets);
  document.documentElement.dataset.xrCertifiedCorners = active ? String(corners.length) : '';
  document.documentElement.dataset.xrCertifiedEdges = active ? String(edges.length) : '';
  for (const element of document.querySelectorAll<HTMLButtonElement>('.xr-manual-toggle')) {
    const pressed = element.getAttribute('aria-pressed') === 'true';
    element.hidden = active && !hasTargets && !pressed;
    if (active && !hasTargets) element.title = 'Scan until a robust edge or corner is certified.';
    else element.removeAttribute('title');
  }
}

export function setCertifiedGuideCorners(next: readonly CertifiedGuideCorner[]): void {
  active = true;
  const deduped: CertifiedGuideCorner[] = [];
  for (const candidate of next) {
    if (!candidate.position.every(Number.isFinite) || !Number.isFinite(candidate.confidence)) continue;
    const existing = deduped.find((item) => Math.hypot(
      item.position[0] - candidate.position[0], item.position[1] - candidate.position[1], item.position[2] - candidate.position[2],
    ) <= 0.045);
    if (existing === undefined) deduped.push(cloneCorner(candidate));
    else if (candidate.confidence > existing.confidence) deduped[deduped.indexOf(existing)] = cloneCorner(candidate);
  }
  corners = deduped.sort((a, b) => b.confidence - a.confidence).slice(0, 8);
  updateManualButtonVisibility();
}

export function setCertifiedGuideEdges(next: readonly CertifiedGuideEdge[]): void {
  active = true;
  const deduped: CertifiedGuideEdge[] = [];
  for (const candidate of next) {
    if (
      !candidate.start.every(Number.isFinite) || !candidate.end.every(Number.isFinite) || !Number.isFinite(candidate.confidence) ||
      Math.hypot(candidate.end[0] - candidate.start[0], candidate.end[1] - candidate.start[1], candidate.end[2] - candidate.start[2]) < 0.035
    ) continue;
    const midpoint: ProjectPoint = [
      (candidate.start[0] + candidate.end[0]) * 0.5,
      (candidate.start[1] + candidate.end[1]) * 0.5,
      (candidate.start[2] + candidate.end[2]) * 0.5,
    ];
    const existing = deduped.find((item) => {
      const other: ProjectPoint = [(item.start[0] + item.end[0]) * 0.5, (item.start[1] + item.end[1]) * 0.5, (item.start[2] + item.end[2]) * 0.5];
      return Math.hypot(midpoint[0] - other[0], midpoint[1] - other[1], midpoint[2] - other[2]) <= 0.065;
    });
    if (existing === undefined) deduped.push(cloneEdge(candidate));
    else if (candidate.confidence > existing.confidence) deduped[deduped.indexOf(existing)] = cloneEdge(candidate);
  }
  edges = deduped.sort((a, b) => b.confidence - a.confidence).slice(0, 10);
  updateManualButtonVisibility();
}

export function deactivateCertifiedGuideRegistry(): void { active = false; corners = []; edges = []; updateManualButtonVisibility(); }
export function isCertifiedGuideRegistryActive(): boolean { return active; }
export function getCertifiedGuideCorners(): readonly CertifiedGuideCorner[] { return corners.map(cloneCorner); }
export function getCertifiedGuideEdges(): readonly CertifiedGuideEdge[] { return edges.map(cloneEdge); }

export function findCertifiedGuideCorner(position: ProjectPoint, radiusMeters = 0.012): CertifiedGuideCorner | null {
  if (!active) return null;
  let nearest: CertifiedGuideCorner | null = null, nearestDistance = Number.POSITIVE_INFINITY;
  for (const candidate of corners) {
    const distance = Math.hypot(candidate.position[0] - position[0], candidate.position[1] - position[1], candidate.position[2] - position[2]);
    if (distance <= radiusMeters && distance < nearestDistance) { nearest = candidate; nearestDistance = distance; }
  }
  return nearest === null ? null : cloneCorner(nearest);
}

export function findCertifiedGuideEdgeAtPoint(position: ProjectPoint, radiusMeters = 0.010): CertifiedGuideEdge | null {
  if (!active) return null;
  let nearest: CertifiedGuideEdge | null = null, nearestDistance = Number.POSITIVE_INFINITY;
  for (const candidate of edges) {
    const distance = pointSegmentDistance(position, candidate);
    if (distance <= radiusMeters && distance < nearestDistance) { nearest = candidate; nearestDistance = distance; }
  }
  return nearest === null ? null : cloneEdge(nearest);
}
