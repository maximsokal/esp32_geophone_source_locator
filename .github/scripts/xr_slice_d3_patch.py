from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


# The spatial hash itself preserves the exact radius membership. Revert the
# diagnostic source-order experiment: plane extraction was failing because its
# endpoint-specific tangent-spread threshold treated PCA sigma as physical width.
replace_once(
    'webui/src/xr/structuralEdgeEndpoints.ts',
    '''    const localIndices = [...spatialIndex.queryRadius(seed.position, LOCAL_RADIUS_METERS)]\n      .sort((left, right) => left - right);\n    for (const index of localIndices) {''',
    '    for (const index of spatialIndex.queryRadius(seed.position, LOCAL_RADIUS_METERS)) {',
    'revert endpoint source-order experiment',
)
replace_once(
    'webui/src/xr/structuralEdgeEndpoints.ts',
    '      minimumTangentSpreadMeters: clamp(snapshot.voxelSizeMeters * 0.55, 0.020, 0.032),',
    '''      // RobustPlane.tangentSpreadMeters is PCA sigma, not full patch width.\n      // A ~36 mm measured strip has sigma ~14.7 mm; requiring 20-22 mm sigma\n      // accidentally rejected legitimate long two-plane edges before the\n      // explicit 9 cm edge-scale certificate could run. Keep a real width floor\n      // here, but express it in the same sigma scale as the fitter.\n      minimumTangentSpreadMeters: clamp(snapshot.voxelSizeMeters * 0.35, 0.014, 0.024),''',
    'endpoint tangent sigma threshold',
)

# Local Gabriel edges form a planar subgraph of a Delaunay triangulation on a
# planar patch. Applying this bounded empty-diameter test before triangle emission
# removes the crossing long fan diagonals that produced hundreds of false
# self-intersections on a perfectly regular measured plane. Co-circular ties are
# resolved deterministically by choosing one of the two competing diagonals.
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''function meshEdgeKey(a: number, b: number): string {\n  return a < b ? `${a}:${b}` : `${b}:${a}`;\n}\n''',
    '''function meshEdgeKey(a: number, b: number): string {\n  return a < b ? `${a}:${b}` : `${b}:${a}`;\n}\n\nfunction compareMeshEdges(a0: number, a1: number, b0: number, b1: number): number {\n  const amin = Math.min(a0, a1), amax = Math.max(a0, a1);\n  const bmin = Math.min(b0, b1), bmax = Math.max(b0, b1);\n  return amin - bmin || amax - bmax;\n}\n\nfunction gabrielEdgeAllowed(\n  first: PointRecord,\n  second: PointRecord,\n  localPool: readonly PointRecord[],\n): boolean {\n  const dx = second.position[0] - first.position[0];\n  const dy = second.position[1] - first.position[1];\n  const dz = second.position[2] - first.position[2];\n  const edgeLengthSquared = dx * dx + dy * dy + dz * dz;\n  if (!Number.isFinite(edgeLengthSquared) || edgeLengthSquared <= 1e-10) return false;\n  const mx = (first.position[0] + second.position[0]) * 0.5;\n  const my = (first.position[1] + second.position[1]) * 0.5;\n  const mz = (first.position[2] + second.position[2]) * 0.5;\n  const radiusSquared = edgeLengthSquared * 0.25;\n  const tolerance = Math.max(1e-10, radiusSquared * 1e-5);\n  const boundary: number[] = [];\n  for (const candidate of localPool) {\n    if (candidate.index === first.index || candidate.index === second.index) continue;\n    const px = candidate.position[0] - mx;\n    const py = candidate.position[1] - my;\n    const pz = candidate.position[2] - mz;\n    const squared = px * px + py * py + pz * pz;\n    if (squared < radiusSquared - tolerance) return false;\n    if (Math.abs(squared - radiusSquared) <= tolerance) boundary.push(candidate.index);\n  }\n  if (boundary.length >= 2) {\n    boundary.sort((left, right) => left - right);\n    // Four co-circular samples admit two Delaunay diagonals. Choose one globally\n    // by source-index edge order so adjacent center fans cannot choose both.\n    if (compareMeshEdges(first.index, second.index, boundary[0], boundary[1]) > 0) return false;\n  }\n  return true;\n}\n''',
    'Gabriel edge helper',
)
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''    const selected = neighbours\n      .filter((neighbour) => neighbour.distance <= localRadiusMeters)\n      .slice(0, MESH_MAX_NEIGHBOURS);\n    selected.sort((left, right) => left.angle - right.angle || left.point.index - right.point.index);''',
    '''    const localNeighbours = neighbours\n      .filter((neighbour) => neighbour.distance <= localRadiusMeters);\n    const selected = localNeighbours.slice(0, MESH_MAX_NEIGHBOURS);\n    // Bounded pool for the empty-diameter edge test. Twice the fan budget is\n    // enough to see closer blockers while keeping mobile work deterministic.\n    const gabrielPool = [center, ...localNeighbours.slice(0, MESH_MAX_NEIGHBOURS * 2).map((item) => item.point)];\n    selected.sort((left, right) => left.angle - right.angle || left.point.index - right.point.index);''',
    'bounded Gabriel pool',
)
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''      if (!triangleGeometryAcceptable(center.position, first.position, second.position, localRadiusMeters)) {\n        geometryRejectedTriangleCount += 1;\n        continue;\n      }\n      const triNormal = triangleNormal(center.position, first.position, second.position);''',
    '''      if (!triangleGeometryAcceptable(center.position, first.position, second.position, localRadiusMeters)) {\n        geometryRejectedTriangleCount += 1;\n        continue;\n      }\n      if (\n        !gabrielEdgeAllowed(center, first, gabrielPool) ||\n        !gabrielEdgeAllowed(first, second, gabrielPool) ||\n        !gabrielEdgeAllowed(second, center, gabrielPool)\n      ) {\n        geometryRejectedTriangleCount += 1;\n        continue;\n      }\n      const triNormal = triangleNormal(center.position, first.position, second.position);''',
    'Gabriel triangle gate',
)
