import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVICE_MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "coverage-services.json"
ARCHIVE_MANIFEST = ROOT / "tests" / "host" / "manifest.d" / "coverage-object-archive.json"


def manifest_tests(path: Path) -> dict[str, dict]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    return {entry["name"]: entry for entry in manifest["tests"]}


def service_tests() -> dict[str, dict]:
    return manifest_tests(SERVICE_MANIFEST)


def test_storage_service_operations_can_reach_real_private_header_dependencies():
    include_dirs = service_tests()["storage_service_operations"]["include_dirs"]

    assert include_dirs[0] == "tests/host/stubs/storage_service_operations"
    assert "tests/host/stubs/storage_service" in include_dirs
    assert "components/storage_service/include" in include_dirs


def test_object_store_lifetime_prefers_public_api_before_lifetime_stubs():
    include_dirs = service_tests()["object_store_lifetime_barrier"]["include_dirs"]

    public_index = include_dirs.index("components/object_store/include")
    lifetime_stub_index = include_dirs.index("tests/host/stubs/object_store_lifetime")
    assert public_index < lifetime_stub_index
    assert "components/object_store/tests/stubs" in include_dirs


def test_object_create_transaction_failure_injection_is_in_host_coverage():
    archive = manifest_tests(ARCHIVE_MANIFEST)
    transaction = archive["object_store_create_transaction"]

    assert transaction["sources"] == [
        "components/object_store/tests/object_store_create_transaction_test.c"
    ]
    assert "components/object_store" in transaction["include_dirs"]
    assert "components/object_store/include" in transaction["include_dirs"]
    assert "components/object_store/tests/stubs" in transaction["include_dirs"]
