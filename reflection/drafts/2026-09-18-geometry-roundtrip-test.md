# Candidate retrospective: geometry persistence coverage

- Evidence: the current `object_store_profile.c` persisted geometry, but no behavioral host test exercised write -> reopen -> deserialize; the existing geometry coverage only tested solver/policy mapping.
- Root cause: the host manifests covered object-store filesystem and cache policy separately, while the cJSON-backed profile implementation had no native harness.
- Recurrence count: one confirmed episode in this task; do not generalize into a repository-wide rule yet.
- Proposed minimal rule: when a persistent wire/storage contract changes, add one production-path round-trip test plus a source-contract assertion for the authoritative length/schema.
- Affected work: MAX-114 snapshot-contract synchronization and MAX-118 geometry persistence follow-up.
- Tooling note: the PlatformIO native wrapper still requires GCC; the available Windows shell selected MSVC, so that native PlatformIO test surface remains blocked independently of the host manifest pass.
