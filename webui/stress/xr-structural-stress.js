import {
  StructuralGuideTracker,
  detectStructuralGuideGeometry,
} from '/src/xr/structuralGuide.ts';
import { applyCoverageRenderBudget } from '/src/xr/liveCoverageRenderer.ts';
import { XrPerformanceProfiler } from '/src/xr/xrPerformanceProfiler.ts';

const statusElement = document.getElementById('status');
const resultElement = document.getElementById('result');

const ITERATIONS = 180;
const GUIDE_P95_LIMIT_MS = 220;
const GUIDE_MAX_LIMIT_MS = 650;
const MAX_CANDIDATE_COMPARISON_RATIO = 0.80;
const MAX_HEAP_GROWTH_BYTES = 96 * 1024 * 1024;

const failures = [];
const pageErrors = [];
window.addEventListener('error', (event) => pageErrors.push(String(event.error?.stack ?? event.message ?? 'window error')));
window.addEventListener('unhandledrejection', (event) => pageErrors.push(String(event.reason?.stack ?? event.reason ?? 'unhandled rejection')));

function assertMetric(condition, message) {
  if (!condition) failures.push(message);
}

function percentile(values, fraction) {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const index = Math.min(sorted.length - 1, Math.max(0, Math.round((sorted.length - 1) * fraction)));
  return sorted[index];
}

function heapUsedBytes() {
  const memory = performance.memory;
  return memory && Number.isFinite(memory.usedJSHeapSize) ? memory.usedJSHeapSize : null;
}

function occupancyVoxelCenter(point, size = 0.18) {
  return [
    (Math.floor(point[0] / size) + 0.5) * size,
    (Math.floor(point[1] / size) + 0.5) * size,
    (Math.floor(point[2] / size) + 0.5) * size,
  ];
}

function occupancyVoxelKey(point, size = 0.18) {
  return `${Math.floor(point[0] / size)}:${Math.floor(point[1] / size)}:${Math.floor(point[2] / size)}`;
}

function scene(iteration = 0) {
  const positions = [];
  const normals = [];
  const confidence = [];
  const jitter = (axis, a, b) => 0.0018 * Math.sin(iteration * 0.31 + axis * 1.7 + a * 17 + b * 23);
  const push = (x, y, z, nx, ny, nz, q = 0.94) => {
    positions.push(x, y, z);
    normals.push(nx, ny, nz);
    confidence.push(q);
  };

  for (let a = -0.42; a <= 0.4201; a += 0.035) {
    for (let b = 0; b <= 0.7001; b += 0.035) {
      push(jitter(0, a, b), a, b, 1, 0, 0);
      push(a, jitter(1, a, b), b, 0, 1, 0);
      if (b <= 0.4201) push(a, b, jitter(2, a, b), 0, 0, 1);
    }
  }

  // Low-confidence clutter must not create structural evidence.
  for (let index = 0; index < 180; index += 1) {
    const x = -0.45 + (index % 18) * 0.05;
    const y = -0.40 + (Math.floor(index / 18) % 10) * 0.08;
    const z = 0.10 + 0.16 * Math.sin(index * 0.71);
    push(x, y, z, 0.577, 0.577, 0.577, 0.28);
  }

  return {
    positions: Float32Array.from(positions),
    normals: Float32Array.from(normals),
    confidence: Float32Array.from(confidence),
  };
}

function guideInput(cloud, keyframeCount, camera, freeSpacePositions) {
  return {
    ...cloud,
    keyframeCount,
    voxelSizeMeters: 0.04,
    viewpoint: { position: camera, keyframeId: keyframeCount },
    freeSpacePositions,
    freeSpaceVoxelSizeMeters: freeSpacePositions ? 0.18 : undefined,
  };
}

function makeRenderSnapshot(regularCount, manualCount) {
  const pointCount = regularCount + manualCount;
  const positions = new Float32Array(pointCount * 3);
  const confidence = new Float32Array(pointCount);
  for (let index = 0; index < pointCount; index += 1) {
    positions[index * 3] = index * 0.001;
    positions[index * 3 + 1] = (index % 97) * 0.002;
    positions[index * 3 + 2] = (index % 53) * 0.003;
    confidence[index] = index < regularCount ? 0.2 + (index % 7) * 0.1 : 2;
  }
  return { positions, confidence, pointCount, manualPointCount: manualCount };
}

async function run() {
  const initialCloud = scene(0);
  const baseline = detectStructuralGuideGeometry(guideInput(initialCloud, 10, [0.55, 0.55, 0.55]));
  assertMetric(baseline.corners.length > 0, 'baseline 3-plane scene produced no corner');
  assertMetric(baseline.edges.length > 0, 'baseline 3-plane scene produced no edge');

  const nearestCorner = [...baseline.corners].sort((a, b) => Math.hypot(...a.position) - Math.hypot(...b.position))[0];
  const freePosition = nearestCorner ? Float32Array.from(occupancyVoxelCenter(nearestCorner.position)) : undefined;
  const freeVoxelKey = freePosition ? occupancyVoxelKey(freePosition) : null;
  const contradicted = detectStructuralGuideGeometry(guideInput(initialCloud, 11, [0.55, 0.55, 0.55], freePosition));
  assertMetric(contradicted.rejectedByFreeSpace > 0, 'verified FREE space rejected no structural hypotheses');
  assertMetric(
    freeVoxelKey === null || !contradicted.corners.some((corner) => occupancyVoxelKey(corner.position) === freeVoxelKey),
    'FREE-space corner survived structural validation',
  );

  const staticTracker = new StructuralGuideTracker();
  for (const keyframe of [10, 20, 40, 80]) {
    const result = staticTracker.update(guideInput(initialCloud, keyframe, [0.55, 0.55, 0.55]));
    assertMetric(result.corners.length === 0, `same viewpoint matured a corner at keyframe ${keyframe}`);
  }
  const matured = staticTracker.update(guideInput(initialCloud, 84, [0.28, 0.52, 0.60]));
  assertMetric(matured.corners.length > 0, 'independent viewpoint did not mature a persistent corner');

  const profiler = new XrPerformanceProfiler();
  for (let index = 0; index < 80; index += 1) {
    profiler.recordFrameTimestamp(index * 46);
    profiler.record('fusion', 105);
    profiler.record('coverageUpload', 28);
    profiler.record('coverageDraw', 15);
    profiler.record('frameWork', 38);
  }
  const highPolicy = profiler.snapshot();
  assertMetric(highPolicy.pressure === 'high', 'synthetic pressure did not enter high mode');
  const rendered = applyCoverageRenderBudget(makeRenderSnapshot(12000, 4), highPolicy.adaptiveCoveragePointBudget);
  assertMetric(rendered.pointCount === highPolicy.adaptiveCoveragePointBudget + 4, 'LOD budget did not bound regular render points');
  assertMetric(rendered.manualPointCount === 4, 'LOD removed manual pins');
  assertMetric(Array.from(rendered.confidence.slice(-4)).every((value) => value === 2), 'LOD changed manual pin confidence');

  const tracker = new StructuralGuideTracker();
  const guideTimes = [];
  let maxComparisons = 0;
  let maxPatches = 0;
  const startHeap = heapUsedBytes();

  for (let iteration = 0; iteration < ITERATIONS; iteration += 1) {
    const cloud = scene(iteration);
    const angle = iteration * 0.052;
    const camera = [0.42 + 0.22 * Math.cos(angle), 0.50 + 0.16 * Math.sin(angle), 0.56 + 0.08 * Math.sin(angle * 0.7)];
    const before = performance.now();
    const result = tracker.update(guideInput(cloud, 100 + iteration, camera));
    const elapsed = performance.now() - before;
    guideTimes.push(elapsed);
    maxComparisons = Math.max(maxComparisons, result.candidateComparisons);
    maxPatches = Math.max(maxPatches, result.planarPatches);
    assertMetric(result.corners.length <= 8, `corner display cap exceeded at iteration ${iteration}`);
    assertMetric(result.edges.length <= 10, `edge display cap exceeded at iteration ${iteration}`);
    if (iteration % 12 === 0) await new Promise((resolve) => requestAnimationFrame(resolve));
  }

  const endHeap = heapUsedBytes();
  const p95 = percentile(guideTimes, 0.95);
  const max = guideTimes.length ? Math.max(...guideTimes) : 0;
  const allPairs = maxPatches > 1 ? maxPatches * (maxPatches - 1) / 2 : 1;
  const comparisonRatio = maxComparisons / allPairs;
  const heapGrowthBytes = startHeap !== null && endHeap !== null ? Math.max(0, endHeap - startHeap) : null;

  assertMetric(p95 <= GUIDE_P95_LIMIT_MS, `structural guide p95 ${p95.toFixed(1)} ms > ${GUIDE_P95_LIMIT_MS} ms`);
  assertMetric(max <= GUIDE_MAX_LIMIT_MS, `structural guide max ${max.toFixed(1)} ms > ${GUIDE_MAX_LIMIT_MS} ms`);
  assertMetric(comparisonRatio <= MAX_CANDIDATE_COMPARISON_RATIO, `spatial lookup compared ${(comparisonRatio * 100).toFixed(1)}% of all plane pairs`);
  if (heapGrowthBytes !== null) assertMetric(heapGrowthBytes <= MAX_HEAP_GROWTH_BYTES, `heap growth ${(heapGrowthBytes / 1048576).toFixed(1)} MiB > ${MAX_HEAP_GROWTH_BYTES / 1048576} MiB`);
  assertMetric(pageErrors.length === 0, `browser errors: ${pageErrors.join(' | ')}`);

  const result = {
    pass: failures.length === 0,
    workload: {
      iterations: ITERATIONS,
      sourcePoints: initialCloud.confidence.length,
      maxPlanarPatches: maxPatches,
      maxCandidateComparisons: maxComparisons,
      allPairsAtMaxPatchCount: allPairs,
      comparisonRatio,
    },
    structural: {
      baselineCorners: baseline.corners.length,
      baselineEdges: baseline.edges.length,
      freeSpaceRejected: contradicted.rejectedByFreeSpace,
      maturedCorners: matured.corners.length,
      p50Ms: percentile(guideTimes, 0.50),
      p95Ms: p95,
      maxMs: max,
    },
    renderPolicy: {
      pressure: highPolicy.pressure,
      pointBudget: highPolicy.adaptiveCoveragePointBudget,
      drawIntervalMs: highPolicy.adaptiveCoverageDrawIntervalMs,
      renderedPoints: rendered.pointCount,
      manualPoints: rendered.manualPointCount,
    },
    memory: { startHeapBytes: startHeap, endHeapBytes: endHeap, growthBytes: heapGrowthBytes },
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
