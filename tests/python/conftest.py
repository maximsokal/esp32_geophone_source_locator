"""Pytest policy shared by deterministic tests and future HIL tests."""

import os

import pytest


def pytest_addoption(parser):
    parser.addoption(
        "--hil",
        action="store_true",
        default=False,
        help="enable tests marked hil; GEOLOCATOR_HIL=1 is still required",
    )


def pytest_collection_modifyitems(config, items):
    if config.getoption("--hil"):
        return
    skip_hil = pytest.mark.skip(reason="HIL disabled; pass --hil explicitly")
    for item in items:
        if "hil" in item.keywords:
            item.add_marker(skip_hil)


@pytest.fixture
def hil_enabled(request):
    """Gate every future fixture that could access physical hardware."""

    if not request.config.getoption("--hil"):
        pytest.fail("HIL access requires --hil")
    if os.environ.get("GEOLOCATOR_HIL") != "1":
        pytest.fail("HIL access requires GEOLOCATOR_HIL=1")
    return True
