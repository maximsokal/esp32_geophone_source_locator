"""Executable HIL gate; this test performs no hardware operation."""

import pytest


@pytest.mark.hil
def test_hil_requires_explicit_environment_gate(hil_enabled):
    assert hil_enabled
