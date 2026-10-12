#pragma once

/* One BUSY edge wait per CONVST sample; retries are additional samples. */
#define AD7606_TCONV_EDGE_WAIT_TIMEOUT_US 100U
#define AD7606_TCONV_MAX_RETRIES 3U

/* Includes the CONVST pulse and BUSY edge capture, bounded by the fake clock. */
#define AD7606_TCONV_CRITICAL_MAX_US 150U

/* Four 100 us timeout attempts plus setup must stay below this startup budget. */
#define AD7606_TCONV_MEASUREMENT_MAX_US 1000U
