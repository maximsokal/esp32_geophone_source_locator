import type { ProjectPoint } from './manualControlPoints';

export interface CertifiedGuideCorner {
  id: string;
  position: ProjectPoint;
  confidence: number;
  support: number;
  source: 'three-plane' | 'multi-view-edge-endpoint';
}

let active = false;
let corners: CertifiedGuideCorner[] = [];

function cloneCorner(corner: CertifiedGuideCorner): CertifiedGuideCorner {
  return {
    ...corner,
    position: [corner.position[0], corner.position[1], corner.position[2]],
  };
}

function updateManualButtonVisibility(): void {
  if (typeof document === 'undefined') return;
  const hasTargets = corners.length > 0;
  document.documentElement.classList.toggle('xr-has-certified-corners', active && hasTargets);
  document.documentElement.dataset.xrCertifiedCorners = active ? String(corners.length) : '';

  // The runtime panel deliberately owns its own interaction state. Keep this
  // registry independent from React while still avoiding a misleading action:
  // when the certified state machine has no valid corner anywhere, there is no
  // meaningful manual corner action to show. If marking is already active we
  // keep the button visible so the operator can always exit the mode.
  for (const element of document.querySelectorAll<HTMLButtonElement>('.xr-manual-toggle')) {
    const pressed = element.getAttribute('aria-pressed') === 'true';
    element.hidden = active && !hasTargets && !pressed;
    if (active && !hasTargets) {
      element.title = 'Scan until a robust corner or finite edge endpoint is certified.';
    } else {
      element.removeAttribute('title');
    }
  }
}

export function setCertifiedGuideCorners(next: readonly CertifiedGuideCorner[]): void {
  active = true;
  const deduped: CertifiedGuideCorner[] = [];
  for (const candidate of next) {
    if (!candidate.position.every(Number.isFinite) || !Number.isFinite(candidate.confidence)) continue;
    const existing = deduped.find((item) => Math.hypot(
      item.position[0] - candidate.position[0],
      item.position[1] - candidate.position[1],
      item.position[2] - candidate.position[2],
    ) <= 0.045);
    if (existing === undefined) {
      deduped.push(cloneCorner(candidate));
    } else if (candidate.confidence > existing.confidence) {
      deduped[deduped.indexOf(existing)] = cloneCorner(candidate);
    }
  }
  corners = deduped.sort((a, b) => b.confidence - a.confidence).slice(0, 8);
  updateManualButtonVisibility();
}

export function deactivateCertifiedGuideRegistry(): void {
  active = false;
  corners = [];
  updateManualButtonVisibility();
}

export function isCertifiedGuideRegistryActive(): boolean {
  return active;
}

export function getCertifiedGuideCorners(): readonly CertifiedGuideCorner[] {
  return corners.map(cloneCorner);
}

export function findCertifiedGuideCorner(
  position: ProjectPoint,
  radiusMeters = 0.012,
): CertifiedGuideCorner | null {
  if (!active) return null;
  let nearest: CertifiedGuideCorner | null = null;
  let nearestDistance = Number.POSITIVE_INFINITY;
  for (const candidate of corners) {
    const distance = Math.hypot(
      candidate.position[0] - position[0],
      candidate.position[1] - position[1],
      candidate.position[2] - position[2],
    );
    if (distance <= radiusMeters && distance < nearestDistance) {
      nearest = candidate;
      nearestDistance = distance;
    }
  }
  return nearest === null ? null : cloneCorner(nearest);
}
