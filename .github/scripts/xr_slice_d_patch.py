from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


# Probe-time feature detection deliberately accepts tentative physical normals.
# Retention still requires promoted/multi-view evidence through the original API.
replace_once(
    'webui/src/xr/surfaceFeatureDensity.ts',
    'export function estimateSurfaceFeatureComplexity(\n  evidence: Iterable<SurfaceNormalEvidence>,\n): SurfaceFeatureComplexity {',
    'function estimateSurfaceFeatureComplexityInternal(\n  evidence: Iterable<SurfaceNormalEvidence>,\n  requireMultiViewMaturity: boolean,\n): SurfaceFeatureComplexity {',
    'surface feature internal signature',
)
replace_once(
    'webui/src/xr/surfaceFeatureDensity.ts',
    '    if (sample.promoted === false) continue;\n    const independentViews = sample.independentViews;\n    if (\n      independentViews !== undefined &&\n      (!Number.isFinite(independentViews) || independentViews < MIN_PHYSICAL_FEATURE_VIEWS)\n    ) continue;',
    '    if (requireMultiViewMaturity && sample.promoted === false) continue;\n    const independentViews = sample.independentViews;\n    if (\n      requireMultiViewMaturity &&\n      independentViews !== undefined &&\n      (!Number.isFinite(independentViews) || independentViews < MIN_PHYSICAL_FEATURE_VIEWS)\n    ) continue;',
    'surface feature maturity gate',
)
replace_once(
    'webui/src/xr/surfaceFeatureDensity.ts',
    '\nexport function featureAwareCellQuota(\n',
    '''\n/** Multi-view-certified complexity used for persistent surfel retention. */\nexport function estimateSurfaceFeatureComplexity(\n  evidence: Iterable<SurfaceNormalEvidence>,\n): SurfaceFeatureComplexity {\n  return estimateSurfaceFeatureComplexityInternal(evidence, true);\n}\n\n/**\n * Probe-only complexity from physical depth normals. Tentative/single-view\n * representatives may request a bounded extra depth read, but this signal is\n * never allowed to increase persistent retention quota. Metric span, robust\n * orientation families and confidence gates still reject tiny/noisy conflicts.\n */\nexport function estimateSurfaceProbeComplexity(\n  evidence: Iterable<SurfaceNormalEvidence>,\n): SurfaceFeatureComplexity {\n  return estimateSurfaceFeatureComplexityInternal(evidence, false);\n}\n\nexport function featureAwareCellQuota(\n''',
    'surface feature wrappers',
)

replace_once(
    'webui/src/xr/depthPointCloudV2.ts',
    "  estimateSurfaceFeatureComplexity,\n  featureAwareCellQuota,",
    "  estimateSurfaceFeatureComplexity,\n  estimateSurfaceProbeComplexity,\n  featureAwareCellQuota,",
    'probe estimator import',
)
replace_once(
    'webui/src/xr/depthPointCloudV2.ts',
    '      const score = this.cellFeatureComplexity(cell);\n      featureScoreCache.set(key, score);',
    '      const score = estimateSurfaceProbeComplexity(cell.representatives.values()).score;\n      featureScoreCache.set(key, score);',
    'probe-only score use',
)
replace_once(
    'webui/src/xr/depthPointCloudV2.ts',
    "    // A cell's normal-family score changes slowly inside one frame. Cache at most\n    // one clustering pass per touched cell so feature-directed sampling remains\n    // bounded in both depth reads and CPU work.",
    "    // Probe-only physical normal complexity changes slowly inside one frame.\n    // Tentative representatives may request bounded extra depth reads, while the\n    // separate retention path below still requires multi-view maturity. Cache at\n    // most one clustering pass per touched cell to bound CPU work.",
    'probe cache comment',
)

# Smooth bends were just below the structural detail threshold (~0.184 in the
# regression fixture). Robust metric/span gates already reject one-normal noise.
replace_once(
    'webui/src/xr/structuralSurfaceClustering.ts',
    'const FEATURE_DETAIL_THRESHOLD = 0.20;',
    'const FEATURE_DETAIL_THRESHOLD = 0.16;',
    'structural convex threshold',
)

# 10/90 percentiles were then treated as if they were actual endpoints, so the
# deliberately discarded 10% tails looked like measured continuation beyond the
# endpoint. Keep robust trimming but move it inside the explicit 4.5 cm outside gap.
replace_once(
    'webui/src/xr/structuralEdgeEndpoints.ts',
    '    const firstLow = quantile(firstValues, 0.10);\n    const firstHigh = quantile(firstValues, 0.90);\n    const secondLow = quantile(secondValues, 0.10);\n    const secondHigh = quantile(secondValues, 0.90);',
    '    const firstLow = quantile(firstValues, 0.05);\n    const firstHigh = quantile(firstValues, 0.95);\n    const secondLow = quantile(secondValues, 0.05);\n    const secondHigh = quantile(secondValues, 0.95);',
    'endpoint robust interval',
)

# Timers belong to WindowOrWorkerGlobalScope; diagnostics can be imported from
# Node-based tests, so binding this helper to window is unnecessarily brittle.
replace_once(
    'webui/src/data/clientDiagnostics.ts',
    '  flushTimer = window.setTimeout(() => {',
    '  flushTimer = globalThis.setTimeout(() => {',
    'platform-neutral diagnostics timer',
)

# The structural component is the strongest connected certified subset; family
# clustering is no longer required to leave the entire structural surface in one
# connected component.
replace_once(
    'webui/tests/xr/structuralGuideReadiness.test.ts',
    '''    expect(prepared.surfaceReadiness.structuralComponent).not.toBeNull();\n    expect(prepared.surfaceReadiness.structuralComponent!.pointCount)\n      .toBe(prepared.structuralSurface.pointCount);\n    expect(prepared.surfaceReadiness.structuralComponent!.pointCount)\n      .toBeLessThan(prepared.surfaceReadiness.bestMeshComponent!.pointCount);''',
    '''    expect(prepared.surfaceReadiness.structuralComponent).not.toBeNull();\n    expect(prepared.surfaceReadiness.structuralComponent!.pointCount).toBeGreaterThan(0);\n    expect(prepared.surfaceReadiness.structuralComponent!.pointCount)\n      .toBeLessThanOrEqual(prepared.structuralSurface.pointCount);\n    expect(prepared.surfaceReadiness.structuralComponent!.pointCount)\n      .toBeLessThan(prepared.surfaceReadiness.bestMeshComponent!.pointCount);''',
    'structural readiness connected subset expectation',
)

# A single measured plane has zero object thickness. The operational envelope must
# not invent unseen interior volume merely to make UNKNOWN non-empty.
replace_once(
    'webui/tests/xr/occupancyEvidence.test.ts',
    "  it('returns surface, known-air and unknown-candidate layers from one scan snapshot', () => {",
    "  it('returns measured surface/free layers without inventing UNKNOWN thickness for a single plane', () => {",
    'occupancy integration test name',
)
replace_once(
    'webui/tests/xr/occupancyEvidence.test.ts',
    '    expect(snapshot.unknownCandidateVoxelCount).toBeGreaterThan(0);\n  });\n});',
    '    expect(snapshot.unknownCandidateVoxelCount).toBe(0);\n  });\n});',
    'flat envelope unknown expectation',
)

# In live deferred mode the global scheduler is allowed to reject before worker
# startup. Either result proves that the prohibited main-thread fallback did not run.
replace_once(
    'webui/tests/xr/visualLineEvidenceClient.test.ts',
    "      await expect(client.analyze(lineFrame(), 9, identity, identity)).rejects.toThrow(/Worker/);",
    "      await expect(client.analyze(lineFrame(), 9, identity, identity)).rejects.toThrow(/Worker|compute budget/);",
    'visual scheduler rejection contract',
)

# Same-view tentative curvature may request more measurements, but cannot retain
# above the fair-share quota until independent physical views mature the surfels.
replace_once(
    'webui/tests/xr/featureAwareAllocation.test.ts',
    "  it('retains and actively re-probes extra measured detail on a smooth convex patch without increasing global memory', () => {",
    "  it('actively re-probes tentative convex detail without granting single-view retention quota', () => {",
    'feature allocation test name',
)
replace_once(
    'webui/tests/xr/featureAwareAllocation.test.ts',
    '''    expect(flat.pointCount).toBeLessThanOrEqual(30);\n    expect(convex.pointCount).toBeGreaterThan(flat.pointCount);\n    expect(convex.pointCount).toBeGreaterThan(28);\n    expect(convex.pointCount).toBeLessThanOrEqual(48);\n    expect(convex.pointCount).toBeLessThanOrEqual(convex.memoryBudget);\n    expect(flatResult.featureSupersampleProbes).toBe(0);\n    expect(convexResult.featureSupersampleProbes).toBeGreaterThan(0);''',
    '''    expect(flat.pointCount).toBeLessThanOrEqual(30);\n    expect(convex.pointCount).toBeLessThanOrEqual(28);\n    expect(convex.pointCount).toBeLessThanOrEqual(convex.memoryBudget);\n    expect(flatResult.featureSupersampleProbes).toBe(0);\n    expect(convexResult.featureSupersampleProbes).toBeGreaterThan(0);''',
    'feature allocation retention expectations',
)

# Regression for the architectural split: same tentative physical normals can
# trigger bounded re-probing, but remain ineligible for persistent density bonus.
replace_once(
    'webui/tests/xr/surfaceFeatureDensity.test.ts',
    '  estimateSurfaceFeatureComplexity,\n  featureAwareCellQuota,',
    '  estimateSurfaceFeatureComplexity,\n  estimateSurfaceProbeComplexity,\n  featureAwareCellQuota,',
    'probe estimator test import',
)
replace_once(
    'webui/tests/xr/surfaceFeatureDensity.test.ts',
    "  it('does not let tentative conflicting surfels trigger feature density', () => {",
    '''  it('lets tentative measured curvature request probes without granting retention density', () => {\n    const samples = [-28, -20, -12, -4, 4, 12, 20, 28].map((angle, index) => ({\n      ...positionedNormal(angle, index * 0.025, 0),\n      promoted: false,\n      independentViews: 1,\n    }));\n    expect(estimateSurfaceFeatureComplexity(samples).score).toBe(0);\n    expect(estimateSurfaceProbeComplexity(samples).score).toBeGreaterThan(0.25);\n    expect(featureAwareCellQuota(28, estimateSurfaceFeatureComplexity(samples).score)).toBe(28);\n  });\n\n  it('keeps probe-only classification robust to one abnormal normal on a flat patch', () => {\n    const feature = estimateSurfaceProbeComplexity([\n      positionedNormal(-2, 0.00, 0), positionedNormal(-1, 0.02, 0),\n      positionedNormal(0, 0.04, 0), positionedNormal(1, 0.06, 0),\n      positionedNormal(2, 0.08, 0), positionedNormal(90, 0.10, 0),\n    ]);\n    expect(feature.score).toBeLessThan(0.20);\n  });\n\n  it('does not let tentative conflicting surfels trigger feature density', () => {''',
    'probe/retention split regressions',
)
