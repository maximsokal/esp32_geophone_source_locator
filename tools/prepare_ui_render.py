#!/usr/bin/env python3
"""Fetch/hash-check only the locked LVGL for native UI tests (manager 2.4.11)."""
import argparse
from pathlib import Path
import shutil
import tempfile


def prepare(managed_dir: Path) -> None:
    from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir
    from idf_component_tools.lock import LockManager
    from idf_component_tools.sources.fetcher import ComponentFetcher

    repo = Path(__file__).resolve().parents[1]
    component = LockManager(repo / 'dependencies.lock').load().solved_components['lvgl/lvgl']
    destination = managed_dir / 'lvgl__lvgl'
    if not destination.exists():
        if ComponentFetcher(component, managed_dir).download() is None:
            raise RuntimeError('The locked LVGL component was not downloaded')
    # Existing modified sources must fail validation, never be overwritten.
    bytecode = [p for p in destination.rglob('*.pyc') if p.parent.name == '__pycache__']
    if bytecode:
        # pytest may leave bytecode in the vendor tree; validate unchanged sources without touching it.
        scratch = repo / '.test-build/lvgl-validation'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            target = Path(directory).resolve()
            assert target.is_relative_to(scratch.resolve())
            def ignore_cache(path, names):
                return [name for name in names if Path(path).name == '__pycache__' and name.endswith('.pyc')]
            shutil.copytree(destination, target / 'lvgl', ignore=ignore_cache)
            validate_hash_eq_hashdir(target / 'lvgl', component.component_hash)
        print(f'Validated sources excluding {len(bytecode)} generated Python bytecode files')
    else:
        validate_hash_eq_hashdir(destination, component.component_hash)
    print(f'Native UI LVGL ready: {component.version}, hash={component.component_hash}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--managed-dir', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'managed_components')
    prepare(parser.parse_args().managed_dir)


if __name__ == '__main__':
    main()
