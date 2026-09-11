# Reflection draft: root-only not-found semantics

- Evidence: the tree remover correctly treated `stat(root) == ENOENT` as a missing requested target, but reused that result for recursive descendants. A child disappearing after enumeration therefore returned `ESP_ERR_NOT_FOUND`, which the recorder cleanup intentionally treated as benign while the root still existed.
- Root cause: the recursive helper did not carry operation context distinguishing the user-requested root from an internal child path.
- Recurrence count: 1 confirmed episode in this repository task.
- Minimal proposed rule: domain errors whose meaning depends on whether a path is the public operation target must be resolved at the public boundary or carry explicit root/recursive context; add a child-specific fault-injection regression.
- Affected verification: `storage_manager_tree_contract_test` now injects descendant `stat(ENOENT)` and asserts `ESP_FAIL`, preserved tree, and balanced storage guard calls.
