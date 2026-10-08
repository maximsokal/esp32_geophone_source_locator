import { useEffect, useEffectEvent, useMemo, useRef, useState } from 'react';

import { clientDiagnostics } from '../data/clientDiagnostics';
import {
  deactivateCertifiedGuideRegistry,
  setCertifiedGuideCorners,
  setCertifiedGuideEdges,
} from '../xr/certifiedCornerRegistry';
import type { StructuralGuideSnapshot } from '../xr/depthPointCloud';
import {
  authorizeCertifiedManualPlacement,
  type ProjectPoint,
} from '../xr/manualControlPoints';
import { solveManualPlacementOnStableSurface } from '../xr/manualPlacementSurface';
import {
  certifyCornerByVisualReprojection,
  certifyEndpointByVisualReprojection,
} from '../xr/reprojectionCertificate';
import {
  computeScanCapabilityReadiness,
  ScanViewpointHistory,
  type SurfaceComponentReadinessSummary,
} from '../xr/scanCapabilityReadiness';
import { compactScanReadinessLabel } from '../xr/scanReadinessUi';
import { certifyOperationalStructuralEdges } from '../xr/structuralEdgeCertification';
import { inferStructuralVerticesFromEdges } from '../xr/structuralEdgeVertices';
import { detectEdgeEndpointObservations } from '../xr/structuralEdgeEndpoints';
import {
  type StructuralCornerCandidate,
  type StructuralEdgeCandidate,
  type StructuralGuideResult,
  type StructuralGuideViewpoint,
} from '../xr/structuralGuide';
import { StructuralGuideClient } from '../xr/structuralGuideClient';
import { applyStructuralVisualCorroboration } from '../xr/structuralVisualCorroboration';
import type { VisualEvidenceFrame } from '../xr/visualLineEvidence';
import type { WebXrRuntime, WebXrRuntimeState } from '../xr/webxrRuntime';
import { getVisualEvidenceFrames } from '../xr/xrGuidanceRegistry';
import { useLocale, useT } from '../i18n';
import { formatDistance, formatNumber, formatPercent } from '../i18n/format';

import './StructuralGuideOverlay.css';

interface Props {
  runtime: WebXrRuntime;
  runtimeState: WebXrRuntimeState;
  interactionEnabled: boolean;
}

interface ProjectedCorner {
  candidate: StructuralCornerCandidate;
  u: number;
  v: number;
  score: number;
}

interface ProjectedEdge {
  candidate: StructuralEdgeCandidate;
  startU: number;
  startV: number;
  endU: number;
  endV: number;
}

type StructuralGuideResultWithReadiness = StructuralGuideResult & {
  surfaceReadiness?: SurfaceComponentReadinessSummary;
};

const MIN_STABLE_POINTS = 180;
const MIN_RAW_STRUCTURAL_POINTS = 60;
const MIN_KEYFRAMES = 8;
const MIN_KEYFRAME_ADVANCE = 4;
const BASE_ANALYSIS_INTERVAL_MS = 1900;
const MIN_VISIBLE_CORNER_CONFIDENCE = 0.70;
const MIN_CERTIFIED_CORNER_CONFIDENCE = 0.70;
const MAX_CERTIFICATE_SHIFT_METERS = 0.055;
const MAX_CERTIFIED_UNCERTAINTY_METERS = 0.032;
const VISUAL_CERTIFICATE_MIN_FRAMES = 3;
const MIN_VISUAL_STRUCTURE_STRENGTH = 0.30;
const VIEW_MARGIN = 0.07;
const MAX_VISIBLE_CORNERS = 4;
const REJECTED_CORNER_RADIUS_METERS = 0.14;

function analysisInterval(state: WebXrRuntimeState): number {
  if (state.performance.pressure === 'high') return 4200;
  if (state.performance.pressure === 'medium') return 2800;
  return BASE_ANALYSIS_INTERVAL_MS;
}

function shouldAnalyzeMotion(state: WebXrRuntimeState): boolean {
  return state.motionState !== 'too-fast' && state.motionState !== 'paused';
}

function normalize3(vector: readonly [number, number, number]): readonly [number, number, number] | null {
  const magnitude = Math.hypot(vector[0], vector[1], vector[2]);
  return magnitude > 1e-8 ? [vector[0] / magnitude, vector[1] / magnitude, vector[2] / magnitude] : null;
}

function pointDistance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

function insideWorkingView(u: number, v: number): boolean {
  return u >= VIEW_MARGIN && u <= 1 - VIEW_MARGIN && v >= VIEW_MARGIN && v <= 1 - VIEW_MARGIN;
}

function percent(value: number, locale: 'en' | 'uk'): string {
  return formatPercent(locale, Math.max(0, Math.min(1, value)));
}

function hasUsableLineCoverage(frames: readonly VisualEvidenceFrame[]): boolean {
  return frames.filter((frame) => frame.lines.some((line) => line.strength >= MIN_VISUAL_STRUCTURE_STRENGTH)).length
    >= VISUAL_CERTIFICATE_MIN_FRAMES;
}

function hasUsableJunctionCoverage(frames: readonly VisualEvidenceFrame[]): boolean {
  return frames.filter((frame) => frame.junctions.some((junction) => junction.strength >= MIN_VISUAL_STRUCTURE_STRENGTH)).length
    >= VISUAL_CERTIFICATE_MIN_FRAMES;
}

function visualCheckActive(frames: readonly VisualEvidenceFrame[]): boolean {
  return hasUsableLineCoverage(frames) || hasUsableJunctionCoverage(frames);
}

function cornerSource(candidate: StructuralCornerCandidate): 'three-plane' | 'multi-view-edge-endpoint' | 'multi-edge-vertex' {
  if (candidate.id.startsWith('endpoint:')) return 'multi-view-edge-endpoint';
  if (candidate.id.startsWith('edge-vertex:')) return 'multi-edge-vertex';
  return 'three-plane';
}

/** Convert the current WebXR viewer pose into the same project frame as fused surfels. */
export function structuralGuideViewpointFromState(state: WebXrRuntimeState): StructuralGuideViewpoint | null {
  const pose = state.pose;
  if (pose === null || state.keyframeCount <= 0) return null;
  const [x, y, z] = pose.position;
  const [qx, qy, qz, qw] = pose.orientation;

  const worldForward: readonly [number, number, number] = [
    -2 * (qx * qz + qw * qy),
    -2 * (qy * qz - qw * qx),
    -(1 - 2 * (qx * qx + qy * qy)),
  ];
  const forward = normalize3([worldForward[0], -worldForward[2], worldForward[1]]);
  return {
    position: [x, -z, y],
    forward: forward ?? undefined,
    keyframeId: state.keyframeCount,
  };
}

function certifyCandidates(
  result: StructuralGuideResult | null,
  snapshot: StructuralGuideSnapshot | null,
  visualFrames: readonly VisualEvidenceFrame[],
  rejected: ReadonlySet<string>,
  rejectedPositions: readonly ProjectPoint[],
): readonly StructuralCornerCandidate[] {
  if (result === null || snapshot === null) return [];
  const endpointObservations = result.corners.some((candidate) => candidate.id.startsWith('endpoint:'))
    ? detectEdgeEndpointObservations(snapshot)
    : [];
  const checkLineVisual = hasUsableLineCoverage(visualFrames);
  const checkJunctionVisual = hasUsableJunctionCoverage(visualFrames);
  const certified: StructuralCornerCandidate[] = [];

  for (const candidate of result.corners) {
    if (candidate.confidence < MIN_VISIBLE_CORNER_CONFIDENCE) continue;
    if (rejected.has(candidate.id)) continue;
    if (rejectedPositions.some((position) => pointDistance(position, candidate.position) <= REJECTED_CORNER_RADIUS_METERS)) continue;

    let validatedCandidate: StructuralCornerCandidate | null = null;
    if (candidate.id.startsWith('endpoint:')) {
      const observation = endpointObservations
        .filter((endpoint) => endpoint.freeBeyond)
        .sort((left, right) => pointDistance(left.position, candidate.position) - pointDistance(right.position, candidate.position))[0];
      if (
        observation === undefined ||
        pointDistance(observation.position, candidate.position) > MAX_CERTIFICATE_SHIFT_METERS ||
        observation.confidence < MIN_CERTIFIED_CORNER_CONFIDENCE
      ) continue;
      validatedCandidate = {
        ...candidate,
        position: observation.position,
        confidence: Math.min(candidate.confidence, observation.confidence),
        support: Math.max(candidate.support, observation.support),
      };
      if (checkLineVisual) {
        const visual = certifyEndpointByVisualReprojection(validatedCandidate.position, visualFrames);
        const corroborated = applyStructuralVisualCorroboration(
          validatedCandidate.confidence,
          validatedCandidate.observations,
          visual,
        );
        validatedCandidate = {
          ...validatedCandidate,
          confidence: corroborated.confidence,
          observations: corroborated.observations,
        };
      }
    } else {
      const certificate = solveManualPlacementOnStableSurface(candidate.position, snapshot);
      if (
        !certificate.accepted ||
        certificate.mode !== 'corner' ||
        certificate.position === null ||
        certificate.confidence < MIN_CERTIFIED_CORNER_CONFIDENCE ||
        (certificate.uncertaintyMeters ?? Number.POSITIVE_INFINITY) > MAX_CERTIFIED_UNCERTAINTY_METERS ||
        pointDistance(candidate.position, certificate.position) > MAX_CERTIFICATE_SHIFT_METERS
      ) continue;
      validatedCandidate = {
        ...candidate,
        position: certificate.position,
        confidence: Math.min(candidate.confidence, certificate.confidence),
        support: Math.max(candidate.support, certificate.supportCount),
      };
      if (checkJunctionVisual) {
        const visual = certifyCornerByVisualReprojection(validatedCandidate.position, visualFrames);
        const corroborated = applyStructuralVisualCorroboration(
          validatedCandidate.confidence,
          validatedCandidate.observations,
          visual,
        );
        validatedCandidate = {
          ...validatedCandidate,
          confidence: corroborated.confidence,
          observations: corroborated.observations,
        };
      }
    }
    certified.push(validatedCandidate);
  }
  return certified.sort((left, right) => right.confidence - left.confidence).slice(0, 8);
}

function mergeCornerCandidates(
  primary: readonly StructuralCornerCandidate[],
  inferred: readonly StructuralCornerCandidate[],
): readonly StructuralCornerCandidate[] {
  const merged = [...primary];
  for (const candidate of inferred) {
    const duplicate = merged.find((current) => pointDistance(current.position, candidate.position) <= 0.075);
    if (duplicate === undefined) merged.push(candidate);
    else if (candidate.confidence > duplicate.confidence) merged[merged.indexOf(duplicate)] = candidate;
  }
  return merged.sort((left, right) => right.confidence - left.confidence).slice(0, 8);
}

export function StructuralGuideOverlay({ runtime, runtimeState, interactionEnabled }: Props) {
  const { locale } = useLocale();
  const t = useT();
  const client = useMemo(() => new StructuralGuideClient(), []);
  const [result, setResult] = useState<StructuralGuideResult | null>(null);
  const [validationSnapshot, setValidationSnapshot] = useState<StructuralGuideSnapshot | null>(null);
  const [surfaceReadiness, setSurfaceReadiness] = useState<SurfaceComponentReadinessSummary | null>(null);
  const [viewHistoryRevision, setViewHistoryRevision] = useState(0);
  const [rejected, setRejected] = useState<Set<string>>(() => new Set());
  const [rejectedPositions, setRejectedPositions] = useState<ProjectPoint[]>([]);
  const [acceptedEdges, setAcceptedEdges] = useState<Set<string>>(() => new Set());
  const lastAnalysisMs = useRef(Number.NEGATIVE_INFINITY);
  const lastAnalysisKeyframe = useRef(0);
  const viewpointHistory = useRef(new ScanViewpointHistory());

  useEffect(() => () => {
    client.dispose();
    deactivateCertifiedGuideRegistry();
  }, [client]);

  useEffect(() => {
    if (runtimeState.phase === 'starting') {
      client.reset();
      deactivateCertifiedGuideRegistry();
      viewpointHistory.current.reset();
      setViewHistoryRevision((value) => value + 1);
      setResult(null);
      setValidationSnapshot(null);
      setSurfaceReadiness(null);
      setRejected(new Set());
      setRejectedPositions([]);
      setAcceptedEdges(new Set());
      lastAnalysisMs.current = Number.NEGATIVE_INFINITY;
      lastAnalysisKeyframe.current = 0;
    } else if (runtimeState.phase !== 'running') {
      client.park();
      deactivateCertifiedGuideRegistry();
    }
  }, [client, runtimeState.phase]);

  const recordCurrentKeyframeViewpoint = useEffectEvent(() => {
    if (runtimeState.phase !== 'running') return;
    if (viewpointHistory.current.record(structuralGuideViewpointFromState(runtimeState))) {
      setViewHistoryRevision((value) => value + 1);
    }
  });

  useEffect(() => {
    // Only a physical keyframe/phase transition should add a viewpoint. The
    // Effect Event still reads the latest committed pose and motion snapshot,
    // so exhaustive-deps stays honest without sampling at XR render frequency.
    recordCurrentKeyframeViewpoint();
  }, [runtimeState.keyframeCount, runtimeState.phase]);

  const readiness = useMemo(() => computeScanCapabilityReadiness({
    stablePointCount: runtimeState.pointCount,
    keyframeCount: runtimeState.keyframeCount,
    visibleCoverageCells: runtimeState.coverageVisibleCells,
    readyCoverageCells: runtimeState.coverageReadyCells,
    surfaceAnalysis: surfaceReadiness,
    viewpoints: viewpointHistory.current.snapshot(),
  }), [
    runtimeState.pointCount,
    runtimeState.keyframeCount,
    runtimeState.coverageVisibleCells,
    runtimeState.coverageReadyCells,
    surfaceReadiness,
    viewHistoryRevision,
  ]);

  useEffect(() => {
    if (
      runtimeState.phase !== 'running' ||
      runtimeState.pointCount < MIN_STABLE_POINTS ||
      runtimeState.keyframeCount < MIN_KEYFRAMES ||
      !shouldAnalyzeMotion(runtimeState)
    ) return;

    const now = performance.now();
    const elapsed = now - lastAnalysisMs.current;
    const keyframeAdvance = runtimeState.keyframeCount - lastAnalysisKeyframe.current;
    if (elapsed < analysisInterval(runtimeState) || keyframeAdvance < MIN_KEYFRAME_ADVANCE || client.busy) return;

    const viewpoint = structuralGuideViewpointFromState(runtimeState);
    if (viewpoint === null) return;

    // Cheap UI-side prerequisites only. Deliberately omit the previous surface
    // summary here: using a stale "object too small" result as an activation gate
    // would deadlock growth because the worker would never get a chance to remeasure
    // the newly scanned component. Component/PCA scale is recomputed in the worker.
    const preflight = computeScanCapabilityReadiness({
      stablePointCount: runtimeState.pointCount,
      keyframeCount: runtimeState.keyframeCount,
      visibleCoverageCells: runtimeState.coverageVisibleCells,
      readyCoverageCells: runtimeState.coverageReadyCells,
      surfaceAnalysis: null,
      viewpoints: viewpointHistory.current.snapshot(),
    });
    if (!preflight.structural.ready) return;

    lastAnalysisMs.current = now;
    lastAnalysisKeyframe.current = runtimeState.keyframeCount;
    const snapshotStarted = performance.now();
    // This is deliberately the raw one-shot worker handoff. Structural-family
    // clustering/PCA/local certification now run after transfer off the XR/UI
    // thread. The certified getter remains reserved for fresh corner validation.
    const snapshot = runtime.getStructuralWorkerSnapshot();
    const snapshotMs = performance.now() - snapshotStarted;
    if (snapshot.positions.length < MIN_RAW_STRUCTURAL_POINTS * 3 || snapshot.normals.length !== snapshot.positions.length) {
      setResult(null);
      setValidationSnapshot(null);
      return;
    }

    const workerStarted = performance.now();
    void client.analyze({
      positions: snapshot.positions,
      normals: snapshot.normals,
      confidence: snapshot.confidence,
      keyframeCount: snapshot.keyframeCount,
      voxelSizeMeters: snapshot.voxelSizeMeters,
      viewpoint,
      freeSpacePositions: snapshot.freeSpacePositions,
      freeSpaceVoxelSizeMeters: snapshot.freeSpaceVoxelSizeMeters,
    }).then((next) => {
      const workerResult = next as StructuralGuideResultWithReadiness;
      const nextSurfaceReadiness = workerResult.surfaceReadiness ?? null;
      setSurfaceReadiness(nextSurfaceReadiness);
      const activation = computeScanCapabilityReadiness({
        stablePointCount: runtimeState.pointCount,
        keyframeCount: runtimeState.keyframeCount,
        visibleCoverageCells: runtimeState.coverageVisibleCells,
        readyCoverageCells: runtimeState.coverageReadyCells,
        surfaceAnalysis: nextSurfaceReadiness,
        viewpoints: viewpointHistory.current.snapshot(),
      });

      if (!activation.structural.ready) {
        setResult(null);
        setValidationSnapshot(null);
        clientDiagnostics.info('XR_GUIDE_DEFERRED', {
          reason: activation.structural.limitingMetric,
          readiness: Number(activation.structural.progress.toFixed(3)),
          stablePoints: runtimeState.pointCount,
          clusteredPoints: next.analyzedPoints,
          majorExtentMeters: activation.objectMajorExtentMeters === null ? null : Number(activation.objectMajorExtentMeters.toFixed(3)),
          minorExtentMeters: activation.objectMinorExtentMeters === null ? null : Number(activation.objectMinorExtentMeters.toFixed(3)),
          cameraBaselineMeters: Number(activation.cameraBaselineMeters.toFixed(3)),
        });
        void clientDiagnostics.flush();
        return;
      }

      // Edge candidates already contain worker-side finite physical support and do
      // not need another O(N) snapshot. Corner/endpoint suggestions deliberately
      // get a fresh independent physical certificate before they can reach UI.
      let certificateSnapshotMs = 0;
      if (next.corners.length > 0) {
        const certificateStarted = performance.now();
        setValidationSnapshot(runtime.getStructuralGuideSnapshot());
        certificateSnapshotMs = performance.now() - certificateStarted;
      } else {
        setValidationSnapshot(null);
      }
      setResult(next);
      clientDiagnostics.info('XR_GUIDE_ANALYSIS', {
        keyframes: next.keyframeCount,
        points: next.analyzedPoints,
        patches: next.planarPatches,
        corners: next.corners.length,
        edges: next.edges.length,
        freeSpaceRejected: next.rejectedByFreeSpace,
        candidateComparisons: next.candidateComparisons,
        visualFrames: runtimeState.visualEvidenceFrames,
        snapshotMs: Number(snapshotMs.toFixed(1)),
        certificateSnapshotMs: Number(certificateSnapshotMs.toFixed(1)),
        workerRoundTripMs: Number((performance.now() - workerStarted).toFixed(1)),
        workerComputeMs: Number(next.computeMs.toFixed(1)),
      });
      if (snapshotMs >= 80) clientDiagnostics.warn('XR_GUIDE_SNAPSHOT_SLOW', { snapshotMs: Number(snapshotMs.toFixed(1)) });
      if (certificateSnapshotMs >= 80) clientDiagnostics.warn('XR_GUIDE_CERTIFICATE_SNAPSHOT_SLOW', { snapshotMs: Number(certificateSnapshotMs.toFixed(1)) });
      void clientDiagnostics.flush();
    }).catch((error) => {
      const message = error instanceof Error ? error.message : String(error);
      if (!message.includes('reset for a new scan')) clientDiagnostics.warn('XR_GUIDE_ANALYSIS_FAILED', error);
    });
  }, [client, runtime, runtimeState]);

  const visualFrames = useMemo(
    () => getVisualEvidenceFrames(),
    [result, runtimeState.visualEvidenceFrames],
  );

  const physicallyCertifiedCandidates = useMemo(
    () => certifyCandidates(result, validationSnapshot, visualFrames, rejected, rejectedPositions),
    [result, validationSnapshot, visualFrames, rejected, rejectedPositions],
  );

  const certifiedEdgeCandidates = useMemo(
    () => certifyOperationalStructuralEdges(result?.edges ?? [], visualFrames),
    [result, visualFrames],
  );

  const inferredEdgeVertices = useMemo(
    () => inferStructuralVerticesFromEdges(certifiedEdgeCandidates).filter((candidate) =>
      !rejected.has(candidate.id) &&
      !rejectedPositions.some((position) => pointDistance(position, candidate.position) <= REJECTED_CORNER_RADIUS_METERS)
    ),
    [certifiedEdgeCandidates, rejected, rejectedPositions],
  );

  const certifiedCandidates = useMemo(
    () => mergeCornerCandidates(physicallyCertifiedCandidates, inferredEdgeVertices),
    [physicallyCertifiedCandidates, inferredEdgeVertices],
  );

  useEffect(() => {
    if (runtimeState.phase !== 'running') return;
    setCertifiedGuideCorners(certifiedCandidates.map((candidate) => ({
      id: candidate.id,
      position: candidate.position,
      confidence: candidate.confidence,
      support: candidate.support,
      source: cornerSource(candidate),
    })));
    setCertifiedGuideEdges(certifiedEdgeCandidates.map((candidate) => ({
      id: candidate.id,
      start: candidate.start,
      end: candidate.end,
      confidence: candidate.confidence,
      support: candidate.support,
      observations: candidate.observations,
      source: 'robust-two-plane',
    })));

    clientDiagnostics.info('XR_GUIDE_CERTIFICATION', {
      workerEdges: result?.edges.length ?? 0,
      certifiedEdges: certifiedEdgeCandidates.length,
      workerCorners: result?.corners.length ?? 0,
      physicalCorners: physicallyCertifiedCandidates.length,
      inferredEdgeVertices: inferredEdgeVertices.length,
      certifiedCorners: certifiedCandidates.length,
      visualFrames: visualFrames.length,
    });
    void clientDiagnostics.flush();
  }, [
    certifiedCandidates,
    certifiedEdgeCandidates,
    inferredEdgeVertices.length,
    physicallyCertifiedCandidates.length,
    result,
    runtimeState.phase,
    visualFrames.length,
  ]);

  if (runtimeState.phase !== 'running') return null;

  const projectedCorners: ProjectedCorner[] = [];
  if (result !== null && validationSnapshot !== null) {
    for (const candidate of certifiedCandidates) {
      const projection = runtime.projectProjectPointToNormalized(candidate.position);
      if (!projection?.visible || !insideWorkingView(projection.u, projection.v)) continue;
      const centerDistance = Math.hypot(projection.u - 0.5, projection.v - 0.5);
      projectedCorners.push({
        candidate,
        u: projection.u,
        v: projection.v,
        score: candidate.confidence - centerDistance * 0.24,
      });
    }
    projectedCorners.sort((left, right) => right.score - left.score);
    projectedCorners.splice(MAX_VISIBLE_CORNERS);
  }

  const projectedEdges: ProjectedEdge[] = [];
  if (result !== null) {
    for (const candidate of certifiedEdgeCandidates) {
      if (acceptedEdges.has(candidate.id)) continue;
      const start = runtime.projectProjectPointToNormalized(candidate.start);
      const end = runtime.projectProjectPointToNormalized(candidate.end);
      if (!start?.visible || !end?.visible) continue;
      if (!insideWorkingView(start.u, start.v) || !insideWorkingView(end.u, end.v)) continue;
      projectedEdges.push({
        candidate,
        startU: start.u,
        startV: start.v,
        endU: end.u,
        endV: end.v,
      });
    }
  }

  const confirmCorner = (candidate: StructuralCornerCandidate): void => {
    try {
      authorizeCertifiedManualPlacement(candidate.position, candidate.confidence);
      const stored = runtime.addSuggestedManualPoint(candidate.position);
      if (stored === null) return;
      setRejected((current) => new Set(current).add(candidate.id));
      clientDiagnostics.info('XR_GUIDE_CORNER_CONFIRMED', {
        candidate: candidate.id,
        source: cornerSource(candidate),
        confidence: Number(candidate.confidence.toFixed(3)),
        manualPointId: stored.point.id,
        snapped: stored.snapped,
      });
      void clientDiagnostics.flush();
    } catch (error) {
      setRejected((current) => new Set(current).add(candidate.id));
      clientDiagnostics.warn('XR_GUIDE_CORNER_CONFIRM_REJECTED', {
        candidate: candidate.id,
        error: error instanceof Error ? error.message : String(error),
      });
      void clientDiagnostics.flush();
    }
  };

  const rejectCorner = (candidate: StructuralCornerCandidate): void => {
    setRejected((current) => new Set(current).add(candidate.id));
    setRejectedPositions((current) => [...current, candidate.position]);
    clientDiagnostics.info('XR_GUIDE_CORNER_REJECTED', { candidate: candidate.id });
  };

  const confirmEdge = (edge: ProjectedEdge): void => {
    const stored = runtime.addOperatorEdge(edge.candidate.start, edge.candidate.end, edge.candidate.confidence);
    if (stored === null) return;
    setAcceptedEdges((current) => new Set(current).add(edge.candidate.id));
    clientDiagnostics.info('XR_GUIDE_EDGE_CONFIRMED', {
      candidate: edge.candidate.id,
      edgeId: stored.id,
      confidence: Number(edge.candidate.confidence.toFixed(3)),
      observations: edge.candidate.observations,
    });
    void clientDiagnostics.flush();
  };

  const rgbActive = result !== null && visualCheckActive(visualFrames);
  const readinessLabel = compactScanReadinessLabel(readiness, {
    visibleCorners: projectedCorners.length,
    certifiedCorners: certifiedCandidates.length,
    certifiedEdges: certifiedEdgeCandidates.length,
    planarPatches: result?.planarPatches ?? 0,
    workerEdges: result?.edges.length ?? 0,
    rgbCorroboration: rgbActive,
  }, locale);
  const metric = (value: string): string => t(({
    points: 'xr.readiness.metric.points',
    views: 'xr.readiness.metric.views',
    extent: 'xr.readiness.metric.extent',
    'surface-density': 'xr.readiness.metric.surface_density',
    faces: 'xr.readiness.metric.faces',
    unknown: 'xr.readiness.metric.unknown',
  } as Record<string, 'xr.readiness.metric.points' | 'xr.readiness.metric.views' | 'xr.readiness.metric.extent' | 'xr.readiness.metric.surface_density' | 'xr.readiness.metric.faces' | 'xr.readiness.metric.unknown'>)[value] ?? 'xr.readiness.metric.unknown');
  const readinessTitle = [
    t('xr.guide.structural_readiness', { progress: percent(readiness.structural.progress, locale), metric: metric(readiness.structural.limitingMetric) }),
    readiness.mesh === null ? t('xr.guide.mesh_not_measured') : t('xr.guide.mesh_readiness', { progress: percent(readiness.mesh.progress, locale), metric: metric(readiness.mesh.limitingMetric) }),
    t('xr.guide.view_coverage', { progress: percent(readiness.viewCoverage, locale), sectors: formatNumber(locale, readiness.viewSectors), total: formatNumber(locale, readiness.totalViewSectors) }),
    t('xr.guide.camera_baseline', { distance: formatDistance(locale, readiness.cameraBaselineMeters) }),
    readiness.objectMajorExtentMeters === null || readiness.objectMinorExtentMeters === null
      ? t('xr.guide.extent_not_measured')
      : t('xr.guide.measured_component', { major: formatDistance(locale, readiness.objectMajorExtentMeters), minor: formatDistance(locale, readiness.objectMinorExtentMeters) }),
  ].join(' · ');

  return (
    <div className="xr-structural-guide" aria-label={t('xr.guide.aria_label')}>
      <div className="xr-structural-guide-status" title={readinessTitle}>
        {readinessLabel}
      </div>

      {projectedEdges.map((edge) => {
        const x1 = edge.startU * 100;
        const y1 = edge.startV * 100;
        const x2 = edge.endU * 100;
        const y2 = edge.endV * 100;
        const midX = (x1 + x2) * 0.5;
        const midY = (y1 + y2) * 0.5;
        return (
          <div key={edge.candidate.id} className="xr-edge-suggestion" aria-hidden={!interactionEnabled}>
            <svg className="xr-edge-svg" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
              <line x1={x1} y1={y1} x2={x2} y2={y2} />
            </svg>
            {interactionEnabled && (
              <button type="button" className="xr-edge-confirm" style={{ left: `${midX}%`, top: `${midY}%` }} onClick={() => confirmEdge(edge)}>
                {t('xr.guide.line_suggestion', { confidence: formatPercent(locale, edge.candidate.confidence) })}
              </button>
            )}
          </div>
        );
      })}

      {projectedCorners.map(({ candidate, u, v }) => (
        <div key={candidate.id} className="xr-corner-suggestion" style={{ left: `${u * 100}%`, top: `${v * 100}%` }}>
          <button
            type="button"
            className="xr-corner-question"
            disabled={!interactionEnabled}
            onClick={() => confirmCorner(candidate)}
            title={candidate.id.startsWith('endpoint:')
              ? t('xr.guide.confirm_endpoint')
              : candidate.id.startsWith('edge-vertex:')
                ? t('xr.guide.confirm_vertex')
                : t('xr.guide.confirm_corner')}
          >
            ?
          </button>
          <span>{formatPercent(locale, candidate.confidence)}</span>
          {interactionEnabled && <button type="button" className="xr-corner-reject" onClick={() => rejectCorner(candidate)} aria-label={t('xr.guide.reject_corner')}>×</button>}
        </div>
      ))}
    </div>
  );
}
