#!/usr/bin/env python3
"""Fetch the locked production Button component for clean-checkout host tests.

Run with idf-component-manager==2.4.11 installed. No IDF toolchain, dependency
resolution, lockfile updates or other managed components are needed.
"""

import argparse
from pathlib import Path

from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir
from idf_component_tools.lock import LockManager
from idf_component_tools.sources.fetcher import ComponentFetcher


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--managed-dir", type=Path, default=repo / "managed_components")
    args = parser.parse_args()
    component = LockManager(repo / "dependencies.lock").load().solved_components[
        "espressif/button"
    ]
    destination = args.managed_dir / "espressif__button"
    if not destination.exists():
        downloaded = ComponentFetcher(component, args.managed_dir).download()
        if downloaded is None:
            raise RuntimeError("The locked Button component was not downloaded")
    # Verify existing and downloaded sources against the lock, rather than
    # trusting a version string or overwriting a locally modified component.
    validate_hash_eq_hashdir(destination, component.component_hash)
    print(f"Host Button ready: {component.version}, hash={component.component_hash}")


if __name__ == "__main__":
    main()
