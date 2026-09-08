# Synthetic test vectors

This directory is reserved for deterministic, reviewable input vectors used by
host and target tests. The current localization suite keeps its small analytic
vectors next to the test source so the expected geometry and tolerances stay
visible in one place. Move a vector here when it is shared by more than one
test surface; do not add captured hardware data without recording its units,
sampling rate, sensor layout and provenance.

The vectors in this task are synthetic only. They do not establish AD7606
signal quality, timing jitter, or physical localization accuracy.
