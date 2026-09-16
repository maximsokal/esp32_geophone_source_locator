#include "storage_service_internal.h"

#include <string.h>

#include "esp_log.h"

#include "object_store.h"
#include "power_control.h"
#include "sd_card_info.h"
#include "storage_manager.h"


static const char *TAG =
    "STORAGE_SERVICE_OP";


static void storage_service_prepare_event(
    storage_service_event_t *event
)
{
    memset(
        event,
        0,
        sizeof(*event)
    );

    event->result =
        ESP_OK;
}


bool storage_service_process_info(
    storage_service_event_t *event
)
{
    storage_service_prepare_event(
        event
    );

    /*
     * sd_card_get_info() accesses FATFS directly, so keep it
     * inside the same operation guard used by object storage.
     */
    event->result =
        storage_manager_operation_begin();

    if (event->result == ESP_OK) {
        event->result =
            sd_card_get_info(
                &event->info
            );

        storage_manager_operation_end();
    }

    if (event->result == ESP_OK) {
        event->type =
            STORAGE_SERVICE_EVENT_INFO_DONE;

        ESP_LOGI(
            TAG,
            "Card info completed"
        );
    }
    else {
        event->type =
            STORAGE_SERVICE_EVENT_INFO_FAILED;

        ESP_LOGE(
            TAG,
            "Card info failed: %s",
            esp_err_to_name(
                event->result
            )
        );
    }

    return true;
}


bool storage_service_process_erase(
    storage_service_event_t *event
)
{
    storage_service_prepare_event(
        event
    );

    object_store_destructive_reservation_t reservation = {0};
    event->result = object_store_destructive_reservation_acquire(
        &reservation
    );

    if (event->result == ESP_OK) {
        /*
         * The logical reservation was committed before storage arbitration.
         * Existing recorder/scan leases therefore make ERASE fail immediately,
         * while new lifetime leases are rejected until this transaction exits.
         */
        event->result =
            storage_manager_operation_begin();

        if (event->result == ESP_OK) {
            event->result =
                storage_manager_erase_application_data();

            if (event->result == ESP_OK) {
                event->result =
                    object_store_reset();
            }

            storage_manager_operation_end();
        }

        const esp_err_t release_err =
            object_store_destructive_reservation_release(
                &reservation
            );

        if (release_err != ESP_OK) {
            if (event->result == ESP_OK) {
                event->result = release_err;
            }
            else {
                ESP_LOGE(
                    TAG,
                    "Destructive reservation cleanup failed after primary erase error: %s",
                    esp_err_to_name(release_err)
                );
            }
        }
    }

    if (event->result == ESP_OK) {
        event->type =
            STORAGE_SERVICE_EVENT_ERASE_DONE;

        ESP_LOGI(
            TAG,
            "Application data erase completed"
        );
    }
    else {
        event->type =
            STORAGE_SERVICE_EVENT_ERASE_FAILED;

        ESP_LOGE(
            TAG,
            "Application data erase failed: %s",
            esp_err_to_name(
                event->result
            )
        );
    }

    /*
     * If POWER was pressed during erase, the operation has already reached a
     * safe boundary and released both storage arbitration and the destructive
     * reservation. Skip the UI redraw and let shutdown continue.
     */
    if (
        power_control_is_shutdown_requested()
    ) {
        ESP_LOGI(
            TAG,
            "Shutdown pending after erase; skipping UI redraw"
        );

        return false;
    }

    return true;
}
