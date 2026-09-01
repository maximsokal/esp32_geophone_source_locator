from pathlib import Path

p = Path('webui/src/xr/webxrRuntime.ts')
s = p.read_text()


def replace_once(old: str, new: str) -> None:
    global s
    count = s.count(old)
    if count != 1:
        raise SystemExit(f'Expected exactly one match, found {count}: {old[:120]!r}')
    s = s.replace(old, new, 1)

replace_once(
"""import {
  ManualControlPointStore,
  projectDepthSampleToProject,
  type ManualControlPoint,
} from './manualControlPoints';
""",
"""import {
  ManualControlPointStore,
  projectDepthSampleToProject,
  type ManualControlPoint,
  type ProjectPoint,
} from './manualControlPoints';
import { OperatorEdgeStore, type OperatorEdge } from './operatorEdges';
import {
  projectProjectPointToNormalized as projectProjectPointToScreen,
  type NormalizedProjection,
} from './projectionMath';
import {
  XrPerformanceProfiler,
  emptyXrPerformanceSnapshot,
  type XrPerformanceSnapshot,
} from './xrPerformanceProfiler';
""",
)

replace_once(
"""export interface ScanPointCloudSnapshot extends DepthScanPointCloudSnapshot {
  manualControlPoints: readonly ManualControlPoint[];
}
""",
"""export interface ScanPointCloudSnapshot extends DepthScanPointCloudSnapshot {
  manualControlPoints: readonly ManualControlPoint[];
  operatorEdges: readonly OperatorEdge[];
}
""",
)

replace_once(
"""  manualPointCount: number;
  manualPointError: string | null;
""",
"""  manualPointCount: number;
  manualEdgeCount: number;
  manualPointError: string | null;
  performance: XrPerformanceSnapshot;
""",
)

replace_once(
"""  manualPointCount: 0,
  manualPointError: null,
""",
"""  manualPointCount: 0,
  manualEdgeCount: 0,
  manualPointError: null,
  performance: emptyXrPerformanceSnapshot,
""",
)

replace_once(
"""  private readonly pointCloud = new DepthPointCloudAccumulator();
  private readonly keyframeSelector = new WebXrFusionKeyframeSelector();
  private readonly manualPoints = new ManualControlPointStore();
""",
"""  private readonly pointCloud = new DepthPointCloudAccumulator();
  private readonly keyframeSelector = new WebXrFusionKeyframeSelector();
  private readonly manualPoints = new ManualControlPointStore();
  private readonly operatorEdges = new OperatorEdgeStore();
  private readonly performanceProfiler = new XrPerformanceProfiler();
  private readonly latestProjectionMatrix = new Float32Array(16);
  private readonly latestWorldFromView = new Float32Array(16);
  private hasLatestProjection = false;
""",
)

replace_once(
"""  getPointCloudSnapshot(): ScanPointCloudSnapshot {
    return {
      ...this.pointCloud.snapshot(this.depthFrameCount, this.sampledDepthFrames),
      manualControlPoints: this.manualPoints.snapshot(),
    };
  }

  getLastManualPointId(): number | null {
    return this.manualPoints.lastId;
  }
""",
"""  getPointCloudSnapshot(): ScanPointCloudSnapshot {
    return {
      ...this.pointCloud.snapshot(this.depthFrameCount, this.sampledDepthFrames),
      manualControlPoints: this.manualPoints.snapshot(),
      operatorEdges: this.operatorEdges.snapshot(),
    };
  }

  getLastManualPointId(): number | null {
    return this.manualPoints.lastId;
  }

  getManualControlPoints(): readonly ManualControlPoint[] {
    return this.manualPoints.snapshot();
  }

  getOperatorEdges(): readonly OperatorEdge[] {
    return this.operatorEdges.snapshot();
  }

  projectProjectPointToNormalized(point: ProjectPoint): NormalizedProjection | null {
    if (!this.hasLatestProjection) return null;
    return projectProjectPointToScreen(point, this.latestProjectionMatrix, this.latestWorldFromView);
  }

  addSuggestedManualPoint(position: ProjectPoint): { point: ManualControlPoint; created: boolean; snapped: boolean } | null {
    if (this.session === null || this.state.phase !== 'running') return null;
    const stored = this.manualPoints.addOrSnap(position);
    this.lastCoverageGpuUploadMs = Number.NEGATIVE_INFINITY;
    this.publish({ manualPointCount: this.manualPoints.size, manualPointError: null });
    return stored;
  }

  addOperatorEdge(start: ProjectPoint, end: ProjectPoint, confidence = 1): OperatorEdge | null {
    if (this.session === null || this.state.phase !== 'running') return null;
    const stored = this.operatorEdges.add(start, end, confidence);
    if (stored !== null) this.publish({ manualEdgeCount: this.operatorEdges.size });
    return stored;
  }
""",
)

replace_once(
"""      keyframes: this.pointCloud.keyframeCount,
      manualPoints: this.manualPoints.size,
""",
"""      keyframes: this.pointCloud.keyframeCount,
      manualPoints: this.manualPoints.size,
      operatorEdges: this.operatorEdges.size,
""",
)

replace_once(
"""    this.pointCloud.reset();
    this.keyframeSelector.reset();
    this.manualPoints.reset();
""",
"""    this.pointCloud.reset();
    this.keyframeSelector.reset();
    this.manualPoints.reset();
    this.operatorEdges.reset();
    this.performanceProfiler.reset();
    this.hasLatestProjection = false;
""",
)

replace_once(
"""      manualPointCount: this.manualPoints.size,
      lightingEstimateAvailable: this.lightingEstimateAvailable,
""",
"""      manualPointCount: this.manualPoints.size,
      manualEdgeCount: this.operatorEdges.size,
      performance: this.performanceProfiler.snapshot(),
      lightingEstimateAvailable: this.lightingEstimateAvailable,
""",
)

replace_once(
"""      manualPoints: snapshot.manualControlPoints.length,
      residualMm: Math.round(snapshot.averageStableResidualMeters * 1000),
""",
"""      manualPoints: snapshot.manualControlPoints.length,
      operatorEdges: snapshot.operatorEdges.length,
      performance: this.performanceProfiler.snapshot(),
      residualMm: Math.round(snapshot.averageStableResidualMeters * 1000),
""",
)

replace_once(
"""  private refreshCoverageGuide(time: number, force = false): void {
    if (!force && time - this.lastCoverageGpuUploadMs < COVERAGE_GPU_UPLOAD_INTERVAL_MS) return;
    this.lastCoverageGpuUploadMs = time;
    const coverage: LiveCoverageSnapshot = this.pointCloud.liveCoverageSnapshot();
""",
"""  private refreshCoverageGuide(time: number, force = false): void {
    const adaptiveIntervalMs = Math.max(
      COVERAGE_GPU_UPLOAD_INTERVAL_MS,
      this.performanceProfiler.recommendedCoverageIntervalMs(),
    );
    if (!force && time - this.lastCoverageGpuUploadMs < adaptiveIntervalMs) return;
    this.lastCoverageGpuUploadMs = time;
    const started = performance.now();
    const coverage: LiveCoverageSnapshot = this.pointCloud.liveCoverageSnapshot();
""",
)

replace_once(
"""    } else {
      this.coverageGuideRenderPoints = 0;
    }
  }

  private updateLightEstimate""",
"""    } else {
      this.coverageGuideRenderPoints = 0;
    }
    this.performanceProfiler.record('coverage', performance.now() - started);
  }

  private updateLightEstimate""",
)

replace_once(
"""      const stored = request.pointId === null
        ? this.manualPoints.add(point)
        : this.manualPoints.update(request.pointId, point);
      if (stored === null) {
        this.publish({ manualPointError: 'The selected manual point no longer exists.' });
        return;
      }

      this.publish({ manualPointCount: this.manualPoints.size, manualPointError: null });
      clientDiagnostics.info(
        request.pointId === null ? 'XR_MANUAL_POINT_ADDED' : 'XR_MANUAL_POINT_MOVED',
        {
          id: stored.id,
          x: Number(stored.position[0].toFixed(3)),
          y: Number(stored.position[1].toFixed(3)),
          z: Number(stored.position[2].toFixed(3)),
        },
      );
""",
"""      let stored: ManualControlPoint | null;
      let snapped = false;
      if (request.pointId === null) {
        const result = this.manualPoints.addOrSnap(point);
        stored = result.point;
        snapped = result.snapped;
      } else {
        stored = this.manualPoints.update(request.pointId, point);
      }
      if (stored === null) {
        this.publish({ manualPointError: 'The selected manual point no longer exists.' });
        return;
      }

      this.publish({ manualPointCount: this.manualPoints.size, manualPointError: null });
      clientDiagnostics.info(
        request.pointId === null
          ? (snapped ? 'XR_MANUAL_POINT_SNAPPED' : 'XR_MANUAL_POINT_ADDED')
          : 'XR_MANUAL_POINT_MOVED',
        {
          id: stored.id,
          snapped,
          x: Number(stored.position[0].toFixed(3)),
          y: Number(stored.position[1].toFixed(3)),
          z: Number(stored.position[2].toFixed(3)),
        },
      );
""",
)

replace_once(
"""    const view = pose.views[0];
    try {
      const depth = frame.getDepthInformation(view);
      if (depth === null) return;
""",
"""    const view = pose.views[0];
    try {
      const depthStarted = performance.now();
      const depth = frame.getDepthInformation(view);
      this.performanceProfiler.record('depthAccess', performance.now() - depthStarted);
      if (depth === null) return;
""",
)

replace_once(
"""      const result = this.pointCloud.sampleDepthFrame(
        depth,
        depthGeometry(depth, view),
        context,
      );
""",
"""      const fusionStarted = performance.now();
      const result = this.pointCloud.sampleDepthFrame(
        depth,
        depthGeometry(depth, view),
        context,
      );
      this.performanceProfiler.record('fusion', performance.now() - fusionStarted);
""",
)

replace_once(
"""      manualPoints: this.manualPoints.size,
      motion: this.motionState,
""",
"""      manualPoints: this.manualPoints.size,
      operatorEdges: this.operatorEdges.size,
      motion: this.motionState,
      performance: this.performanceProfiler.snapshot(),
""",
)

replace_once(
"""    session.requestAnimationFrame(this.handleFrame);
    gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
""",
"""    session.requestAnimationFrame(this.handleFrame);
    this.performanceProfiler.recordFrameTimestamp(time);
    const frameWorkStarted = performance.now();
    gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
""",
)

replace_once(
"""    const pose = frame.getViewerPose(referenceSpace);
    this.frameCount += 1;
""",
"""    const pose = frame.getViewerPose(referenceSpace);
    if (pose !== null && pose.views.length > 0) {
      const view = pose.views[0];
      for (let index = 0; index < 16; index += 1) {
        this.latestProjectionMatrix[index] = Number(view.projectionMatrix[index]);
        this.latestWorldFromView[index] = Number(view.transform.matrix[index]);
      }
      this.hasLatestProjection = true;
    }
    this.frameCount += 1;
""",
)

replace_once(
"""    this.reportClientTelemetry(time);

    if (time - this.lastUiUpdateMs < 100 && this.frameCount > 1) return;
""",
"""    this.performanceProfiler.record('frameWork', performance.now() - frameWorkStarted);
    this.reportClientTelemetry(time);

    if (time - this.lastUiUpdateMs < 100 && this.frameCount > 1) return;
""",
)

replace_once(
"""      manualPointCount: this.manualPoints.size,
      lightingEstimateAvailable: this.lightingEstimateAvailable,
""",
"""      manualPointCount: this.manualPoints.size,
      manualEdgeCount: this.operatorEdges.size,
      performance: this.performanceProfiler.snapshot(),
      lightingEstimateAvailable: this.lightingEstimateAvailable,
""",
)

p.write_text(s)
print('Patched', p)
