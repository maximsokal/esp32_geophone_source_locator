#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmsis_core.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>

#include "nrf_fault_record.h"
#include "nrf_fault_record_zephyr.h"
#include "nrf_reboot_intent_zephyr.h"
#include "nrf_log_events.h"

#define TEST_EVENT_CAPACITY 21U

struct test_event {
    uint16_t id;
    uint32_t index;
    uint32_t value;
};

test_scb_t nrf_fault_test_scb;

static struct k_thread s_current_thread;
static bool s_in_isr;
static uint32_t s_reset_cause = 0xA5A55A5AU;
static jmp_buf s_reboot;
static struct test_event s_events[TEST_EVENT_CAPACITY];
static size_t s_event_count;
static unsigned s_printk_calls;
static char s_printk_text[160];

int printk(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(s_printk_text, sizeof(s_printk_text), format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(s_printk_text));
    ++s_printk_calls;
    return length;
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf);

struct k_thread *k_current_get(void)
{
    return &s_current_thread;
}

bool k_is_in_isr(void)
{
    return s_in_isr;
}

const char *k_thread_name_get(struct k_thread *thread)
{
    assert(thread == &s_current_thread);
    return "sensor_worker";
}

int hwinfo_get_reset_cause(uint32_t *cause)
{
    assert(cause != NULL);
    *cause = s_reset_cause;
    return 0;
}

uint32_t nrf_log_runtime_zephyr_boot_id(void)
{
    return 77U;
}

void nrf_log_runtime_zephyr_append_event(uint16_t event_id, uint32_t arg0, uint32_t arg1)
{
    assert(s_event_count < TEST_EVENT_CAPACITY);
    s_events[s_event_count++] = (struct test_event){
        .id = event_id,
        .index = arg0,
        .value = arg1,
    };
}

void sys_reboot(int type)
{
    assert(type == SYS_REBOOT_COLD);
    longjmp(s_reboot, 1);
}

static void fire_fatal(unsigned int reason, bool in_isr)
{
    const struct arch_esf esf = {.basic = {.pc = 0x12345678U, .lr = 0x87654321U}};

    s_in_isr = in_isr;
    nrf_fault_test_scb.CFSR = 0x00010000U;
    nrf_fault_test_scb.MMFAR = 0x20001234U;
    if (setjmp(s_reboot) == 0) {
        k_sys_fatal_error_handler(reason, &esf);
        assert(false);
    }
}

static nrf_fault_record_t report_and_decode_pending(void)
{
    uint8_t bytes[NRF_FAULT_RECORD_ENCODED_SIZE] = {0};
    nrf_fault_record_t record;

    s_event_count = 0U;
    nrf_fault_record_zephyr_report_pending();
    assert(s_event_count == TEST_EVENT_CAPACITY);
    for (size_t i = 0U; i < s_event_count; ++i) {
        assert(s_events[i].id == NRF_LOG_EVENT_FAULT_RECORD_WORD);
        assert(s_events[i].index == i);
        for (size_t byte = 0U; byte < sizeof(uint32_t); ++byte) {
            bytes[i * sizeof(uint32_t) + byte] = (uint8_t)(s_events[i].value >> (byte * 8U));
        }
    }
    assert(nrf_fault_record_decode(bytes, sizeof(bytes), &record) == NRF_FAULT_RECORD_OK);
    return record;
}

static void test_reboot_intent_is_reported_once_and_separate_from_faults(void)
{
    nrf_reboot_intent_zephyr_report_pending();
    assert(s_printk_calls == 0U);
    for (unsigned stage = NRF_REBOOT_INTENT_READDRESS; stage <= NRF_REBOOT_INTENT_ASSIGN_RETRY_QUEUE; ++stage) {
        nrf_reboot_intent_zephyr_record((nrf_reboot_intent_stage_t)stage, -22);
        nrf_reboot_intent_zephyr_report_pending();
        assert(s_printk_calls == stage);
        char expected[160];
        (void)snprintf(expected, sizeof(expected),
            "nrf_reboot_intent: previous software reboot request stage=%u error=-22\n", stage);
        assert(strcmp(s_printk_text, expected) == 0);
        nrf_reboot_intent_zephyr_report_pending();
        assert(s_printk_calls == stage);
    }
    nrf_reboot_intent_zephyr_record(NRF_REBOOT_INTENT_ASSIGN_SENSOR, -5);
    nrf_reboot_intent_zephyr_record(NRF_REBOOT_INTENT_READDRESS, 0);
    nrf_reboot_intent_zephyr_report_pending();
    assert(strcmp(s_printk_text,
        "nrf_reboot_intent: previous software reboot request stage=1 error=0\n") == 0);
    const unsigned printed = s_printk_calls;
    nrf_reboot_intent_zephyr_record((nrf_reboot_intent_stage_t)0, -1);
    nrf_reboot_intent_zephyr_report_pending();
    nrf_reboot_intent_zephyr_record((nrf_reboot_intent_stage_t)(NRF_REBOOT_INTENT_ASSIGN_RETRY_QUEUE + 1), -1);
    nrf_reboot_intent_zephyr_report_pending();
    assert(s_printk_calls == printed);
    assert(s_event_count == 0U);
    nrf_fault_record_zephyr_report_pending();
    assert(s_event_count == 0U);
}

int main(void)
{
    test_reboot_intent_is_reported_once_and_separate_from_faults();
    fire_fatal(K_ERR_STACK_CHK_FAIL, false);
    nrf_fault_record_t record = report_and_decode_pending();
    assert(record.fault_flags == NRF_FAULT_RECORD_FLAG_TERMINAL);
    assert(record.fault_class == NRF_FAULT_CLASS_STACK_EXHAUSTION);
    assert(record.reset_cause == s_reset_cause);
    assert(record.stacked_pc == 0x12345678U && record.stacked_lr == 0x87654321U);
    assert(strcmp(record.thread_name, "sensor_worker") == 0);
    nrf_fault_record_zephyr_report_pending();
    assert(s_event_count == TEST_EVENT_CAPACITY);

    fire_fatal(K_ERR_CPU_EXCEPTION, true);
    record = report_and_decode_pending();
    assert(record.fault_flags == NRF_FAULT_RECORD_FLAG_TERMINAL);
    assert(record.fault_class == NRF_FAULT_CLASS_INTERRUPT_CONTEXT);
    return 0;
}
