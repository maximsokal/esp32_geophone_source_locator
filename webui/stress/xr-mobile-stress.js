import { DepthPointCloudAccumulator } from '/src/xr/depthPointCloud.ts';
import { buildSurfaceMeshPreview } from '/src/xr/meshPreview.ts';
import { WebXrFusionKeyframeSelector } from '/src/xr/webxrFusionQuality.ts';

const statusElement = document.getElementById('status');
const resultElement = document.getElementById('result');

const LOGICAL_CAPTURE_INTERVAL_MS = 140;
const LOGICAL_FRAMES = 480;
const COVERAGE_EVERY_INTEGRATED_FRAMES = 4;
const YIELD_EVERY_LOGICAL_FRAMES = 1;
const SYNTHETIC_DEPTH_WIDTH = 320;
const SYNTHETIC_DEPTH_HEIGHT = 180;
const DEPTH_READ_HARD_LIMIT = 40000;

const thresholds = {
  fusionP95Ms: 140,
  fusionP99Ms: 220,
  fusionMaxMs: 500,
  coverageP95Ms: 110,
  coverageMaxMs: 400,
  snapshotMaxMs: 900,
  meshMaxMs: 1500,
  latencyGrowthRatio: 2.0,
  heapGrowthBytes: 160 * 1024 * 1024,
  heapPeakBytes: 320 * 1024 * 1024,
  longestLongTaskMs: 700,
  maxDepthReadsPerIntegratedFrame: DEPTH_READ_HARD_LIMIT,
};

const failures = [];
const pageErrors = [];
const longTasks = [];

window.addEventListener('error', (event) => {
  pageErrors.push(String(event.error?.stack ?? event.message ?? 'window error'));
});
window.addEventListener('unhandledrejection', (event) => {
  pageErrors.push(String(event.reason?.stack ?? event.reason ?? 'unhandled rejection'));
});

if ('PerformanceObserver' in window) {
  try {
    const observer = new PerformanceObserver((list) => {
      for (const entry of list.getEntries()) longTasks.push(entry.duration);
    });
    observer.observe({ entryTypes: ['longtask'] });
  } catch {
    // Long Task API is advisory; headless Chromium revisions may omit it.
  }
}

function percentile(values, fraction) {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.max(0, Math.round((sorted.length - 1) * fraction)));
  return sorted[index];
}

function summarize(values) {
  return {
    count: values.length,
    meanMs: values.length === 0 ? 0 : values.reduce((sum, value) => sum + value, 0) / values.length,
    p50Ms: percentile(values, 0.50),
    p95Ms: percentile(values, 0.95),
    p99Ms: percentile(values, 0.99),
    maxMs: values.length === 0 ? 0 : Math.max(...values),
  };
}

function heapUsedBytes() {
  const memory = performance.memory;
  return memory && Number.isFinite(memory.usedJSHeapSize) ? memory.usedJSHeapSize : null;
}

function makeProjection() {
  const fovY = 60 * Math.PI / 180;
  const aspect = SYNTHETIC_DEPTH_WIDTH / SYNTHETIC_DEPTH_HEIGHT;
  const f = 1 / Math.tan(fovY * 0.5);
  return new Float32Array([
    f / aspect, 0, 0, 0,
    0, f, 0, 0,
    0, 0, -1, -1,
    0, 0, -0.2, 0,
  ]);
}

function makeWorldFromView(tx, ty, tz, yaw) {
  const c = Math.cos(yaw);
  const s = Math.sin(yaw);
  return new Float32Array([
    c, 0, -s, 0,
    0, 1, 0, 0,
    s, 0, c, 0,
    tx, ty, tz, 1,
  ]);
}

function makePose(frameIndex) {
  const t = frameIndex * LOGICAL_CAPTURE_INTERVAL_MS / 1000;
  const x = 1.05 * Math.sin(t * 0.22);
  const y = 0.08 * Math.sin(t * 0.13);
  const z = 0.18 * Math.cos(t * 0.17);
  const yaw = 0.32 * Math.sin(t * 0.16);
  const half = yaw * 0.5;
  return {
    position: { x, y, z },
    orientation: { x: 0, y: Math.sin(half), z: 0, w: Math.cos(half) },
    yaw,
  };
}

function syntheticDepthMeters(u, v, frameIndex) {
  const rubble =
    2.35 +
    0.18 * Math.sin(u * Math.PI * 6.0) * Math.cos(v * Math.PI * 4.0) +
    0.07 * Math.sin((u + v) * Math.PI * 13.0);
  const step = u > 0.58 ? 0.055 : 0;
  const shallowPocket = (u > 0.22 && u < 0.34 && v > 0.46 && v < 0.70) ? -0.11 : 0;
  const temporalNoise = 0.004 * Math.sin(frameIndex * 0.73 + u * 19 + v * 11);
  return rubble + step + shallowPocket + temporalNoise;
}

function nextFrame() {
  return new Promise((resolve) => requestAnimationFrame(() => resolve()));
}

function assertMetric(condition, message) {
  if (!condition) failures.push(message);
}

async function run() {
  const accumulator = new DepthPointCloudAccumulator(0.04, 60000, 5.0);
  const selector = new WebXrFusionKeyframeSelector();
  const projection = makeProjection();
  const fusionTimes = [];
  const coverageTimes = [];
  const depthReadsPerIntegratedFrame = [];
  const heapSamples = [];
  const yieldDelays = [];
  const decisionCounts = { keyframe: 0, polish: 0, skip: 0 };
  let integratedFrames = 0;
  let lastYieldStart = performance.now();

  const startHeap = heapUsedBytes();
  if (startHeap !== null) heapSamples.push(startHeap);
  const wallStart = performance.now();

  for (let frameIndex = 0; frameIndex < LOGICAL_FRAMES; frameIndex += 1) {
    const logicalTime = frameIndex * LOGICAL_CAPTURE_INTERVAL_MS;
    const pose = makePose(frameIndex);
    const decision = selector.evaluate(logicalTime, pose);
    decisionCounts[decision.kind] += 1;

    if (decision.kind !== 'skip') {
      let depthReads = 0;
      const transform = makeWorldFromView(
        pose.position.x,
        pose.position.y,
        pose.position.z,
        pose.yaw,
      );
      const depth = {
        width: SYNTHETIC_DEPTH_WIDTH,
        height: SYNTHETIC_DEPTH_HEIGHT,
        projectionMatrix: projection,
        transform: { matrix: transform },
        getDepthInMeters(u, v) {
          depthReads += 1;
          if (!(u > 0 && u < 1 && v > 0 && v < 1)) return Number.NaN;
          return syntheticDepthMeters(u, v, frameIndex);
        },
      };
      const view = { projectionMatrix: projection, transform: { matrix: transform } };
      const context = {
        keyframeId: decision.keyframeId,
        evidenceKeyframe: decision.kind === 'keyframe',
        frameWeight: decision.frameWeight,
        cameraProjectPosition: [pose.position.x, -pose.position.z, pose.position.y],
      };

      const before = performance.now();
      accumulator.sampleDepthFrame(depth, view, context);
      fusionTimes.push(performance.now() - before);
      depthReadsPerIntegratedFrame.push(depthReads);
      integratedFrames += 1;

      if (integratedFrames % COVERAGE_EVERY_INTEGRATED_FRAMES === 0) {
        const coverageBefore = performance.now();
        accumulator.liveCoverageSnapshot();
        coverageTimes.push(performance.now() - coverageBefore);
      }
    }

    if (frameIndex % 24 === 0) {
      const heap = heapUsedBytes();
      if (heap !== null) heapSamples.push(heap);
    }

    if (frameIndex % YIELD_EVERY_LOGICAL_FRAMES === 0) {
      const beforeYield = performance.now();
      await nextFrame();
      const afterYield = performance.now();
      yieldDelays.push(afterYield - beforeYield);
      lastYieldStart = afterYield;
    }
  }

  void lastYieldStart;
  const snapshotBefore = performance.now();
  const snapshot = accumulator.snapshot(LOGICAL_FRAMES, integratedFrames);
  const snapshotMs = performance.now() - snapshotBefore;

  const meshBefore = performance.now();
  const mesh = buildSurfaceMeshPreview({
    positions: snapshot.positions,
    normals: snapshot.normals,
    confidence: snapshot.confidence,
    voxelSizeMeters: snapshot.voxelSizeMeters,
  });
  const meshMs = performance.now() - meshBefore;

  const endHeap = heapUsedBytes();
  if (endHeap !== null) heapSamples.push(endHeap);
  const wallMs = performance.now() - wallStart;

  const fusion = summarize(fusionTimes);
  const coverage = summarize(coverageTimes);
  const yields = summarize(yieldDelays);
  const firstQuarter = fusionTimes.slice(0, Math.max(1, Math.floor(fusionTimes.length / 4)));
  const lastQuarter = fusionTimes.slice(Math.max(0, Math.floor(fusionTimes.length * 0.75)));
  const firstQuarterP95 = percentile(firstQuarter, 0.95);
  const lastQuarterP95 = percentile(lastQuarter, 0.95);
  const latencyGrowthRatio = firstQuarterP95 > 0 ? lastQuarterP95 / firstQuarterP95 : 0;
  const heapPeakBytes = heapSamples.length > 0 ? Math.max(...heapSamples) : null;
  const heapGrowthBytes = startHeap !== null && endHeap !== null ? Math.max(0, endHeap - startHeap) : null;
  const longestLongTaskMs = longTasks.length > 0 ? Math.max(...longTasks) : 0;
  const maxDepthReads = depthReadsPerIntegratedFrame.length > 0
    ? Math.max(...depthReadsPerIntegratedFrame)
    : 0;

  assertMetric(fusion.p95Ms <= thresholds.fusionP95Ms, `fusion p95 ${fusion.p95Ms.toFixed(1)} ms > ${thresholds.fusionP95Ms} ms`);
  assertMetric(fusion.p99Ms <= thresholds.fusionP99Ms, `fusion p99 ${fusion.p99Ms.toFixed(1)} ms > ${thresholds.fusionP99Ms} ms`);
  assertMetric(fusion.maxMs <= thresholds.fusionMaxMs, `fusion max ${fusion.maxMs.toFixed(1)} ms > ${thresholds.fusionMaxMs} ms`);
  assertMetric(coverage.p95Ms <= thresholds.coverageP95Ms, `coverage p95 ${coverage.p95Ms.toFixed(1)} ms > ${thresholds.coverageP95Ms} ms`);
  assertMetric(coverage.maxMs <= thresholds.coverageMaxMs, `coverage max ${coverage.maxMs.toFixed(1)} ms > ${thresholds.coverageMaxMs} ms`);
  assertMetric(snapshotMs <= thresholds.snapshotMaxMs, `final snapshot ${snapshotMs.toFixed(1)} ms > ${thresholds.snapshotMaxMs} ms`);
  assertMetric(meshMs <= thresholds.meshMaxMs, `mesh preview ${meshMs.toFixed(1)} ms > ${thresholds.meshMaxMs} ms`);
  assertMetric(latencyGrowthRatio <= thresholds.latencyGrowthRatio, `fusion p95 grew ${latencyGrowthRatio.toFixed(2)}x from first to last quarter`);
  assertMetric(maxDepthReads <= thresholds.maxDepthReadsPerIntegratedFrame, `depth reads/frame ${maxDepthReads} > ${thresholds.maxDepthReadsPerIntegratedFrame}; possible accidental nested loop/recursion`);
  assertMetric(snapshot.rawPointCount <= snapshot.memoryBudgetPoints, `retained ${snapshot.rawPointCount} > memory budget ${snapshot.memoryBudgetPoints}`);
  assertMetric(snapshot.knownFreeVoxelCount > 0, 'stress scan produced no confirmed FREE voxels');
  assertMetric(snapshot.occupiedSurfaceVoxelCount > 0, 'stress scan produced no confirmed SURFACE voxels');
  assertMetric(snapshot.unknownCandidateVoxelCount > 0, 'stress scan produced no UNKNOWN candidate volume');
  assertMetric(pageErrors.length === 0, `browser errors: ${pageErrors.join(' | ')}`);
  assertMetric(longestLongTaskMs <= thresholds.longestLongTaskMs, `longest browser long task ${longestLongTaskMs.toFixed(1)} ms > ${thresholds.longestLongTaskMs} ms`);
  if (heapPeakBytes !== null) assertMetric(heapPeakBytes <= thresholds.heapPeakBytes, `heap peak ${(heapPeakBytes / 1048576).toFixed(1)} MiB > ${(thresholds.heapPeakBytes / 1048576).toFixed(0)} MiB`);
  if (heapGrowthBytes !== null) assertMetric(heapGrowthBytes <= thresholds.heapGrowthBytes, `heap growth ${(heapGrowthBytes / 1048576).toFixed(1)} MiB > ${(thresholds.heapGrowthBytes / 1048576).toFixed(0)} MiB`);

  const result = {
    pass: failures.length === 0,
    workload: {
      logicalFrames: LOGICAL_FRAMES,
      logicalScanSeconds: LOGICAL_FRAMES * LOGICAL_CAPTURE_INTERVAL_MS / 1000,
      integratedFrames,
      decisionCounts,
      depthResolution: `${SYNTHETIC_DEPTH_WIDTH}x${SYNTHETIC_DEPTH_HEIGHT}`,
      coverageEveryIntegratedFrames: COVERAGE_EVERY_INTEGRATED_FRAMES,
    },
    timing: {
      wallMs,
      fusion,
      coverage,
      requestAnimationFrameYield: yields,
      snapshotMs,
      meshMs,
      latencyGrowthRatio,
      firstQuarterP95Ms: firstQuarterP95,
      lastQuarterP95Ms: lastQuarterP95,
      longestLongTaskMs,
      longTaskCount: longTasks.length,
    },
    workAmplification: {
      maxDepthReadsPerIntegratedFrame: maxDepthReads,
      meanDepthReadsPerIntegratedFrame: depthReadsPerIntegratedFrame.length === 0
        ? 0
        : depthReadsPerIntegratedFrame.reduce((a, b) => a + b, 0) / depthReadsPerIntegratedFrame.length,
    },
    memory: {
      startHeapBytes: startHeap,
      endHeapBytes: endHeap,
      peakHeapBytes: heapPeakBytes,
      growthBytes: heapGrowthBytes,
      retainedPoints: snapshot.rawPointCount,
      memoryBudgetPoints: snapshot.memoryBudgetPoints,
      coverageCells: snapshot.coverageCellCount,
    },
    rescueGeometry: {
      stableSurfacePoints: snapshot.pointCount,
      tentativePoints: snapshot.tentativePointCount,
      freeVoxels: snapshot.knownFreeVoxelCount,
      surfaceVoxels: snapshot.occupiedSurfaceVoxelCount,
      unknownCandidateVoxels: snapshot.unknownCandidateVoxelCount,
      meshTriangles: mesh.triangleCount,
    },
    thresholds,
    failures,
    pageErrors,
  };

  window.__XR_STRESS_RESULT__ = result;
  window.__XR_STRESS_DONE__ = true;
  statusElement.textContent = result.pass ? 'PASS' : 'FAIL';
  resultElement.textContent = JSON.stringify(result, null, 2);
  console.log('XR_STRESS_RESULT ' + JSON.stringify(result));
}

window.__XR_STRESS_DONE__ = false;
window.__XR_STRESS_RESULT__ = null;
run().catch((error) => {
  const failure = String(error?.stack ?? error);
  window.__XR_STRESS_RESULT__ = { pass: false, failures: [failure], pageErrors };
  window.__XR_STRESS_DONE__ = true;
  statusElement.textContent = 'FAIL';
  resultElement.textContent = failure;
  console.error('XR_STRESS_FATAL', error);
});
