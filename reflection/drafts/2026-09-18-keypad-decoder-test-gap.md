# Candidate retrospective: keypad decoder refactor verification gap

- Evidence: while extracting the keypad decoder, the adapter still contained the old private `keypad_decode_raw()` implementation and its removed threshold macros. The first focused native build also omitted the ESP-IDF keypad stub include for the new decoder-only manifest entry.
- Root cause: the production extraction and the manifest/test wiring were changed together without a compile-first check of the complete translation unit and its standalone include closure.
- Recurrence count: 1 confirmed episode in this task; this is not yet a generalized pattern.
- Minimal proposal: whenever a production function is extracted into a new C translation unit, immediately run one standalone host compile for the new unit and one target/component build before expanding the test suite. Do not change shared agent rules or skills from this single episode.
- Affected work: current keypad false-input investigation; related Linear MAX-6 and MAX-7 validation. No prior task is confirmed to share this exact extraction/wiring failure.
