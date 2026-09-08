/* Include the production operation guard so its real depth/reserve pairing is tested. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define STORAGE_MANAGER_MOUNT_POINT "storage_manager_dma_contract_tmp"

#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "storage_manager_internal.h"

static size_t fake_largest_dma;
static size_t fake_free_dma;
static unsigned fake_spi_begin_calls;
static unsigned fake_spi_end_calls;
static esp_err_t fake_spi_begin_result;
static unsigned fake_mutex_take_calls;
static unsigned fake_mutex_give_calls;
static unsigned fake_frees;
static bool fake_malloc_fail;

size_t heap_caps_get_free_size(unsigned capabilities)
{
    (void)capabilities;
    return fake_free_dma;
}

size_t heap_caps_get_largest_free_block(unsigned capabilities)
{
    (void)capabilities;
    return fake_largest_dma;
}

void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    (void)capabilities;
    if (fake_malloc_fail) {
        return NULL;
    }
    return malloc(size);
}

void heap_caps_free(void *ptr)
{
    ++fake_frees;
    free(ptr);
}

unsigned esp_log_timestamp(void)
{
    return 0U;
}

esp_err_t app_spi_bus_sd_operation_begin(void)
{
    ++fake_spi_begin_calls;
    return fake_spi_begin_result;
}

void app_spi_bus_sd_operation_end(void)
{
    ++fake_spi_end_calls;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, unsigned long timeout)
{
    (void)semaphore;
    (void)timeout;
    ++fake_mutex_take_calls;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    ++fake_mutex_give_calls;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}

esp_err_t storage_manager_ensure_directory_locked(const char *path)
{
    (void)path;
    return ESP_OK;
}

#include "../storage_manager.c"

static void reset_fake_state(void)
{
    if (s_dma_reserve != NULL) {
        heap_caps_free(s_dma_reserve);
    }
    s_dma_reserve = NULL;
    s_operation_depth = 0U;
    s_operation_mutex = (SemaphoreHandle_t)1;
    s_ready = true;
    fake_largest_dma = 4096U;
    fake_free_dma = 4096U;
    fake_spi_begin_calls = 0U;
    fake_spi_end_calls = 0U;
    fake_spi_begin_result = ESP_OK;
    fake_mutex_take_calls = 0U;
    fake_mutex_give_calls = 0U;
    fake_frees = 0U;
    fake_malloc_fail = false;
    s_outer_operation_started_us = 0;
    s_outer_mutex_wait_us = 0;
    s_outer_tft_quiesce_us = 0;
}

static void test_unready_and_spi_failure(void)
{
    reset_fake_state();
    s_ready = false;
    assert(storage_manager_operation_begin() == ESP_ERR_INVALID_STATE);
    assert(fake_spi_begin_calls == 0U);

    s_ready = true;
    fake_spi_begin_result = ESP_FAIL;
    assert(storage_manager_operation_begin() == ESP_FAIL);
    assert(fake_spi_begin_calls == 1U);
    assert(fake_spi_end_calls == 0U);
    assert(s_operation_depth == 0U);
}

static void test_begin_end_reserves_dma_and_pairs_spi(void)
{
    reset_fake_state();
    assert(storage_manager_operation_begin() == ESP_OK);
    assert(s_operation_depth == 1U);
    assert(s_dma_reserve == NULL);
    assert(fake_spi_begin_calls == 1U);
    assert(fake_spi_end_calls == 0U);

    storage_manager_operation_end();
    assert(s_operation_depth == 0U);
    assert(s_dma_reserve != NULL);
    assert(fake_spi_end_calls == 1U);
    assert(fake_mutex_take_calls == 1U);
    assert(fake_mutex_give_calls == 1U);
}

static void test_nested_operations_share_one_spi_boundary(void)
{
    reset_fake_state();
    assert(storage_manager_operation_begin() == ESP_OK);
    assert(storage_manager_operation_begin() == ESP_OK);
    assert(s_operation_depth == 2U);
    assert(fake_spi_begin_calls == 1U);

    storage_manager_operation_end();
    assert(s_operation_depth == 1U);
    assert(s_dma_reserve == NULL);
    assert(fake_spi_end_calls == 0U);

    storage_manager_operation_end();
    assert(s_operation_depth == 0U);
    assert(s_dma_reserve != NULL);
    assert(fake_spi_end_calls == 1U);
}

static void test_dma_failure_restores_reserve_and_spi_pairing(void)
{
    reset_fake_state();
    assert(storage_manager_operation_begin() == ESP_OK);
    storage_manager_operation_end();
    assert(s_dma_reserve != NULL);

    fake_largest_dma = 256U;
    const unsigned frees_before = fake_frees;
    assert(storage_manager_operation_begin() == ESP_ERR_NO_MEM);
    assert(s_operation_depth == 0U);
    assert(s_dma_reserve != NULL);
    assert(fake_frees == frees_before + 1U);
    assert(fake_spi_begin_calls == 2U);
    assert(fake_spi_end_calls == 2U);

    /* Total free DMA can be abundant while the contiguous sector block is not. */
    fake_largest_dma = 511U;
    fake_free_dma = 8192U;
    assert(storage_manager_operation_begin() == ESP_ERR_NO_MEM);
    assert(s_operation_depth == 0U);

    fake_largest_dma = 512U;
    assert(storage_manager_operation_begin() == ESP_OK);
    storage_manager_operation_end();
}

static void test_reserve_allocation_failure_is_recoverable(void)
{
    reset_fake_state();
    fake_malloc_fail = true;
    assert(storage_manager_operation_begin() == ESP_OK);
    storage_manager_operation_end();
    assert(s_operation_depth == 0U);
    assert(s_dma_reserve == NULL);
    assert(fake_spi_begin_calls == 1U);
    assert(fake_spi_end_calls == 1U);
}

static void test_unmatched_end_is_noop(void)
{
    reset_fake_state();
    storage_manager_operation_end();
    assert(s_operation_depth == 0U);
    assert(fake_spi_end_calls == 0U);
    assert(fake_mutex_give_calls == 0U);
}

int main(void)
{
    test_unready_and_spi_failure();
    test_begin_end_reserves_dma_and_pairs_spi();
    test_nested_operations_share_one_spi_boundary();
    test_dma_failure_restores_reserve_and_spi_pairing();
    test_reserve_allocation_failure_is_recoverable();
    test_unmatched_end_is_noop();
    reset_fake_state();
    return 0;
}
