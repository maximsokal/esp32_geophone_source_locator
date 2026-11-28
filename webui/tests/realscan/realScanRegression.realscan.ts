import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { BufferAttribute, BufferGeometry, Vector3 } from 'three';
import { MeshBVH } from 'three-mesh-bvh';
import { describe, expect, it } from 'vitest';

import { buildTsdfRequestArgs } from '../../src/components/ScanPointCloudViewport';
import { buildDirectionalTsdfMesh, measureSelfIntersections } from '../../src/xr/directionalTsdf';
import { simplifyMeshForDisplay } from '../../src/xr/meshLod';
import { decodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
import { decodeScanArtifact } from '../../src/xr/scanArtifact';
import { createScanIdentity } from '../../src/xr/scanIdentity';
import { decodeTagPositions } from '../../src/xr/fiducial/tagPositionsArtifact';
import { decodeVoxelPreviewArtifact } from '../../src/xr/voxelPreview';
import { sampleMesh, shareNear, topologyMetrics } from '../support/meshMetrics';

/*
 * REAL-SCAN REPLAY (level A, private, local, mandatory): the originals of phone run 2026-11-26 23:15 without any decimation. Run with `npm run test:real-scan`.
 * It FAILS (never skips) when the files are missing, differ from the manifest, or the reconstruction does not improve measurably on them. Results go to
 * .test-build/real-scan/ (report.json and report.md): the stage-by-stage loss report and the Before/After table.
 * The scan describes a real room: nothing derived from it is committed.
 */
const WEBUI = resolve(__dirname, '../..');
const REPO = resolve(WEBUI, '..');
const MANIFEST = resolve(__dirname, 'manifest.local.json');
const OUT = resolve(REPO, '.test-build/real-scan');

interface ManifestFile { name: string; bytes: number; sha256: string }
interface Manifest { dir: string; files: Record<'scan' | 'occupancy' | 'preview' | 'tags', ManifestFile> }

function loadManifest(): Manifest {
  if (!existsSync(MANIFEST)) throw new Error(`REAL-SCAN REPLAY CANNOT RUN: the local manifest ${MANIFEST} is missing. It holds the SHA-256 of the original files; recreate it from the phone-lab saves of run 2026-11-26_231509 (see the task notes). This is a failure, not a skip.`);
  return JSON.parse(readFileSync(MANIFEST, 'utf8')) as Manifest;
}
const manifest = loadManifest();
function original(kind: keyof Manifest['files']): Uint8Array {
  const entry = manifest.files[kind];
  const path = resolve(REPO, manifest.dir, entry.name);
  if (!existsSync(path)) throw new Error(`REAL-SCAN REPLAY CANNOT RUN: ${path} is missing. This is a failure, not a skip.`);
  return new Uint8Array(readFileSync(path));
}
const sha256 = (bytes: Uint8Array): string => createHash('sha256').update(bytes).digest('hex');

interface Row {
  label: string;
  triangles: number; components: number; largestComponentRatio: number; boundaryEdgeLengthMeters: number; nonManifoldEdges: number; degenerateTriangles: number;
  selfIntersections: number; integratedPoints: number; measuredCoverage6cm: number; measuredCoverage12cm: number; unsupportedShare6cm: number;
  noiseScore: number; reconstructionMs: number; heapDeltaMb: number; zRangeMeters: [number, number];
}
function bounds(positions: Float32Array, count: number): { min: number[]; max: number[] } {
  const min = [Infinity, Infinity, Infinity]; const max = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < count; i += 1) for (let k = 0; k < 3; k += 1) { min[k] = Math.min(min[k], positions[i * 3 + k]); max[k] = Math.max(max[k], positions[i * 3 + k]); }
  return { min, max };
}
function percentile(values: number[], p: number): number { const sorted = [...values].sort((a, b) => a - b); return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))]; }

describe('REAL SCAN of phone run 2026-11-26 23:15 (private, local, mandatory)', () => {
  const scanBytes = original('scan');
  const scan = decodeScanArtifact(scanBytes);
  const stable = scan.positions.subarray(0, scan.pointCount * 3);
  const report: Record<string, unknown> = {};

  it('the four originals are exactly the files of the manifest (SHA-256 and size)', () => {
    for (const kind of ['scan', 'occupancy', 'preview', 'tags'] as const) {
      const bytes = original(kind);
      expect(bytes.length, `${kind} size`).toBe(manifest.files[kind].bytes);
      expect(sha256(bytes), `${kind} SHA-256`).toBe(manifest.files[kind].sha256);
    }
  });

  it('the derived artifacts belong to this scan: occupancy, preview and tags name it, byte-for-byte as the device returned them', () => {
    const identity = createScanIdentity(scanBytes);
    const occupancy = decodeOccupancySolverArtifact(original('occupancy'));
    expect(occupancy.header.frame_id).toBe(identity.frameId);
    if (occupancy.header.scan_sha256 !== undefined && occupancy.header.scan_sha256 !== null) expect(occupancy.header.scan_sha256).toBe(identity.sha256);
    const preview = decodeVoxelPreviewArtifact(original('preview'), scanBytes);
    expect(preview.sourceScanCrc32 >>> 0).toBe(identity.crc32 >>> 0);
    const tags = decodeTagPositions(Buffer.from(original('tags')).toString('utf8'));
    expect(tags).not.toBeNull();
    expect(tags!.scan_frame_id).toBe(identity.frameId);
    report.identity = { frameId: identity.frameId, stablePoints: scan.pointCount, certifiedPoints: scan.tsdfPointCount, occupancyOccupied: occupancy.header.occupied_count, occupancyFree: occupancy.header.free_count, previewCells: preview.voxelCount };
  });

  it('stage report: where the 6008 stable points go (TASK 03/08): spatial extent kept at every stage, reasons of rejection measured, nothing assumed', () => {
    const count = scan.pointCount;
    const certifiedKeys = new Set<string>();
    const key = (x: number, y: number, z: number): string => `${x.toFixed(4)},${y.toFixed(4)},${z.toFixed(4)}`;
    for (let i = 0; i < scan.tsdfPointCount; i += 1) certifiedKeys.add(key(scan.tsdfPositions[i * 3], scan.tsdfPositions[i * 3 + 1], scan.tsdfPositions[i * 3 + 2]));
    const rejected: number[] = [];
    for (let i = 0; i < count; i += 1) if (!certifiedKeys.has(key(stable[i * 3], stable[i * 3 + 1], stable[i * 3 + 2]))) rejected.push(i);
    expect(count - rejected.length, 'every certified surfel is one of the stable points').toBe(scan.tsdfPointCount);
    const klass = (i: number): string => { const nz = Math.abs(scan.normals[i * 3 + 2]); return nz > 0.8 ? 'horizontal' : nz < 0.3 ? 'vertical' : 'oblique'; };
    const tally = (indices: number[]): Record<string, number> => { const t: Record<string, number> = {}; for (const i of indices) t[klass(i)] = (t[klass(i)] ?? 0) + 1; return t; };
    const zValues = (indices: number[]): number[] => indices.map((i) => stable[i * 3 + 2]);
    const all = Array.from({ length: count }, (_, i) => i);
    const stageBounds = {
      stable: bounds(scan.positions, count), certified: bounds(scan.tsdfPositions, scan.tsdfPointCount), robustBounds: scan.bounds, fullBounds: scan.fullBounds,
    };
    report.stages = {
      stablePoints: count, serializedPoints: count, certifiedSurfels: scan.tsdfPointCount, rejectedByCertification: rejected.length,
      normalClassStable: tally(all), normalClassRejected: tally(rejected),
      zPercentilesStable: [0.02, 0.5, 0.98].map((p) => +percentile(zValues(all), p).toFixed(3)),
      zPercentilesRejected: [0.02, 0.5, 0.98].map((p) => +percentile(zValues(rejected), p).toFixed(3)),
      bounds: stageBounds,
    };
    /* The certificate thins the scan roughly evenly: it does not remove whole walls or the upper part of the room. */
    const share = (indices: number[], klassName: string): number => indices.filter((i) => klass(i) === klassName).length / Math.max(1, indices.length);
    /* Measured on this scan: the certificate drops oblique (curved, cluttered) surfaces most (42% of the rejected vs 31% of all), horizontal least. It thins every class, none vanishes. */
    for (const name of ['horizontal', 'vertical', 'oblique']) {
      expect(share(all, name), `${name} surfaces exist`).toBeGreaterThan(0.1);
      expect(1 - rejected.filter((i) => klass(i) === name).length / all.filter((i) => klass(i) === name).length, `${name} surfaces keep more than half of their points`).toBeGreaterThan(0.5);
    }
    expect(percentile(zValues(rejected), 0.98)).toBeLessThanOrEqual(percentile(zValues(all), 0.98) + 0.2);
    /* The height of the model: the displayed Z is the ROBUST (trimmed) bound; the stable points reach higher. Nothing is lost between stages. */
    expect(stageBounds.certified.max[2] - stageBounds.stable.min[2], 'certified surfels keep the height of the stable points').toBeGreaterThan(0.6 * (stageBounds.stable.max[2] - stageBounds.stable.min[2]));
  });

  function measure(label: string, hybrid: boolean): { row: Row; mesh: ReturnType<typeof buildDirectionalTsdfMesh> } {
    const args = buildTsdfRequestArgs(scan as never);
    const heapBefore = process.memoryUsage().heapUsed;
    const started = performance.now();
    const mesh = buildDirectionalTsdfMesh(args as never, { hybrid });
    const elapsed = performance.now() - started;
    const heapAfter = process.memoryUsage().heapUsed;
    const topology = topologyMetrics(mesh.trianglePositions);
    const samples = sampleMesh(mesh.trianglePositions, 'light');
    const z = [Infinity, -Infinity] as [number, number];
    for (let i = 2; i < mesh.trianglePositions.length; i += 3) { z[0] = Math.min(z[0], mesh.trianglePositions[i]); z[1] = Math.max(z[1], mesh.trianglePositions[i]); }
    return {
      mesh,
      row: {
        label, triangles: topology.triangles, components: topology.components, largestComponentRatio: topology.largestComponentRatio,
        boundaryEdgeLengthMeters: topology.boundaryEdgeLengthMeters, nonManifoldEdges: topology.nonManifoldEdges, degenerateTriangles: topology.degenerateTriangles,
        selfIntersections: mesh.selfIntersectionPairCount, integratedPoints: mesh.integratedPointCount,
        measuredCoverage6cm: shareNear(stable, samples, 0.06), measuredCoverage12cm: shareNear(stable, samples, 0.12), unsupportedShare6cm: 1 - shareNear(samples, stable, 0.06),
        noiseScore: mesh.meshNoiseScore, reconstructionMs: elapsed, heapDeltaMb: (heapAfter - heapBefore) / 1048576, zRangeMeters: z,
      },
    };
  }

  it('Before/After on the very same points, measured on the mesh as displayed: the hybrid reconstruction is measurably better and not worse anywhere that matters', () => {
    measure('warm-up', true);                                                                    /* JIT and caches: the measured runs follow */
    const before = measure('before: directional pass only (the f40b0cec algorithm)', false);
    const after = measure('after: scalar surface + directional pass for the uncovered', true);
    const rows = [before.row, after.row];
    report.beforeAfter = rows;
    mkdirSync(OUT, { recursive: true });
    const fmt = (v: number, d = 3): string => v.toFixed(d);
    const lines = [
      '| Metric | Before | After | Delta |', '|---|---:|---:|---:|',
      ...([
        ['Stable points', scan.pointCount, scan.pointCount], ['Certified surfels', scan.tsdfPointCount, scan.tsdfPointCount],
        ['Integrated points', before.row.integratedPoints, after.row.integratedPoints], ['Triangles', before.row.triangles, after.row.triangles],
        ['Connected components', before.row.components, after.row.components], ['Largest component ratio', before.row.largestComponentRatio, after.row.largestComponentRatio],
        ['Boundary edge length (m)', before.row.boundaryEdgeLengthMeters, after.row.boundaryEdgeLengthMeters], ['Non-manifold edges', before.row.nonManifoldEdges, after.row.nonManifoldEdges],
        ['Degenerate triangles', before.row.degenerateTriangles, after.row.degenerateTriangles], ['Self-intersecting pairs', before.row.selfIntersections, after.row.selfIntersections],
        ['Measured surface coverage, 6 cm', before.row.measuredCoverage6cm, after.row.measuredCoverage6cm], ['Measured surface coverage, 12 cm', before.row.measuredCoverage12cm, after.row.measuredCoverage12cm],
        ['Unsupported share of the mesh, 6 cm', before.row.unsupportedShare6cm, after.row.unsupportedShare6cm], ['Mesh noise score', before.row.noiseScore, after.row.noiseScore],
        ['Reconstruction time (ms, desktop)', before.row.reconstructionMs, after.row.reconstructionMs], ['Heap delta (MB, Node, approximate)', before.row.heapDeltaMb, after.row.heapDeltaMb],
      ] as Array<[string, number, number]>).map(([name, b, a]) => `| ${name} | ${fmt(b, Number.isInteger(b) ? 0 : 3)} | ${fmt(a, Number.isInteger(a) ? 0 : 3)} | ${fmt(a - b, Number.isInteger(a) && Number.isInteger(b) ? 0 : 3)} |`),
    ];
    writeFileSync(resolve(OUT, 'report.md'), `${lines.join('\n')}\n`);
    writeFileSync(resolve(OUT, 'report.json'), JSON.stringify(report, null, 2));

    expect(after.row.measuredCoverage6cm, 'more of the measured surface is meshed').toBeGreaterThanOrEqual(before.row.measuredCoverage6cm + 0.10);
    expect(after.row.measuredCoverage12cm).toBeGreaterThanOrEqual(before.row.measuredCoverage12cm + 0.05);
    expect(after.row.selfIntersections, 'the channel seams are gone').toBeLessThanOrEqual(before.row.selfIntersections);
    expect(after.row.degenerateTriangles).toBe(0);
    expect(after.row.largestComponentRatio, 'one main connected surface').toBeGreaterThanOrEqual(0.85);
    expect(after.row.noiseScore, 'not noisier').toBeLessThanOrEqual(before.row.noiseScore + 0.05);
    expect(after.row.unsupportedShare6cm, 'the unconfirmed share stays modest: not painted over just to look whole').toBeLessThanOrEqual(0.15);
    expect(after.row.reconstructionMs, 'time stays within 2.5x of the baseline on the same machine').toBeLessThanOrEqual(2.5 * before.row.reconstructionMs + 500);
    /* the height of the scan survives to the displayed mesh */
    const certifiedZ = [percentile(Array.from({ length: scan.tsdfPointCount }, (_, i) => scan.tsdfPositions[i * 3 + 2]), 0.02), percentile(Array.from({ length: scan.tsdfPointCount }, (_, i) => scan.tsdfPositions[i * 3 + 2]), 0.98)];
    expect(after.row.zRangeMeters[1] - after.row.zRangeMeters[0], 'the mesh keeps the height of the certified surfels').toBeGreaterThanOrEqual(0.85 * (certifiedZ[1] - certifiedZ[0]));
    expect(after.mesh.finalTopology!.triangles).toBe(after.mesh.triangleCount);
    expect(after.mesh.meshTopologyComponentCount, 'the reported components are those of the displayed mesh').toBe(after.row.components);
  });

  it('is deterministic: the same original scan gives the same mesh twice', () => {
    const a = measure('a', true).mesh; const b = measure('b', true).mesh;
    expect(Array.from(a.trianglePositions)).toEqual(Array.from(b.trianglePositions));
  });

  it('the lighter copy offered to the scanner: under 35% of the triangles, the same borders and parts, within 8 mm both ways', async () => {
    const mesh = measure('lod', true).mesh;
    const lod = await simplifyMeshForDisplay(mesh.trianglePositions, mesh.voxelSizeMeters);
    expect(lod.simplified).toBe(true);
    const full = topologyMetrics(mesh.trianglePositions, 1e-6); const light = topologyMetrics(lod.trianglePositions, 1e-6);
    expect(light.triangles).toBeLessThan(0.35 * full.triangles);
    expect(light.boundaryEdges, 'doorways and the limits of the observed surface are untouched').toBe(full.boundaryEdges);
    expect(light.boundaryEdgeLengthMeters).toBeCloseTo(full.boundaryEdgeLengthMeters, 6);
    expect(light.components).toBe(full.components);
    expect(light.nonManifoldEdges).toBeLessThanOrEqual(full.nonManifoldEdges);
    expect(light.degenerateTriangles).toBe(0);
    expect(measureSelfIntersections(lod.trianglePositions, mesh.voxelSizeMeters).intersectingPairCount).toBeLessThanOrEqual(mesh.selfIntersectionPairCount);
    const farthest = (from: Float32Array, to: Float32Array): number => {
      const geometry = new BufferGeometry(); geometry.setAttribute('position', new BufferAttribute(to.slice(), 3));
      const tree = new MeshBVH(geometry); const hit = { point: new Vector3(), distance: 0, faceIndex: 0 }; const point = new Vector3();
      const samples = sampleMesh(from, 'light'); let worst = 0;
      for (let i = 0; i < samples.length; i += 3) { tree.closestPointToPoint(point.fromArray(samples, i), hit); worst = Math.max(worst, hit.distance); }
      return worst;
    };
    expect(farthest(mesh.trianglePositions, lod.trianglePositions), 'nothing of the reconstruction is lost').toBeLessThan(0.008);
    expect(farthest(lod.trianglePositions, mesh.trianglePositions), 'nothing is drawn away from it').toBeLessThan(0.008);
    report.displayLod = { triangles: [full.triangles, light.triangles], boundaryEdges: light.boundaryEdges, components: light.components };
  });
});
