from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


# BoundedSpatialPointIndex returns nearest-first. The legacy endpoint solver fed
# robust RANSAC in source-index order, and its deterministic triplets depend on
# that ordering. Restore only the endpoint caller's legacy order while keeping
# the O(local-neighbourhood) spatial pruning.
replace_once(
    'webui/src/xr/structuralEdgeEndpoints.ts',
    '    for (const index of spatialIndex.queryRadius(seed.position, LOCAL_RADIUS_METERS)) {',
    '''    const localIndices = [...spatialIndex.queryRadius(seed.position, LOCAL_RADIUS_METERS)]\n      .sort((left, right) => left - right);\n    for (const index of localIndices) {''',
    'endpoint spatial index source order',
)

# The endpoint regression was caused before interval construction, so retain the
# original 10/90 robust interval rather than changing physical endpoint semantics.
replace_once(
    'webui/src/xr/structuralEdgeEndpoints.ts',
    '    const firstLow = quantile(firstValues, 0.05);\n    const firstHigh = quantile(firstValues, 0.95);\n    const secondLow = quantile(secondValues, 0.05);\n    const secondHigh = quantile(secondValues, 0.95);',
    '    const firstLow = quantile(firstValues, 0.10);\n    const firstHigh = quantile(firstValues, 0.90);\n    const secondLow = quantile(secondValues, 0.10);\n    const secondHigh = quantile(secondValues, 0.90);',
    'restore endpoint robust interval',
)

# The GP3 preview can propose many locally valid fan triangles whose shared edges
# are already saturated by two faces. Preventing a third face at generation time
# preserves measured local triangles while avoiding the huge non-manifold repair
# cascade that previously fragmented a healthy planar surface into dozens of
# quarantined islands. The later topology sanitizer remains fully enabled.
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''function key(x: number, y: number, z: number): string {\n  return `${x}:${y}:${z}`;\n}\n''',
    '''function key(x: number, y: number, z: number): string {\n  return `${x}:${y}:${z}`;\n}\n\nfunction meshEdgeKey(a: number, b: number): string {\n  return a < b ? `${a}:${b}` : `${b}:${a}`;\n}\n''',
    'mesh edge key helper',
)
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''  const triangles: number[] = [];\n  const emitted = new Set<string>();\n  let geometryRejectedTriangleCount = 0;\n''',
    '''  const triangles: number[] = [];\n  const emitted = new Set<string>();\n  const emittedEdgeUseCount = new Map<string, number>();\n  let geometryRejectedTriangleCount = 0;\n  let manifoldRejectedTriangleCount = 0;\n''',
    'mesh generation edge budget state',
)
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''      // normalsCompatible is intentionally sign-invariant because fused surfel\n      // normals may arrive with either sign. For actual mesh winding we still\n      // choose the sign that agrees with the measured center surfel. The topology\n      // pass below then propagates consistent winding across shared manifold edges.\n      if (dotVector(triNormal, center.normal) < 0) {\n        triangles.push(...center.position, ...second.position, ...first.position);\n      } else {\n        triangles.push(...center.position, ...first.position, ...second.position);\n      }\n      if (triangles.length / 9 >= MESH_MAX_TRIANGLES) break;\n''',
    '''      const edgeKeys = [\n        meshEdgeKey(center.index, first.index),\n        meshEdgeKey(first.index, second.index),\n        meshEdgeKey(second.index, center.index),\n      ] as const;\n      if (edgeKeys.some((edgeKey) => (emittedEdgeUseCount.get(edgeKey) ?? 0) >= 2)) {\n        manifoldRejectedTriangleCount += 1;\n        continue;\n      }\n\n      // normalsCompatible is intentionally sign-invariant because fused surfel\n      // normals may arrive with either sign. For actual mesh winding we still\n      // choose the sign that agrees with the measured center surfel. The topology\n      // pass below then propagates consistent winding across shared manifold edges.\n      if (dotVector(triNormal, center.normal) < 0) {\n        triangles.push(...center.position, ...second.position, ...first.position);\n      } else {\n        triangles.push(...center.position, ...first.position, ...second.position);\n      }\n      for (const edgeKey of edgeKeys) {\n        emittedEdgeUseCount.set(edgeKey, (emittedEdgeUseCount.get(edgeKey) ?? 0) + 1);\n      }\n      if (triangles.length / 9 >= MESH_MAX_TRIANGLES) break;\n''',
    'mesh generation manifold gate',
)
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''    rejectedTriangleCount:\n      geometryRejectedTriangleCount +\n      topology.quarantinedTriangleCount +\n      topology.discardedTriangleCount,''',
    '''    rejectedTriangleCount:\n      geometryRejectedTriangleCount +\n      manifoldRejectedTriangleCount +\n      topology.quarantinedTriangleCount +\n      topology.discardedTriangleCount,''',
    'mesh rejection diagnostics',
)

# Guard the generator regression directly: a healthy regular plane must enter the
# sanitizer already manifold instead of relying on destructive repair.
replace_once(
    'webui/tests/xr/meshPreview.test.ts',
    '''    expect(mesh.meshReadiness).toBe(1);\n    expect(mesh.triangleCount).toBeGreaterThan(100);''',
    '''    expect(mesh.meshReadiness).toBe(1);\n    expect(mesh.nonManifoldEdgeCount).toBe(0);\n    expect(mesh.triangleCount).toBeGreaterThan(100);''',
    'planar mesh manifold regression',
)
