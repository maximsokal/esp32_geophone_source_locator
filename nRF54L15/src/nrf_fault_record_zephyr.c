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
#include <zephyr/sys/printk.h>

#include "nrf_fault_record.h"
#include "nrf_fault_record_zephyr.h"
#include "nrf_reboot_intent_zephyr.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"

static __noinit uint8_t s_retained_fault_record[NRF_FAULT_RECORD_ENCODED_SIZE];

/* A separate retained marker for explicit software reboot requests. The
 * fatal-error record stays reserved exclusively for actual CPU exceptions.
 * A checksum and a magic-last commit reject uninitialized/corrupt RAM. */
#define NRF_REBOOT_INTENT_MAGIC UINT32_C(0x4E524249)
#define NRF_REBOOT_INTENT_SALT UINT32_C(0xC8BD2941)
static __noinit uint32_t s_reboot_intent_words[4];

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

void nrf_reboot_intent_zephyr_record(nrf_reboot_intent_stage_t stage, int error_code)
{
    const uint32_t code = (uint32_t)stage;
    const uint32_t error_bits = (uint32_t)error_code;

    /* Commit the validity word last so a reset during this write will be
     * rejected instead of being interpreted as an intentional reboot. */
    s_reboot_intent_words[0] = 0U;
    s_reboot_intent_words[1] = code;
    s_reboot_intent_words[2] = error_bits;
    s_reboot_intent_words[3] =
        NRF_REBOOT_INTENT_MAGIC ^ NRF_REBOOT_INTENT_SALT ^ code ^ error_bits;
    __DSB();
    s_reboot_intent_words[0] = NRF_REBOOT_INTENT_MAGIC;
    __DSB();
}

void nrf_reboot_intent_zephyr_report_pending(void)
{
    const uint32_t magic = s_reboot_intent_words[0];
    const uint32_t stage = s_reboot_intent_words[1];
    const uint32_t error_bits = s_reboot_intent_words[2];
    const uint32_t checksum = s_reboot_intent_words[3];

    /* Consume before printing: a second reboot must not replay old intent. */
    s_reboot_intent_words[0] = 0U;
    __DSB();
    if (magic != NRF_REBOOT_INTENT_MAGIC ||
        stage < NRF_REBOOT_INTENT_READDRESS ||
        stage > NRF_REBOOT_INTENT_ASSIGN_RETRY_QUEUE ||
        checksum != (NRF_REBOOT_INTENT_MAGIC ^ NRF_REBOOT_INTENT_SALT ^
                     stage ^ error_bits)) {
        return;
    }

    printk("nrf_reboot_intent: previous software reboot request stage=%u error=%d\n",
           (unsigned)stage, (int32_t)error_bits);
}
