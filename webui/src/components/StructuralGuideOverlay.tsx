import { useEffect, useMemo, useRef, useState } from 'react';

import { clientDiagnostics } from '../data/clientDiagnostics';
import {
  matchEdgeToManualPoints,
  type StructuralCornerCandidate,
  type StructuralEdgeCandidate,
  type StructuralGuideResult,
} from '../xr/structuralGuide';
import { StructuralGuideClient } from '../xr/structuralGuideClient';
import type { WebXrRuntime, WebXrRuntimeState } from '../xr/webxrRuntime';

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

function analysisInterval(state: WebXrRuntimeState): number {
  if (state.performance.pressure === 'high') return 4200;
  if (state.performance.pressure === 'medium') return 2800;
  return BASE_ANALYSIS_INTERVAL_MS;
}

function shouldAnalyzeMotion(state: WebXrRuntimeState): boolean {
  return state.motionState !== 'too-fast' && state.motionState !== 'paused';
}

export function StructuralGuideOverlay({ runtime, runtimeState, interactionEnabled }: Props) {
  const client = useMemo(() => new StructuralGuideClient(), []);
  const [result, setResult] = useState<StructuralGuideResult | null>(null);
  const [rejected, setRejected] = useState<Set<string>>(() => new Set());
  const [acceptedEdges, setAcceptedEdges] = useState<Set<string>>(() => new Set());
  const lastAnalysisMs = useRef(Number.NEGATIVE_INFINITY);
  const lastAnalysisKeyframe = useRef(0);

  useEffect(() => () => client.dispose(), [client]);

  useEffect(() => {
    if (runtimeState.phase === 'starting') {
      client.reset();
      setResult(null);
      setRejected(new Set());
      setAcceptedEdges(new Set());
      lastAnalysisMs.current = Number.NEGATIVE_INFINITY;
      lastAnalysisKeyframe.current = 0;
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

    lastAnalysisMs.current = now;
    lastAnalysisKeyframe.current = runtimeState.keyframeCount;
    const snapshotStarted = performance.now();
    const snapshot = runtime.getPointCloudSnapshot();
    const snapshotMs = performance.now() - snapshotStarted;
    if (snapshot.positions.length < MIN_STABLE_POINTS * 3 || snapshot.normals.length !== snapshot.positions.length) return;

    const workerStarted = performance.now();
    void client.analyze({
      positions: snapshot.positions,
      normals: snapshot.normals,
      confidence: snapshot.confidence,
      keyframeCount: snapshot.integrationKeyframes,
      voxelSizeMeters: snapshot.voxelSizeMeters,
    }).then((next) => {
      setResult(next);
      clientDiagnostics.info('XR_GUIDE_ANALYSIS', {
        keyframes: next.keyframeCount,
        points: next.analyzedPoints,
        patches: next.planarPatches,
        corners: next.corners.length,
        edges: next.edges.length,
        snapshotMs: Number(snapshotMs.toFixed(1)),
        workerRoundTripMs: Number((performance.now() - workerStarted).toFixed(1)),
        workerComputeMs: Number(next.computeMs.toFixed(1)),
      });
      if (snapshotMs >= 80) clientDiagnostics.warn('XR_GUIDE_SNAPSHOT_SLOW', { snapshotMs: Number(snapshotMs.toFixed(1)) });
      void clientDiagnostics.flush();
    }).catch((error) => {
      clientDiagnostics.warn('XR_GUIDE_ANALYSIS_FAILED', error);
    });
  }, [client, runtime, runtimeState]);

  if (runtimeState.phase !== 'running' || result === null) return null;

  const projectedCorners: ProjectedCorner[] = [];
  for (const candidate of result.corners) {
    if (rejected.has(candidate.id)) continue;
    const projection = runtime.projectProjectPointToNormalized(candidate.position);
    if (projection?.visible) projectedCorners.push({ candidate, u: projection.u, v: projection.v });
  }

  const manualPoints = runtime.getManualControlPoints();
  const projectedEdges: ProjectedEdge[] = [];
  for (const candidate of result.edges) {
    if (acceptedEdges.has(candidate.id)) continue;
    const match = matchEdgeToManualPoints(candidate, manualPoints);
    if (match === null) continue;
    const start = runtime.projectProjectPointToNormalized(match[0].position);
    const end = runtime.projectProjectPointToNormalized(match[1].position);
    if (!start?.visible || !end?.visible) continue;
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
    const stored = runtime.addSuggestedManualPoint(candidate.position);
    if (stored === null) return;
    setRejected((current) => new Set(current).add(candidate.id));
    clientDiagnostics.info('XR_GUIDE_CORNER_CONFIRMED', {
      candidate: candidate.id,
      confidence: Number(candidate.confidence.toFixed(3)),
      manualPointId: stored.point.id,
      snapped: stored.snapped,
    });
    void clientDiagnostics.flush();
  };

  const rejectCorner = (candidate: StructuralCornerCandidate): void => {
    setRejected((current) => new Set(current).add(candidate.id));
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
        AUTO GUIDE · {result.corners.length} corner · {result.edges.length} edge · {result.planarPatches} planes
      </div>

      {projectedEdges.map((edge) => {
        const x1=edge.startU*100, y1=edge.startV*100, x2=edge.endU*100, y2=edge.endV*100;
        const midX=(x1+x2)*0.5, midY=(y1+y2)*0.5;
        return (
          <div key={edge.candidate.id} className="xr-edge-suggestion" aria-hidden={!interactionEnabled}>
            <svg className="xr-edge-svg" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
              <line x1={x1} y1={y1} x2={x2} y2={y2} />
            </svg>
            {interactionEnabled && (
              <button type="button" className="xr-edge-confirm" style={{left:`${midX}%`,top:`${midY}%`}} onClick={() => confirmEdge(edge)}>
                LINE? {Math.round(edge.candidate.confidence*100)}%
              </button>
            )}
          </div>
        );
      })}

      {projectedCorners.map(({candidate,u,v}) => (
        <div key={candidate.id} className="xr-corner-suggestion" style={{left:`${u*100}%`,top:`${v*100}%`}}>
          <button type="button" className="xr-corner-question" disabled={!interactionEnabled} onClick={() => confirmCorner(candidate)} title="Confirm suggested 3D corner">
            ?
          </button>
          <span>{Math.round(candidate.confidence*100)}%</span>
          {interactionEnabled && <button type="button" className="xr-corner-reject" onClick={() => rejectCorner(candidate)} aria-label="Reject suggested corner">×</button>}
        </div>
      ))}
    </div>
  );
}
