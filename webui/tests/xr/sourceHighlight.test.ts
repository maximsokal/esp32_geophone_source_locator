import { describe, expect, it } from 'vitest';
import sourceVectors from '../../../tests/fixtures/source_highlight_vectors.json';

import type { LocalizationRun } from '../../src/api/deviceApi';
import {
  buildSourceHighlightScene,
  selectLocalizationSources,
  sourceVoxelGridForScan,
  sourceUncertaintyVoxelIndices,
  sourcePositionToProject,
  sourceToVoxelIndex,
  voxelCell,
  type SourceVoxelGrid,
} from '../../src/xr/sourceHighlight';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import { displayForScanMode } from '../../src/xr/scanDisplayModes';
import { prepareScanScene, scanLayerPlan, type ScanSceneSource } from '../../src/xr/scanViewportScene';

const IDENTITY = { objectId: 'OBJ_0001', sessionId: 'S000001' } as const;
const GRID: SourceVoxelGrid = { origin: [0, 0, 0], dimensions: [2, 2, 2], voxelSizeMeters: 1 };
const SENSOR_TO_LOCAL_FLOOR = [1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1];

function registration(matrix = SENSOR_TO_LOCAL_FLOOR, geometryComputedAtMs = 1, scanFrameId = 'scan-crc32:current'): RegistrationArtifact {
  return {
    schema: 1, source: 'apriltag', family: 'tag36h11', geometry_computed_at_ms: geometryComputedAtMs, created_at_ms: 1,
    scan_frame_id: scanFrameId,
    tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
    bindings: [], sensor_to_scan: matrix, mirrored: false, residual_rms_m: 0, threshold_m: 0.05,
    per_node: [], tier: 'minimum', valid: true, reason: null,
  };
}

function run(overrides: Partial<LocalizationRun> = {}): LocalizationRun {
  return {
    schema_version: 4, object_id: IDENTITY.objectId, session_id: IDENTITY.sessionId, computed_at_ms: 1,
    kind: 'individual_method', method_name: 'method_a', method_version: 1,
    channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'capable_3d',
    geometry_computed_at_ms: 1, propagation_model_name: '', propagation_speed_mps: 340,
    propagation_speed_uncertainty_mps: 0, confidence_score: 0.5, solved: true,
    source_position: { x_m: 0.5, y_m: 0.5, z_m: 0.5 }, ...overrides,
  };
}

function sceneSource(): ScanSceneSource {
  return {
    bounds: { min: [0, 0, 0], max: [2, 2, 2] },
    positions: new Float32Array([0, 0, 0]), confidence: new Float32Array([1]),
    tentativePositions: new Float32Array(0), tentativeConfidence: new Float32Array(0),
    manualControlPoints: [], operatorEdges: [], freeAirPositions: new Float32Array(0),
    unknownCandidatePositions: new Float32Array(0),
  };
}

describe('source voxel indexing', () => {
  it('maps interior and boundary points, with inclusive lower and exclusive upper faces', () => {
    expect(sourceToVoxelIndex([0.5, 0.5, 0.5], GRID)).toBe(0);
    expect(sourceToVoxelIndex([0, 0, 0], GRID)).toBe(0);
    expect(sourceToVoxelIndex([1, 0.5, 0.5], GRID)).toBe(1);
    expect(sourceToVoxelIndex([2, 0.5, 0.5], GRID)).toBeNull();
    expect(sourceToVoxelIndex([-0.001, 0.5, 0.5], GRID)).toBeNull();
    expect(voxelCell(7, GRID)).toEqual({ index: 7, coordinates: [1, 1, 1], center: [1.5, 1.5, 1.5] });
    expect(voxelCell(8, GRID)).toBeNull();
  });

  it('matches the C renderer mapping vectors', () => {
    for (const [ox, oy, oz, dx, dy, dz, size, x, y, z, expected] of sourceVectors.mapping) {
      const grid: SourceVoxelGrid = {
        origin: [ox, oy, oz], dimensions: [dx, dy, dz], voxelSizeMeters: size,
      };
      expect(sourceToVoxelIndex([x, y, z], grid)).toBe(expected < 0 ? null : expected);
    }
  });

  it('matches the C horizontal ellipse, degenerate sigma, unknown sigma, and cap vectors', () => {
    for (const [ox, oy, oz, dx, dy, dz, size, x, y, z, sigmaX, sigmaY, valid, cap, ...expectedValues] of sourceVectors.uncertainty) {
      const grid: SourceVoxelGrid = {
        origin: [ox, oy, oz], dimensions: [dx, dy, dz], voxelSizeMeters: size,
      };
      const expected = expectedValues.filter((index) => index >= 0);
      expect(sourceUncertaintyVoxelIndices(
        [x, y, z], valid === 0 ? undefined : sigmaX, valid === 0 ? undefined : sigmaY, grid, cap,
      )).toEqual(expected);
    }
  });

  it('converts a registered sensor point through local-floor into project coordinates', () => {
    expect(sourcePositionToProject({ x_m: 1, y_m: 2, z_m: 3 }, registration([1, 0, 0, 4, 0, 1, 0, 5, 0, 0, 1, 6, 0, 0, 0, 1])))
      .toEqual([5, -9, 7]);
    expect(sourcePositionToProject({ x_m: 1, y_m: 2, z_m: 3 }, null)).toBeNull();
  });
});

describe('persisted run selection and source layer', () => {
  it('prefers an accepted joint result, falls back from an uncertain joint, and drops old reruns', () => {
    const method = run({ method_name: 'method_a', confidence_score: 0.9, computed_at_ms: 1 });
    const second = run({ method_name: 'method_b', confidence_score: 0.7, computed_at_ms: 2 });
    const joint = run({ kind: 'joint_fusion', method_name: 'joint_fusion', confidence_score: 0.2, computed_at_ms: 3 });
    const selected = selectLocalizationSources([method, second, joint, run({ ...method, computed_at_ms: 4, solved: false })], IDENTITY);
    expect(selected.map((item) => [item.run.method_name, item.primary])).toEqual([
      ['joint_fusion', true], ['method_b', false],
    ]);

    const uncertain = { ...joint, uncertainty_too_high: true };
    const fallback = selectLocalizationSources([method, second, uncertain], IDENTITY);
    expect(fallback.map((item) => [item.run.method_name, item.primary])).toEqual([
      ['method_a', true], ['method_b', false], ['joint_fusion', false],
    ]);
  });

  it('handles a single method, no solved run, mismatched session, and multiple sources', () => {
    expect(selectLocalizationSources([run()], IDENTITY).map((source) => [source.run.method_name, source.primary]))
      .toEqual([['method_a', true]]);
    expect(selectLocalizationSources([run({ solved: false }), run({ object_id: 'OBJ_0002' })], IDENTITY)).toEqual([]);
    expect(selectLocalizationSources([run(), run({ method_name: 'method_b' })], IDENTITY))
      .toHaveLength(2);
  });

  it('suppresses every source layer when registration is missing and reports the predicate reason', () => {
    const result = buildSourceHighlightScene([run()], IDENTITY, null, GRID);
    expect(result.reason).toBe('missing');
    expect(result.primary).toBeNull();
    expect(result.secondary).toEqual([]);
    const plan = scanLayerPlan(prepareScanScene(sceneSource()), displayForScanMode('quality'), new Float32Array(0), false, [], result);
    expect(plan.some((layer) => layer.dataType === 'source-highlight')).toBe(false);
  });

  it('places a primary voxel and all solved alternatives, and reports an outside-grid edge marker', () => {
    const uncertainty = {
      std_m: { x_m: 1.1, y_m: 1.1, z_m: 0 }, credible_level: 0.9,
      credible_volume_m3: 0, credible_area_m2: 0,
      credible_min: { x_m: 0, y_m: 0, z_m: 0 }, credible_max: { x_m: 1, y_m: 1, z_m: 0 },
    };
    const inside = buildSourceHighlightScene(
      [run({ uncertainty }), run({ method_name: 'method_b', source_position: { x_m: 1.5, y_m: 0.5, z_m: 0.5 } })],
      IDENTITY, registration(), GRID, { currentScanId: 'scan-crc32:current' },
    );
    expect(inside.primary?.voxel?.index).toBe(0);
    expect(inside.uncertaintyVoxels.map((cell) => cell.index)).toEqual([0, 1, 2, 3]);
    expect(inside.secondary).toHaveLength(1);
    const plan = scanLayerPlan(prepareScanScene(sceneSource()), displayForScanMode('quality'), new Float32Array(0), false, [], inside);
    const sourceLayer = plan.find((layer) => layer.dataType === 'source-highlight');
    expect(sourceLayer?.kind).toBe('source-highlights');
    if (sourceLayer?.kind === 'source-highlights') expect(sourceLayer.uncertaintyVoxels).toHaveLength(4);

    const outside = buildSourceHighlightScene(
      [run({ source_position: { x_m: 2.5, y_m: 0.5, z_m: 0.5 } })], IDENTITY, registration(), GRID,
      { currentScanId: 'scan-crc32:current' },
    );
    expect(outside.reason).toBe('outside_grid');
    expect(outside.primary?.voxel).toBeNull();
    expect(outside.primary?.edgeMarker).toEqual([1.5, 0.5, 0.5]);
  });

  it('places buried candidates inside the envelope depth and clamps only candidates below that volume', () => {
    const grid = sourceVoxelGridForScan({
      bounds: { min: [-10, -10, -4], max: [10, 10, 4] },
      occupancyEnvelopeBounds: { min: [0, 0, 0], max: [0.2, 0.2, 0.2] },
      occupancySolverCandidateBelowSurfaceMeters: 2,
      occupancyVoxelSizeMeters: 0.2,
      occupancyFineVoxelSizeMeters: 0.1,
    });
    expect(grid?.origin).toEqual([0, 0, -2]);
    expect(grid?.voxelSizeMeters).toBe(0.1);
    const inside = buildSourceHighlightScene(
      [run({ source_position: { x_m: 0.05, y_m: 0.05, z_m: -1 } })],
      IDENTITY, registration(), grid, { currentScanId: 'scan-crc32:current' },
    );
    expect(inside.primary?.voxel).not.toBeNull();
    expect(inside.reason).toBeNull();

    const outside = buildSourceHighlightScene(
      [run({ source_position: { x_m: 0.05, y_m: 0.05, z_m: -2.5 } })],
      IDENTITY, registration(), grid, { currentScanId: 'scan-crc32:current' },
    );
    expect(outside.reason).toBe('outside_grid');
    expect(outside.primary?.voxel).toBeNull();
    expect(outside.primary?.edgeMarker).toEqual([0.05, 0.05, -1.95]);
  });

  it('uses rotated legacy standard deviations as a conservative 3D project-frame box', () => {
    const localFloorYaw90 = [0, -1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 0, 0, 0, 0, 1];
    const uncertain = run({
      source_position: { x_m: 1, y_m: 1, z_m: 1 },
      uncertainty: {
        std_m: { x_m: 0.1, y_m: 0.6, z_m: 0.2 },
        credible_level: 0.9,
        credible_volume_m3: 1,
        credible_area_m2: 0,
        credible_min: { x_m: Number.NaN, y_m: 0, z_m: 0 },
        credible_max: { x_m: 0, y_m: 0, z_m: 0 },
      },
    });
    const result = buildSourceHighlightScene(
      [uncertain], IDENTITY, registration(localFloorYaw90),
      { origin: [-3, -3, -3], dimensions: [30, 30, 30], voxelSizeMeters: 0.2 },
      { currentScanId: 'scan-crc32:current' },
    );
    const cells = result.uncertaintyVoxels;
    const xSpan = Math.max(...cells.map((cell) => cell.center[0])) - Math.min(...cells.map((cell) => cell.center[0]));
    const ySpan = Math.max(...cells.map((cell) => cell.center[1])) - Math.min(...cells.map((cell) => cell.center[1]));
    const zLayers = new Set(cells.map((cell) => cell.coordinates[2]));
    expect(result.primary?.positionProjectM).toEqual([-1, 1, 1]);
    expect(xSpan).toBeGreaterThan(ySpan);
    expect(zLayers.size).toBeGreaterThan(1);
  });

  it('rejects run geometry that differs from the registration and suppresses its uncertainty', () => {
    const result = buildSourceHighlightScene(
      [run({ geometry_computed_at_ms: 100, uncertainty: {
        std_m: { x_m: 1, y_m: 1, z_m: 1 }, credible_level: 0.9, credible_volume_m3: 1,
        credible_area_m2: 0, credible_min: { x_m: 0, y_m: 0, z_m: 0 }, credible_max: { x_m: 1, y_m: 1, z_m: 1 },
      } })],
      IDENTITY,
      registration(SENSOR_TO_LOCAL_FLOOR, 200),
      GRID,
      { currentScanId: 'scan-crc32:current' },
    );
    expect(result.reason).toBe('geometry_mismatch');
    expect(result.primary).toBeNull();
    expect(result.uncertaintyVoxels).toEqual([]);
    expect(result.staleRuns.map(({ reason }) => reason)).toEqual(['geometry_mismatch']);
  });

  it('keeps replaced-scan constrained runs in history while allowing matching unconstrained and current runs', () => {
    const previous = run({
      method_name: 'traveltime_scan_constrained', computed_at_ms: 1,
      scan_constraint_applied: true, scan_frame_id: 'scan-crc32:previous',
    });
    const legacy = run({ method_name: 'bayesian_scan_voids', computed_at_ms: 2 });
    const current = run({
      method_name: 'eikonal_scan_voids', computed_at_ms: 3,
      scan_constraint_applied: true, scan_frame_id: 'scan-crc32:current',
    });
    const unconstrained = run({ method_name: 'method_a', computed_at_ms: 4 });
    const result = buildSourceHighlightScene(
      [previous, legacy, current, unconstrained], IDENTITY, registration(), GRID,
      { currentScanId: 'scan-crc32:current' },
    );
    expect(result.primary?.run.method_name).toBe('method_a');
    expect(result.secondary.map(({ run: selected }) => selected.method_name)).toEqual(['eikonal_scan_voids']);
    expect(result.historyRuns.map(({ run: historical, currency }) => [historical.method_name, currency])).toEqual([
      ['traveltime_scan_constrained', 'history_previous_scan'],
      ['bayesian_scan_voids', 'history_scan_unknown'],
    ]);
  });

  it('fails closed with scan_unknown when the displayed scan id is omitted', () => {
    const result = buildSourceHighlightScene([run()], IDENTITY, registration(), GRID);
    expect(result.reason).toBe('scan_unknown');
    expect(result.primary).toBeNull();
  });
});
