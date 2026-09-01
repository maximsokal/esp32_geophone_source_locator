from pathlib import Path

p=Path('webui/src/xr/depthPointCloudV2.ts')
s=p.read_text()
old="""export interface LiveCoverageSnapshot {
"""
new="""export interface StructuralGuidePointCloudSnapshot {
  /** Promoted surface hypotheses only; no occupancy or robust-bounds work. */
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  pointCount: number;
  keyframeCount: number;
  voxelSizeMeters: number;
}

export interface LiveCoverageSnapshot {
"""
assert s.count(old)==1
s=s.replace(old,new,1)

old="""  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
"""
new="""  structuralGuideSnapshot(): StructuralGuidePointCloudSnapshot {
    // The live guide must not call the full final snapshot: final snapshot also
    // builds tentative arrays, support maps, robust sorted bounds and occupancy
    // in the wrapper. Here we copy only the promoted surfel state required by
    // the off-main-thread plane/corner detector.
    const count = this.promotedRepresentativeCount;
    const positions = new Float32Array(count * 3);
    const normals = new Float32Array(count * 3);
    const confidence = new Float32Array(count);
    let write = 0;
    for (const point of this.representatives.values()) {
      if (!point.promoted) continue;
      const offset = write * 3;
      positions[offset] = point.x;
      positions[offset + 1] = point.y;
      positions[offset + 2] = point.z;
      normals[offset] = point.normalX;
      normals[offset + 1] = point.normalY;
      normals[offset + 2] = point.normalZ;
      confidence[write] = representativeQuality(point);
      write += 1;
    }
    return {
      positions: write === count ? positions : positions.subarray(0, write * 3),
      normals: write === count ? normals : normals.subarray(0, write * 3),
      confidence: write === count ? confidence : confidence.subarray(0, write),
      pointCount: write,
      keyframeCount: this.integrationKeyframes,
      voxelSizeMeters: this.voxelSizeMeters,
    };
  }

  snapshot(depthFrameCount: number, sampledDepthFrames: number): ScanPointCloudSnapshot {
"""
assert s.count(old)==1
s=s.replace(old,new,1)
p.write_text(s)

p=Path('webui/src/xr/webxrRuntime.ts')
s=p.read_text()
old="""  getManualControlPoints(): readonly ManualControlPoint[] {
"""
new="""  getStructuralGuideSnapshot() {
    return this.pointCloud.structuralGuideSnapshot();
  }

  getManualControlPoints(): readonly ManualControlPoint[] {
"""
assert s.count(old)==1
s=s.replace(old,new,1)
p.write_text(s)

p=Path('webui/src/components/StructuralGuideOverlay.tsx')
s=p.read_text()
old="""    const snapshot = runtime.getPointCloudSnapshot();
    const snapshotMs = performance.now() - snapshotStarted;
    if (snapshot.positions.length < MIN_STABLE_POINTS * 3 || snapshot.normals.length !== snapshot.positions.length) return;

    const workerStarted = performance.now();
    void client.analyze({
      positions: snapshot.positions,
      normals: snapshot.normals,
      confidence: snapshot.confidence,
      keyframeCount: snapshot.integrationKeyframes,
      voxelSizeMeters: snapshot.voxelSizeMeters,
"""
new="""    const snapshot = runtime.getStructuralGuideSnapshot();
    const snapshotMs = performance.now() - snapshotStarted;
    if (snapshot.positions.length < MIN_STABLE_POINTS * 3 || snapshot.normals.length !== snapshot.positions.length) return;

    const workerStarted = performance.now();
    void client.analyze({
      positions: snapshot.positions,
      normals: snapshot.normals,
      confidence: snapshot.confidence,
      keyframeCount: snapshot.keyframeCount,
      voxelSizeMeters: snapshot.voxelSizeMeters,
"""
assert s.count(old)==1
s=s.replace(old,new,1)
p.write_text(s)
print('Patched lightweight guidance snapshots')
