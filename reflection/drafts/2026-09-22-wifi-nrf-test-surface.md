# Candidate retrospective: Wi-Fi and nRF test-surface audit

## Evidence

- The nRF checkout had two sibling roots (`nRF54L15/test` and `nRF54L15/tests/ncs_native`) because PlatformIO Unity and Zephyr/NCS ztest were added under different runner conventions. The PlatformIO suite was moved below `nRF54L15/tests/platformio_native`; the contract and wrapper now enforce one top-level test root.
- Host and WebUI tests covered Wi-Fi policy and mocked HTTP, but they did not constitute physical ESP radio or phone-browser proof. A target stress probe and live HIL runners exist, but were not run here.
- The phone path had executable client serialization and object-store persistence coverage, but the C request/status boundary was only source-level coverage until `test_phone_location_transport_contract.py` was added.
- One test-authoring correction was needed: the first contract asserted a direct `sendControl(...)` call in `DeviceConsoleApp.tsx`; the actual abstraction is `runControl(...)`. The assertion was corrected after inspecting the source and the focused suite then passed 14/14.

## Root cause

Test-runner organization and evidence boundaries were not obvious from the directory names. Mocked browser/HTTP tests were also easy to mistake for live transport evidence, and the first new contract targeted an implementation layer that the UI does not call directly.

## Recurrence count

One confirmed test-authoring mismatch in this task. The related phone transport gap and the previously identified MAX-118 runtime bridge gap are two boundary-coverage examples, but they are not yet enough evidence to generalize a repository-wide process rule.

## Proposed minimal rule

For each device/network boundary, name the evidence level in the test and keep one contract at the actual call seam: host executable test for pure behavior, source contract only for wiring, and an explicit target/HIL test for radio/HTTP/phone claims. Before adding a source assertion, inspect the concrete call site rather than inferring the abstraction name.

## Affected work

- nRF54L15 PlatformIO/NCS test layout and wrapper discovery.
- Wi-Fi host/policy/physical-probe evidence separation.
- Phone GPS WebUI -> `/api/control` -> object store -> `/api/status` path.
- MAX-118 pod-status bridge runtime coverage audit.
