# Unity test project layout

## Evidence

- The standalone ESP-IDF Unity project was stored at repository root `test_app`, while the remaining deterministic test surfaces lived under `tests/host`, `tests/python`, and `tests/vectors`.
- The user requested that the target test project be grouped with the other tests.
- It was moved to `tests/target/test_app`; all operational references were updated and the moved project compiled successfully.

## Root cause

The original test foundation added a target-specific ESP-IDF project at the root without extending the test directory taxonomy. The build and HIL tooling consequently encoded the root-level path.

## Minimal fix

Use `tests/target/test_app` as the target/HIL test surface, update the small set of scripts/tasks/docs, and adjust the moved CMake file's relative component paths. Keep generated target output in the project-local ignored build directory and remove it after verification.

## Recurrence count

1 confirmed layout issue in this task. This is not enough evidence for a generalized repository-wide rule.

## Proposed rule/tool change

When adding a standalone target test application, place it below `tests/target` from the start and run one clean configure from the final path before reporting success. Add a path assertion to the task contract so a root-level move cannot silently leave stale tooling references.

## Affected work

The ESP-IDF Unity test application and its suite/HIL/docs/task references. Production firmware sources and hardware behavior were not changed.
