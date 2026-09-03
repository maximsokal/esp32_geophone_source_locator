from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    file = Path(path)
    text = file.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected 1 match, got {count}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


# Remove radially shadowed neighbours BEFORE angular fan ordering. A farther point
# on the same ray used to sit between two valid angular neighbours, so the later
# triangle-level Gabriel test rejected both wedges and left a sparse disconnected
# mesh. Center-to-neighbour Gabriel filtering keeps only local Delaunay candidates
# in the fan; the remaining opposite edge is checked per triangle.
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''    const localNeighbours = neighbours\n      .filter((neighbour) => neighbour.distance <= localRadiusMeters);\n    const selected = localNeighbours.slice(0, MESH_MAX_NEIGHBOURS);\n    // Bounded pool for the empty-diameter edge test. Twice the fan budget is\n    // enough to see closer blockers while keeping mobile work deterministic.\n    const gabrielPool = [center, ...localNeighbours.slice(0, MESH_MAX_NEIGHBOURS * 2).map((item) => item.point)];\n    selected.sort((left, right) => left.angle - right.angle || left.point.index - right.point.index);''',
    '''    const localNeighbours = neighbours\n      .filter((neighbour) => neighbour.distance <= localRadiusMeters);\n    // Bounded pool for the empty-diameter edge test. Twice the fan budget is\n    // enough to see closer blockers while keeping mobile work deterministic.\n    const gabrielPool = [center, ...localNeighbours.slice(0, MESH_MAX_NEIGHBOURS * 2).map((item) => item.point)];\n    const selected = localNeighbours\n      .slice(0, MESH_MAX_NEIGHBOURS)\n      .filter((neighbour) => gabrielEdgeAllowed(center, neighbour.point, gabrielPool));\n    selected.sort((left, right) => left.angle - right.angle || left.point.index - right.point.index);''',
    'pre-filter radial fan neighbours',
)
replace_once(
    'webui/src/xr/meshPreview.ts',
    '''      if (\n        !gabrielEdgeAllowed(center, first, gabrielPool) ||\n        !gabrielEdgeAllowed(first, second, gabrielPool) ||\n        !gabrielEdgeAllowed(second, center, gabrielPool)\n      ) {''',
    '''      if (!gabrielEdgeAllowed(first, second, gabrielPool)) {''',
    'triangle opposite Gabriel edge',
)
