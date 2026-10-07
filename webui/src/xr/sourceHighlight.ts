import type { LocalizationRun, LocalizationRunPosition } from '../api/deviceApi';
import { classifyRunCurrency } from '../domain/localizationOverlay';
import type { RegistrationArtifact } from './fiducial/registrationArtifact';
import { registrationApplicability, type RegistrationInapplicableReason } from './registrationApplicability';
import { localFloorMatrixToProject } from './projectFrame';
import { solverCandidateVolume } from './candidateVolume';
import { webXrLocalFloorToProject, type Vec3Tuple } from './scanViewportScene';
import {
  sensorSigmasToProjectHalfExtents,
  transformSensorBoxToProjectAabb,
  type ProjectAxisAlignedBox,
} from './uncertaintyTransform';

export interface SourceSessionIdentity {
  objectId: string;
  sessionId: string;
}

export interface SourceVoxelGrid {
  origin: Vec3Tuple;
  dimensions: readonly [number, number, number];
  voxelSizeMeters: number;
}

export interface SelectedLocalizationSource {
  run: LocalizationRun;
  primary: boolean;
}

export interface SourceVoxelCell {
  index: number;
  coordinates: readonly [number, number, number];
  center: Vec3Tuple;
}

export const MAX_SOURCE_MARKERS = 17;
export const MAX_SOURCE_UNCERTAINTY_VOXELS = 64;

export interface HighlightedLocalizationSource {
  run: LocalizationRun;
  positionProjectM: Vec3Tuple;
  primary: boolean;
  voxel: SourceVoxelCell | null;
  edgeMarker: Vec3Tuple | null;
}

export interface SourceHighlightScene {
  reason: RegistrationInapplicableReason | 'outside_grid' | null;
  primary: HighlightedLocalizationSource | null;
  uncertaintyVoxels: readonly SourceVoxelCell[];
  secondary: readonly HighlightedLocalizationSource[];
  voxelSizeMeters: number;
  historyRuns: readonly { run: LocalizationRun; currency: 'history_previous_scan' | 'history_scan_unknown' }[];
  staleRuns: readonly { run: LocalizationRun; reason: RegistrationInapplicableReason }[];
}

function finitePosition(position: LocalizationRunPosition | undefined): position is LocalizationRunPosition {
  return position !== undefined &&
    Number.isFinite(position.x_m) && Number.isFinite(position.y_m) && Number.isFinite(position.z_m);
}

function compareSourceQuality(left: LocalizationRun, right: LocalizationRun): number {
  return sourceConfidence(right) - sourceConfidence(left) ||
    right.computed_at_ms - left.computed_at_ms ||
    left.method_name.localeCompare(right.method_name);
}

function sourceConfidence(run: LocalizationRun): number {
  return run.confidence_score === undefined ? 0 : run.confidence_score;
}

function belongsToIdentity(run: LocalizationRun, identity: SourceSessionIdentity): boolean {
  return run.object_id === identity.objectId && run.session_id === identity.sessionId &&
    Number.isFinite(run.computed_at_ms) && typeof run.method_name === 'string' && run.method_name.length > 0;
}

function latestPerMethod(
  runs: readonly LocalizationRun[],
  identity: SourceSessionIdentity,
  currentScanId: string | null,
): LocalizationRun[] {
  const latest = new Map<string, LocalizationRun>();
  for (const run of runs) {
    if (!belongsToIdentity(run, identity) || classifyRunCurrency(run, currentScanId) === 'history_previous_scan' ||
        classifyRunCurrency(run, currentScanId) === 'history_scan_unknown') continue;
    const key = `${run.kind}/${run.method_name}`;
    const current = latest.get(key);
    if (current === undefined || run.computed_at_ms > current.computed_at_ms) latest.set(key, run);
  }
  return [...latest.values()].filter((run) => run.solved && finitePosition(run.source_position) &&
    Number.isFinite(sourceConfidence(run)) && sourceConfidence(run) >= 0 && sourceConfidence(run) <= 1);
}

/** Selects the current source per method; historical reruns never become extra markers. */
export function selectLocalizationSources(
  runs: readonly LocalizationRun[],
  identity: SourceSessionIdentity,
  currentScanId: string | null = null,
): SelectedLocalizationSource[] {
  const solved = latestPerMethod(runs, identity, currentScanId);
  const joint = solved.filter((run) => run.kind === 'joint_fusion' && run.uncertainty_too_high !== true)
    .sort((left, right) => right.computed_at_ms - left.computed_at_ms || compareSourceQuality(left, right))[0] ?? null;
  const method = solved.filter((run) => run.kind === 'individual_method')
    .sort(compareSourceQuality)[0] ?? null;
  const primary = joint ?? method;
  const ordered = [...solved].filter((run) => run !== primary).sort(compareSourceQuality);
  return [
    ...(primary === null ? [] : [{ run: primary, primary: true }]),
    ...ordered.map((run) => ({ run, primary: false })),
  ].slice(0, MAX_SOURCE_MARKERS);
}

function usableTransform(registration: RegistrationArtifact | null): number[] | null {
  const matrix = registration?.sensor_to_scan;
  return registration?.valid === true && matrix !== null && matrix !== undefined && matrix.length === 16 &&
    matrix.every(Number.isFinite) ? matrix : null;
}

/** Sensor frame -> registered WebXR local-floor frame -> project Z-up frame. */
export function sourcePositionToProject(
  position: LocalizationRunPosition,
  registration: RegistrationArtifact | null,
): Vec3Tuple | null {
  const matrix = usableTransform(registration);
  if (matrix === null || !finitePosition(position)) return null;
  const { x_m: x, y_m: y, z_m: z } = position;
  const localFloor = [
    matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
    matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
    matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
  ];
  if (!localFloor.every(Number.isFinite)) return null;
  return webXrLocalFloorToProject(localFloor[0], localFloor[1], localFloor[2]);
}

function validGrid(grid: SourceVoxelGrid): boolean {
  return Number.isFinite(grid.voxelSizeMeters) && grid.voxelSizeMeters > 0 &&
    grid.origin.every(Number.isFinite) && grid.dimensions.every((value) => Number.isInteger(value) && value > 0);
}

/** Lower grid faces are inclusive; the upper faces are exclusive. */
export function sourceToVoxelIndex(position: Vec3Tuple, grid: SourceVoxelGrid): number | null {
  if (!validGrid(grid) || !position.every(Number.isFinite)) return null;
  const coordinates = position.map((value, axis) => Math.floor((value - grid.origin[axis]) / grid.voxelSizeMeters));
  if (coordinates.some((value, axis) => value < 0 || value >= grid.dimensions[axis])) return null;
  const index = (coordinates[2] * grid.dimensions[1] + coordinates[1]) * grid.dimensions[0] + coordinates[0];
  return Number.isSafeInteger(index) && index <= 0xffff_ffff ? index : null;
}

export function voxelCell(index: number, grid: SourceVoxelGrid): SourceVoxelCell | null {
  if (!validGrid(grid) || !Number.isSafeInteger(index) || index < 0 || index > 0xffff_ffff) return null;
  const volume = grid.dimensions[0] * grid.dimensions[1] * grid.dimensions[2];
  if (!Number.isSafeInteger(volume) || index >= volume) return null;
  const x = index % grid.dimensions[0];
  const y = Math.floor(index / grid.dimensions[0]) % grid.dimensions[1];
  const z = Math.floor(index / (grid.dimensions[0] * grid.dimensions[1]));
  return {
    index,
    coordinates: [x, y, z],
    center: [
      grid.origin[0] + (x + 0.5) * grid.voxelSizeMeters,
      grid.origin[1] + (y + 0.5) * grid.voxelSizeMeters,
      grid.origin[2] + (z + 0.5) * grid.voxelSizeMeters,
    ],
  };
}

/** Returns capped project-frame uncertainty voxels; without a box it keeps the legacy XY ellipse. */
export function sourceUncertaintyVoxelIndices(
  position: Vec3Tuple,
  sigmaX: number | undefined,
  sigmaY: number | undefined,
  grid: SourceVoxelGrid,
  cap = MAX_SOURCE_UNCERTAINTY_VOXELS,
  sigmaZ = 0,
  projectBounds?: ProjectAxisAlignedBox,
): number[] {
  const sourceIndex = sourceToVoxelIndex(position, grid);
  if (sourceIndex === null || !Number.isSafeInteger(cap) || cap <= 0) return [];
  if (projectBounds !== undefined || sigmaZ > 0) {
    const bounds = projectBounds ?? (
      sigmaX !== undefined && sigmaY !== undefined && [sigmaX, sigmaY, sigmaZ].every((sigma) => Number.isFinite(sigma) && sigma >= 0)
        ? {
          min: [position[0] - sigmaX, position[1] - sigmaY, position[2] - sigmaZ] as Vec3Tuple,
          max: [position[0] + sigmaX, position[1] + sigmaY, position[2] + sigmaZ] as Vec3Tuple,
        }
        : undefined
    );
    return bounds === undefined ? [sourceIndex] : uncertaintyBoxVoxelIndices(bounds, sourceIndex, grid, cap);
  }
  if (sigmaX === undefined || sigmaY === undefined || !Number.isFinite(sigmaX) || !Number.isFinite(sigmaY) ||
      sigmaX <= 0 || sigmaY <= 0) return [sourceIndex];

  const sourceX = sourceIndex % grid.dimensions[0];
  const sourceY = Math.floor(sourceIndex / grid.dimensions[0]) % grid.dimensions[1];
  const origin = [grid.origin[0], grid.origin[1]] as const;
  const source = [position[0], position[1]] as const;
  const sigma = [sigmaX, sigmaY] as const;
  const dimension = [grid.dimensions[0], grid.dimensions[1]] as const;
  const sourceCell = [sourceX, sourceY] as const;
  const first: number[] = [];
  const last: number[] = [];
  for (let axis = 0; axis < 2; axis++) {
    const minimum = Math.ceil((source[axis] - sigma[axis] - origin[axis]) / grid.voxelSizeMeters - 0.5);
    const maximum = Math.floor((source[axis] + sigma[axis] - origin[axis]) / grid.voxelSizeMeters - 0.5);
    if (maximum < 0 || minimum >= dimension[axis]) return [];
    first[axis] = minimum <= 0 ? 0 : minimum;
    last[axis] = maximum >= dimension[axis] - 1 ? dimension[axis] - 1 : maximum;
    if (first[axis] > last[axis]) return [];
  }

  const sourceDx = (origin[0] + (sourceX + 0.5) * grid.voxelSizeMeters - position[0]) / sigmaX;
  const sourceDy = (origin[1] + (sourceY + 0.5) * grid.voxelSizeMeters - position[1]) / sigmaY;
  if (sourceDx * sourceDx + sourceDy * sourceDy > 1) return [];

  const maxRadius = Math.max(sourceX - first[0], last[0] - sourceX, sourceY - first[1], last[1] - sourceY);
  const indices: number[] = [];
  for (let radius = 0; radius <= maxRadius && indices.length < cap; radius++) {
    const xStart = Math.max(first[0], sourceX - radius);
    const xEnd = Math.min(last[0], sourceX + radius);
    const yStart = Math.max(first[1], sourceY - radius);
    const yEnd = Math.min(last[1], sourceY + radius);
    for (let y = yStart; y <= yEnd && indices.length < cap; y++) {
      for (let x = xStart; x <= xEnd && indices.length < cap; x++) {
        if (Math.max(Math.abs(x - sourceX), Math.abs(y - sourceY)) !== radius) continue;
        const centerX = origin[0] + (x + 0.5) * grid.voxelSizeMeters;
        const centerY = origin[1] + (y + 0.5) * grid.voxelSizeMeters;
        const dx = (centerX - position[0]) / sigmaX;
        const dy = (centerY - position[1]) / sigmaY;
        if (dx * dx + dy * dy <= 1) {
          const z = Math.floor(sourceIndex / (grid.dimensions[0] * grid.dimensions[1]));
          indices.push((z * grid.dimensions[1] + y) * grid.dimensions[0] + x);
        }
      }
    }
  }
  return indices;
}

function uncertaintyBoxVoxelIndices(
  bounds: ProjectAxisAlignedBox,
  sourceIndex: number,
  grid: SourceVoxelGrid,
  cap: number,
): number[] {
  if (!bounds.min.every(Number.isFinite) || !bounds.max.every(Number.isFinite) ||
      bounds.min.some((value, axis) => value > bounds.max[axis])) return [sourceIndex];
  const sourceCell = [
    sourceIndex % grid.dimensions[0],
    Math.floor(sourceIndex / grid.dimensions[0]) % grid.dimensions[1],
    Math.floor(sourceIndex / (grid.dimensions[0] * grid.dimensions[1])),
  ];
  const first = [0, 1, 2].map((axis) => Math.max(0, Math.ceil(
    (bounds.min[axis] - grid.origin[axis]) / grid.voxelSizeMeters - 0.5 - 1e-9,
  )));
  const last = [0, 1, 2].map((axis) => Math.min(grid.dimensions[axis] - 1, Math.floor(
    (bounds.max[axis] - grid.origin[axis]) / grid.voxelSizeMeters - 0.5 + 1e-9,
  )));
  if (first.some((value, axis) => value > last[axis])) return [sourceIndex];

  const maxRadius = Math.max(...[0, 1, 2].flatMap((axis) => [
    Math.abs(first[axis] - sourceCell[axis]), Math.abs(last[axis] - sourceCell[axis]),
  ]));
  const indices = [sourceIndex];
  const seen = new Set(indices);
  for (let radius = 0; radius <= maxRadius && indices.length < cap; radius += 1) {
    const xStart = Math.max(first[0], sourceCell[0] - radius);
    const xEnd = Math.min(last[0], sourceCell[0] + radius);
    const yStart = Math.max(first[1], sourceCell[1] - radius);
    const yEnd = Math.min(last[1], sourceCell[1] + radius);
    const zStart = Math.max(first[2], sourceCell[2] - radius);
    const zEnd = Math.min(last[2], sourceCell[2] + radius);
    for (let z = zStart; z <= zEnd && indices.length < cap; z += 1) {
      for (let y = yStart; y <= yEnd && indices.length < cap; y += 1) {
        for (let x = xStart; x <= xEnd && indices.length < cap; x += 1) {
          if (Math.max(Math.abs(x - sourceCell[0]), Math.abs(y - sourceCell[1]), Math.abs(z - sourceCell[2])) !== radius) continue;
          const index = (z * grid.dimensions[1] + y) * grid.dimensions[0] + x;
          if (seen.has(index)) continue;
          const center: Vec3Tuple = [
            grid.origin[0] + (x + 0.5) * grid.voxelSizeMeters,
            grid.origin[1] + (y + 0.5) * grid.voxelSizeMeters,
            grid.origin[2] + (z + 0.5) * grid.voxelSizeMeters,
          ];
          if (center.every((value, axis) => value >= bounds.min[axis] - 1e-9 && value <= bounds.max[axis] + 1e-9)) {
            indices.push(index);
            seen.add(index);
          }
        }
      }
    }
  }
  return indices;
}

function projectUncertaintyBounds(
  run: LocalizationRun,
  position: Vec3Tuple,
  registration: RegistrationArtifact,
): ProjectAxisAlignedBox {
  const uncertainty = run.uncertainty;
  if (uncertainty === undefined) return { min: position, max: position };
  const sensorToProject = localFloorMatrixToProject(registration.sensor_to_scan ?? []);
  const credibleMin = uncertainty.credible_min;
  const credibleMax = uncertainty.credible_max;
  let credibleBox: ProjectAxisAlignedBox | null = null;
  if (finitePosition(credibleMin) && finitePosition(credibleMax)) {
    credibleBox = transformSensorBoxToProjectAabb(
      [credibleMin.x_m, credibleMin.y_m, credibleMin.z_m],
      [credibleMax.x_m, credibleMax.y_m, credibleMax.z_m],
      sensorToProject,
    );
  }
  const standardDeviation = uncertainty.std_m;
  const extents = standardDeviation === undefined ? null : sensorSigmasToProjectHalfExtents(
    [standardDeviation.x_m, standardDeviation.y_m, standardDeviation.z_m], sensorToProject,
  );
  const sigmaBox: ProjectAxisAlignedBox | null = extents === null ? null : {
    min: [position[0] - extents[0], position[1] - extents[1], position[2] - extents[2]],
    max: [position[0] + extents[0], position[1] + extents[1], position[2] + extents[2]],
  };
  if (credibleBox !== null && sigmaBox !== null) return {
    min: [0, 1, 2].map((axis) => Math.min(credibleBox!.min[axis], sigmaBox.min[axis])) as unknown as Vec3Tuple,
    max: [0, 1, 2].map((axis) => Math.max(credibleBox!.max[axis], sigmaBox.max[axis])) as unknown as Vec3Tuple,
  };
  return credibleBox ?? sigmaBox ?? { min: position, max: position };
}

export function sourceVoxelGridForScan(snapshot: Pick<
  import('./webxrRuntime').ScanPointCloudSnapshot,
  'bounds' | 'occupancyEnvelopeBounds' | 'occupancySolverCandidateBelowSurfaceMeters' |
  'occupancyVoxelSizeMeters' | 'occupancyFineVoxelSizeMeters'
>): SourceVoxelGrid | null {
  const volume = solverCandidateVolume(snapshot);
  const { min: boundsMin, max: boundsMax, voxelSize: size } = volume;
  if (boundsMin === null || boundsMax === null || !Number.isFinite(size) || size <= 0) return null;
  const minimum = boundsMin.map((value) => Math.floor(value / size));
  const maximum = boundsMax.map((value) => Math.floor(value / size));
  const dimensions = maximum.map((value, axis) => value - minimum[axis] + 1);
  const grid: SourceVoxelGrid = {
    origin: [minimum[0] * size, minimum[1] * size, minimum[2] * size],
    dimensions: [dimensions[0], dimensions[1], dimensions[2]],
    voxelSizeMeters: size,
  };
  return validGrid(grid) ? grid : null;
}

function sourceEdgeMarker(position: Vec3Tuple, grid: SourceVoxelGrid): Vec3Tuple | null {
  if (!validGrid(grid)) return null;
  const coordinates = position.map((value, axis) => Math.max(0, Math.min(
    grid.dimensions[axis] - 1,
    Math.floor((value - grid.origin[axis]) / grid.voxelSizeMeters),
  )));
  const index = (coordinates[2] * grid.dimensions[1] + coordinates[1]) * grid.dimensions[0] + coordinates[0];
  return voxelCell(index, grid)?.center ?? null;
}

export function buildSourceHighlightScene(
  runs: readonly LocalizationRun[],
  identity: SourceSessionIdentity,
  registration: RegistrationArtifact | null,
  grid: SourceVoxelGrid | null,
  options: { currentScanId?: string | null } = {},
): SourceHighlightScene {
  const currentScanId = options.currentScanId ?? null;
  const historyRuns = runs.flatMap((run) => {
    if (!belongsToIdentity(run, identity)) return [];
    const currency = classifyRunCurrency(run, currentScanId);
    return currency === 'history_previous_scan' || currency === 'history_scan_unknown' ? [{ run, currency }] : [];
  });
  const selected = selectLocalizationSources(runs, identity, currentScanId);
  const inapplicable: Array<{ run: LocalizationRun; reason: RegistrationInapplicableReason }> = [];
  const applicable = selected.filter(({ run }) => {
    const expectedGeometry = Number.isFinite(run.geometry_computed_at_ms) ? run.geometry_computed_at_ms : null;
    const result = registrationApplicability(registration, { geometryComputedAtMs: expectedGeometry, scanFrameId: currentScanId });
    if (!result.ok) {
      inapplicable.push({ run, reason: result.reason });
      return false;
    }
    if (registration === null || !Number.isFinite(run.geometry_computed_at_ms) ||
        registration.geometry_computed_at_ms !== run.geometry_computed_at_ms) {
      inapplicable.push({ run, reason: 'geometry_mismatch' });
      return false;
    }
    return true;
  });
  const staleReason = inapplicable.find(({ run }) => selected.find((source) => source.primary)?.run === run)?.reason ??
    inapplicable[0]?.reason ?? null;
  const emptyScene = (reason: SourceHighlightScene['reason']): SourceHighlightScene => ({
    reason, primary: null, uncertaintyVoxels: [], secondary: [], voxelSizeMeters: 0, historyRuns, staleRuns: inapplicable,
  });
  if (selected.length === 0) return emptyScene(null);
  if (applicable.length === 0) return emptyScene(staleReason);
  if (grid === null || !validGrid(grid)) {
    return emptyScene(staleReason ?? 'outside_grid');
  }
  let outside = false;
  const transformed = applicable.flatMap((source) => {
    const position = sourcePositionToProject(source.run.source_position!, registration);
    if (position === null) return [];
    const index = sourceToVoxelIndex(position, grid);
    const voxel = index === null ? null : voxelCell(index, grid);
    const edgeMarker = voxel === null ? sourceEdgeMarker(position, grid) : null;
    outside ||= voxel === null;
    return [{ ...source, positionProjectM: position, voxel, edgeMarker }];
  });
  const primary = transformed.find((source) => source.primary) ?? null;
  const uncertaintyIndices = primary?.voxel === null || primary === null ? [] : sourceUncertaintyVoxelIndices(
    primary.positionProjectM,
    undefined,
    undefined,
    grid,
    MAX_SOURCE_UNCERTAINTY_VOXELS,
    0,
    projectUncertaintyBounds(primary.run, primary.positionProjectM, registration!),
  );
  return {
    reason: staleReason ?? (outside ? 'outside_grid' : null),
    primary,
    uncertaintyVoxels: uncertaintyIndices.flatMap((index) => {
      const cell = voxelCell(index, grid);
      return cell === null ? [] : [cell];
    }),
    secondary: transformed.filter((source) => !source.primary),
    voxelSizeMeters: grid.voxelSizeMeters,
    historyRuns,
    staleRuns: inapplicable,
  };
}
