import { useEffect, useMemo, useRef, useState } from 'react';

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
  certifyEdgeByVisualReprojection,
  certifyEndpointByVisualReprojection,
} from '../xr/reprojectionCertificate';
import { detectEdgeEndpointObservations } from '../xr/structuralEdgeEndpoints';
import {
  matchEdgeToManualPoints,
  type StructuralCornerCandidate,
  type StructuralEdgeCandidate,
  type StructuralGuideResult,
  type StructuralGuideViewpoint,
} from '../xr/structuralGuide';
import { StructuralGuideClient } from '../xr/structuralGuideClient';
import type { VisualEvidenceFrame } from '../xr/visualLineEvidence';
import type { WebXrRuntime, WebXrRuntimeState } from '../xr/webxrRuntime';
import { getVisualEvidenceFrames } from '../xr/xrGuidanceRegistry';

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
  pointAId: number;
  pointBId: number;
}

const MIN_STABLE_POINTS = 180;
const MIN_KEYFRAMES = 8;
const MIN_KEYFRAME_ADVANCE = 4;
const BASE_ANALYSIS_INTERVAL_MS = 1900;
const MIN_VISIBLE_CORNER_CONFIDENCE = 0.70;
const MIN_CERTIFIED_CORNER_CONFIDENCE = 0.70;
const MIN_CERTIFIED_EDGE_CONFIDENCE = 0.66;
const MIN_CERTIFIED_EDGE_OBSERVATIONS = 2;
const MAX_CERTIFICATE_SHIFT_METERS = 0.055;
const MAX_CERTIFIED_UNCERTAINTY_METERS = 0.032;
const VISUAL_CERTIFICATE_MIN_FRAMES = 3;
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

function needsVisualCertificate(frames: readonly VisualEvidenceFrame[]): boolean {
  return frames.length >= VISUAL_CERTIFICATE_MIN_FRAMES;
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
  const requireVisual = needsVisualCertificate(visualFrames);
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
        observation.confidence < 0.68
      ) continue;
      validatedCandidate = {
        ...candidate,
        position: observation.position,
        confidence: Math.min(candidate.confidence, observation.confidence),
        support: Math.max(candidate.support, observation.support),
      };
      if (requireVisual) {
        const visual = certifyEndpointByVisualReprojection(validatedCandidate.position, visualFrames);
        if (!visual.accepted) continue;
        validatedCandidate = {
          ...validatedCandidate,
          confidence: Math.min(validatedCandidate.confidence, 0.74 + visual.confidence * 0.26),
          observations: Math.max(validatedCandidate.observations, visual.observations),
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
      if (requireVisual) {
        const visual = certifyCornerByVisualReprojection(validatedCandidate.position, visualFrames);
        if (!visual.accepted) continue;
        validatedCandidate = {
          ...validatedCandidate,
          confidence: Math.min(validatedCandidate.confidence, 0.74 + visual.confidence * 0.26),
          observations: Math.max(validatedCandidate.observations, visual.observations),
        };
      }
    }
    certified.push(validatedCandidate);
  }
  return certified.sort((left, right) => right.confidence - left.confidence).slice(0, 8);
}

function certifyEdges(
  result: StructuralGuideResult | null,
  visualFrames: readonly VisualEvidenceFrame[],
): readonly StructuralEdgeCandidate[] {
  if (result === null) return [];
  const requireVisual = needsVisualCertificate(visualFrames);
  const output: StructuralEdgeCandidate[] = [];
  for (const edge of result.edges) {
    if (edge.confidence < MIN_CERTIFIED_EDGE_CONFIDENCE) continue;
    if (edge.observations < MIN_CERTIFIED_EDGE_OBSERVATIONS) continue;
    const length = pointDistance(edge.start, edge.end);
    if (!Number.isFinite(length) || length < 0.065) continue;
    if (requireVisual) {
      const visual = certifyEdgeByVisualReprojection(edge.start, edge.end, visualFrames);
      if (!visual.accepted) continue;
      output.push({
        ...edge,
        confidence: Math.min(edge.confidence, 0.74 + visual.confidence * 0.26),
        observations: Math.max(edge.observations, visual.observations),
      });
    } else {
      output.push(edge);
    }
  }
  return output.sort((left, right) => right.confidence - left.confidence).slice(0, 10);
}

export function StructuralGuideOverlay({ runtime, runtimeState, interactionEnabled }: Props) {
  const client = useMemo(() => new StructuralGuideClient(), []);
  const [result, setResult] = useState<StructuralGuideResult | null>(null);
  const [validationSnapshot, setValidationSnapshot] = useState<StructuralGuideSnapshot | null>(null);
  const [rejected, setRejected] = useState<Set<string>>(() => new Set());
  const [rejectedPositions, setRejectedPositions] = useState<ProjectPoint[]>([]);
  const [acceptedEdges, setAcceptedEdges] = useState<Set<string>>(() => new Set());
  const lastAnalysisMs = useRef(Number.NEGATIVE_INFINITY);
  const lastAnalysisKeyframe = useRef(0);

  useEffect(() => () => {
    client.dispose();
    deactivateCertifiedGuideRegistry();
  }, [client]);

  useEffect(() => {
    if (runtimeState.phase === 'starting') {
      client.reset();
      deactivateCertifiedGuideRegistry();
      setResult(null);
      setValidationSnapshot(null);
      setRejected(new Set());
      setRejectedPositions([]);
      setAcceptedEdges(new Set());
      lastAnalysisMs.current = Number.NEGATIVE_INFINITY;
      lastAnalysisKeyframe.current = 0;
    } else if (runtimeState.phase !== 'running') {
      deactivateCertifiedGuideRegistry();
    }
  }, [client, runtimeState.phase]);

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

    lastAnalysisMs.current = now;
    lastAnalysisKeyframe.current = runtimeState.keyframeCount;
    const snapshotStarted = performance.now();
    const snapshot = runtime.getStructuralGuideSnapshot();
    const snapshotMs = performance.now() - snapshotStarted;
    if (snapshot.positions.length < MIN_STABLE_POINTS * 3 || snapshot.normals.length !== snapshot.positions.length) return;

    const workerStarted = performance.now();
    void client.analyze({
      positions: snapshot.positions,
      normals: snapshot.normals,
      confidence: snapshot.confidence,
      residualMeters: snapshot.residualMeters,
      normalConfidence: snapshot.normalConfidence,
      coverageConfidence: snapshot.coverageConfidence,
      coverageReady: snapshot.coverageReady,
      keyframeCount: snapshot.keyframeCount,
      voxelSizeMeters: snapshot.voxelSizeMeters,
      viewpoint,
      freeSpacePositions: snapshot.freeSpacePositions,
      freeSpaceVoxelSizeMeters: snapshot.freeSpaceVoxelSizeMeters,
    }).then((next) => {
      const certificateSnapshot = runtime.getStructuralGuideSnapshot();
      setValidationSnapshot(certificateSnapshot);
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
        workerRoundTripMs: Number((performance.now() - workerStarted).toFixed(1)),
        workerComputeMs: Number(next.computeMs.toFixed(1)),
      });
      if (snapshotMs >= 80) clientDiagnostics.warn('XR_GUIDE_SNAPSHOT_SLOW', { snapshotMs: Number(snapshotMs.toFixed(1)) });
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

  const certifiedCandidates = useMemo(
    () => certifyCandidates(result, validationSnapshot, visualFrames, rejected, rejectedPositions),
    [result, validationSnapshot, visualFrames, rejected, rejectedPositions],
  );

  const certifiedEdgeCandidates = useMemo(
    () => certifyEdges(result, visualFrames),
    [result, visualFrames],
  );

  useEffect(() => {
    if (runtimeState.phase !== 'running') return;
    setCertifiedGuideCorners(certifiedCandidates.map((candidate) => ({
      id: candidate.id,
      position: candidate.position,
      confidence: candidate.confidence,
      support: candidate.support,
      source: candidate.id.startsWith('endpoint:') ? 'multi-view-edge-endpoint' : 'three-plane',
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
  }, [certifiedCandidates, certifiedEdgeCandidates, runtimeState.phase]);

  if (runtimeState.phase !== 'running' || result === null || validationSnapshot === null) return null;

  const projectedCorners: ProjectedCorner[] = [];
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

  const manualPoints = runtime.getManualControlPoints();
  const projectedEdges: ProjectedEdge[] = [];
  for (const candidate of certifiedEdgeCandidates) {
    if (acceptedEdges.has(candidate.id)) continue;
    const match = matchEdgeToManualPoints(candidate, manualPoints);
    if (match === null) continue;
    const start = runtime.projectProjectPointToNormalized(match[0].position);
    const end = runtime.projectProjectPointToNormalized(match[1].position);
    if (!start?.visible || !end?.visible) continue;
    if (!insideWorkingView(start.u, start.v) || !insideWorkingView(end.u, end.v)) continue;
    projectedEdges.push({
      candidate,
      startU: start.u,
      startV: start.v,
      endU: end.u,
      endV: end.v,
      pointAId: match[0].id,
      pointBId: match[1].id,
    });
  }

  const confirmCorner = (candidate: StructuralCornerCandidate): void => {
    try {
      if (candidate.id.startsWith('endpoint:')) {
        authorizeCertifiedManualPlacement(candidate.position, candidate.confidence);
      }
      const stored = runtime.addSuggestedManualPoint(candidate.position);
      if (stored === null) return;
      setRejected((current) => new Set(current).add(candidate.id));
      clientDiagnostics.info('XR_GUIDE_CORNER_CONFIRMED', {
        candidate: candidate.id,
        source: candidate.id.startsWith('endpoint:') ? 'multi-view-edge-endpoint' : 'three-plane',
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
    const pointA = manualPoints.find((point) => point.id === edge.pointAId);
    const pointB = manualPoints.find((point) => point.id === edge.pointBId);
    if (pointA === undefined || pointB === undefined) return;
    const stored = runtime.addOperatorEdge(pointA.position, pointB.position, edge.candidate.confidence);
    if (stored === null) return;
    setAcceptedEdges((current) => new Set(current).add(edge.candidate.id));
    clientDiagnostics.info('XR_GUIDE_EDGE_CONFIRMED', {
      candidate: edge.candidate.id,
      edgeId: stored.id,
      a: pointA.id,
      b: pointB.id,
      confidence: Number(edge.candidate.confidence.toFixed(3)),
    });
    void clientDiagnostics.flush();
  };

  return (
    <div className="xr-structural-guide" aria-label="Automatic structural suggestions">
      <div className="xr-structural-guide-status">
        AUTO GUIDE · {projectedCorners.length}/{certifiedCandidates.length} certified corner · {certifiedEdgeCandidates.length} certified edge · {result.planarPatches} patches{needsVisualCertificate(visualFrames) ? ' · RGB CHECK' : ''}
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
                LINE? {Math.round(edge.candidate.confidence * 100)}%
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
            title={candidate.id.startsWith('endpoint:') ? 'Confirm multi-view edge endpoint' : 'Confirm robustly certified 3D corner'}
          >
            ?
          </button>
          <span>{Math.round(candidate.confidence * 100)}%</span>
          {interactionEnabled && <button type="button" className="xr-corner-reject" onClick={() => rejectCorner(candidate)} aria-label="Reject suggested corner">×</button>}
        </div>
      ))}
    </div>
  );
}
