#pragma once

/* Host contract tests model the API shape only; they do not model heap
 * capabilities or prove that a target stack lives in PSRAM. */
#define MALLOC_CAP_SPIRAM  (1U << 10)
#define MALLOC_CAP_8BIT    (1U << 2)

