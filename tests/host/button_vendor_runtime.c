/* Compile the installed production Button engine; do not emulate debounce. */
#ifdef _MSC_VER
/* Vendor code targets 32-bit ESP32/GCC, and uses integer-valued pointers in
 * its parameter API. Suppress only its MSVC porting warnings in this TU;
 * production modules and pipeline assertions still compile with /W4 /WX. */
#pragma warning(push)
#pragma warning(disable: 4018 4100 4244 4267 4311)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wvoid-pointer-to-int-cast"
#elif defined(__GNUC__)
/* Equivalent vendor-only porting diagnostics for the Linux host CI. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#endif
#include "../../managed_components/espressif__button/iot_button.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif
