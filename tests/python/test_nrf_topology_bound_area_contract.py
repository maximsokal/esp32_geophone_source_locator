from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVICE = ROOT / "components" / "nrf_topology_service" / "nrf_topology_service.c"


def _function(source: str, signature: str) -> str:
    start = source.index(signature)
    end = source.index("\n}\n", start)
    return source[start:end]


def test_missing_edge_bounds_are_cut_by_the_cables_before_they_are_logged():
    source = SERVICE.read_text(encoding="utf-8")
    body = _function(source, "static bool service_ranging_check_triangles(")
    bound_log = body.index('"RANGING_BOUND')
    cap_call = body.index("nrf_geometry_edge_bounds_cap(&bounds")
    cable_cap = body.index("nrf_topology_edge_cable_cap_mm(")
    # The cap derives from the cables of both ends and is applied to the interval that is then logged.
    assert cable_cap < cap_call < bound_log
    assert "service_node_cable_limit_mm(worker, pair->node_a)" in body
    assert "service_node_cable_limit_mm(worker, pair->node_b)" in body


def test_footprint_is_logged_from_the_solved_layout_and_never_rejects():
    source = SERVICE.read_text(encoding="utf-8")
    body = _function(source, "static void service_ranging_log_footprint(")
    assert "nrf_geometry_hull_surface_area_mm2(x, y, z, count)" in body
    assert "nrf_geometry_max_hull_surface_area_mm2(count, radius_mm)" in body
    assert "RANGING_AREA" in body
    # 3D: the barometric Z of every placed node goes in beside the Bluetooth X and Y.
    assert "z[count] = node->z_mm;" in body
    # Diagnostic only: no failure path, no result change.
    assert "service_ranging_finish" not in body and "success = false" not in body and "return false" not in body
    caller = _function(source, "static bool service_ranging_check_triangles(")
    assert "service_ranging_log_footprint(worker);" in caller
    # Only nodes the solver actually placed take part.
    assert "if (!node->solved) continue;" in body
