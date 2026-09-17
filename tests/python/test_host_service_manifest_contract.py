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


def test_storage_service_operations_uses_one_real_component_api_surface():
    include_dirs = service_tests()["storage_service_operations"]["include_dirs"]

    assert include_dirs[:3] == [
        "components/storage_service",
        "components/storage_service/include",
        "components/sd_card/include",
    ]
    assert "tests/host/stubs/storage_service_operations" in include_dirs
    assert "tests/host/stubs/storage_service" in include_dirs

    # The dependency-stub directory also contains a legacy private-header stub.
    # Keeping the real component/private/public roots first prevents one TU from
    # mixing that reduced declaration set with the production public API.
    assert include_dirs.index("components/storage_service") < include_dirs.index(
        "tests/host/stubs/storage_service_operations"
    )


def test_object_store_lifetime_uses_real_object_and_format_apis():
    include_dirs = service_tests()["object_store_lifetime_barrier"]["include_dirs"]

    assert include_dirs[:3] == [
        "components/object_store",
        "components/object_store/include",
        "components/object_format/include",
    ]
    assert "components/object_store/tests/stubs" in include_dirs
    assert "tests/host/stubs/object_store_lifetime" not in include_dirs


def test_object_create_transaction_failure_injection_is_in_host_coverage():
    archive = manifest_tests(ARCHIVE_MANIFEST)
    transaction = archive["object_store_create_transaction"]

    assert transaction["sources"] == [
        "components/object_store/tests/object_store_create_transaction_test.c"
    ]
    assert "components/object_store" in transaction["include_dirs"]
    assert "components/object_store/include" in transaction["include_dirs"]
    assert "components/object_store/tests/stubs" in transaction["include_dirs"]
