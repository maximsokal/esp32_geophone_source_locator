/* UNVERIFIED-ON-TARGET: retained placement, exception-frame layout and reboot behavior need NCS target validation. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <cmsis_core.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/linker/sections.h>
#include <zephyr/sys/reboot.h>

#include "nrf_fault_record.h"
#include "nrf_fault_record_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"

static __noinit uint8_t s_retained_fault_record[NRF_FAULT_RECORD_ENCODED_SIZE];

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    if (capacity == 0U) {
        return;
    }
    while (source != NULL && source[index] != '\0' && index + 1U < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    static const char firmware_version[] = __DATE__ " " __TIME__;
    nrf_fault_record_t record = {0};
    uint32_t reset_cause = 0U;
    struct k_thread *thread = k_current_get();
    const char *thread_name = NULL;

    (void)hwinfo_get_reset_cause(&reset_cause);
    record.reset_cause = reset_cause;
    record.fault_flags = NRF_FAULT_RECORD_FLAG_TERMINAL;
    record.fault_class = (uint8_t)nrf_fault_record_classify(
        k_is_in_isr(),
        reason == K_ERR_STACK_CHK_FAIL);
    const uint32_t mmfar = SCB->MMFAR;

    record.cfsr = SCB->CFSR;
    record.mmfsr = (uint8_t)(record.cfsr & 0xFFU);
    if ((record.mmfsr & NRF_FAULT_RECORD_MMARVALID) != 0U) {
        record.mmfar = mmfar;
    }
    if (esf != NULL) {
        record.stacked_pc = esf->basic.pc;
        record.stacked_lr = esf->basic.lr;
    }
    record.thread_id = (uint32_t)(uintptr_t)thread;
#if defined(CONFIG_THREAD_NAME)
    thread_name = k_thread_name_get(thread);
#endif
    copy_text(record.thread_name, sizeof(record.thread_name), thread_name);
    record.boot_id = nrf_log_runtime_zephyr_boot_id();
    copy_text(record.firmware_version, sizeof(record.firmware_version), firmware_version);
    (void)nrf_fault_record_encode(
        &record,
        s_retained_fault_record,
        sizeof(s_retained_fault_record));
    __DSB();
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
        /* If the controlled reboot fails, let the already armed WDT recover the node. */
    }
}

void nrf_fault_record_zephyr_report_pending(void)
{
    nrf_fault_record_t record;

    if (nrf_fault_record_decode(
            s_retained_fault_record,
            sizeof(s_retained_fault_record),
            &record) != NRF_FAULT_RECORD_OK) {
        return;
    }
    (void)record;
    for (uint32_t index = 0U; index < NRF_FAULT_RECORD_ENCODED_SIZE / sizeof(uint32_t); ++index) {
        const uint32_t offset = index * sizeof(uint32_t);
        const uint32_t value = (uint32_t)s_retained_fault_record[offset] |
            ((uint32_t)s_retained_fault_record[offset + 1U] << 8U) |
            ((uint32_t)s_retained_fault_record[offset + 2U] << 16U) |
            ((uint32_t)s_retained_fault_record[offset + 3U] << 24U);

        nrf_log_runtime_zephyr_append_event(
            NRF_LOG_EVENT_FAULT_RECORD_WORD,
            index,
            value);
    }
    memset(s_retained_fault_record, 0, sizeof(s_retained_fault_record));
}
