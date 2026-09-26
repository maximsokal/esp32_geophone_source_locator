#include "ui_internal.h"

#include "app_events.h"
#include "ui_event_policy.h"
#include "wifi_manager.h"


static bool ui_setup_client_event_is_authoritative(void)
{
    return ui_event_setup_client_notice_allowed(
        wifi_manager_is_running(),
        wifi_manager_is_setup_mode(),
        wifi_manager_is_radio_state_uncertain(),
        wifi_manager_network_events_suppressed()
    );
}


static bool ui_sta_event_is_authoritative(void)
{
    return ui_event_sta_notice_allowed(
        wifi_manager_is_running(),
        wifi_manager_is_setup_mode(),
        wifi_manager_is_radio_state_uncertain(),
        wifi_manager_network_events_suppressed()
    );
}


void ui_move_index(
    size_t *value,
    size_t count,
    bool down
)
{
    if (
        value == NULL ||
        count == 0
    ) {
        return;
    }

    if (down) {
        *value = (*value + 1U) % count;
    } else if (*value == 0) {
        *value = count - 1U;
    } else {
        (*value)--;
    }
}


void ui_open_home_locked(void)
{
    ui_screen_open_locked(UI_SCREEN_HOME);
}


void ui_dispatch_key_locked(
    keypad_key_t key
)
{
    ui_screen_handle_key_locked(
        ui_context()->screen,
        key
    );
}


void ui_dispatch_app_event_locked(
    int event_type,
    int32_t event_value
)
{
    ui_context_t *context = ui_context();

    switch ((app_event_type_t)event_type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
            if (!ui_setup_client_event_is_authoritative()) {
                return;
            }
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_PHONE_READY)
            );
            break;

        case APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED:
            if (!ui_setup_client_event_is_authoritative()) {
                return;
            }
            context->wifi_setup_page_opened = false;

            if (context->wifi_credentials_saved) {
                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_WIFI_NOTICE_LINK_CLOSED)
                );
            } else if (
                context->wifi_ca_downloaded &&
                !context->wifi_ca_install_confirmed
            ) {
                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_WIFI_NOTICE_INSTALL_RECONNECT)
                );
            } else {
                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_WIFI_NOTICE_PHONE_DISCONNECTED)
                );
            }
            break;

        case APP_EVENT_WIFI_SETUP_PAGE_OPENED:
            context->wifi_setup_page_opened = true;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_SETUP_OPEN)
            );
            break;

        case APP_EVENT_WIFI_CREDENTIALS_SAVED:
            context->wifi_credentials_saved = true;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_SAVED)
            );
            break;

        case APP_EVENT_WIFI_STA_CONNECTING:
            if (
                !ui_sta_event_is_authoritative() ||
                wifi_manager_sta_has_ip()
            ) {
                return;
            }
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            context->wifi_verify_ready = false;
            context->wifi_verify_result_received = false;
            context->wifi_verify_nonce = 0;
            context->wifi_verify_result_mask = 0;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_CONNECTING)
            );
            break;

        case APP_EVENT_WIFI_STA_CONNECTED:
            if (
                !ui_sta_event_is_authoritative() ||
                !wifi_manager_sta_has_ip()
            ) {
                return;
            }
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_CONNECTED)
            );
            break;

        case APP_EVENT_WIFI_STA_DISCONNECTED:
            if (
                !ui_sta_event_is_authoritative() ||
                wifi_manager_sta_has_ip()
            ) {
                return;
            }
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            context->wifi_verify_ready = false;
            context->wifi_verify_result_received = false;
            context->wifi_verify_nonce = 0;
            context->wifi_verify_result_mask = 0;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_DISCONNECTED)
            );
            break;

        case APP_EVENT_TLS_CA_DOWNLOADED:
            context->wifi_ca_downloaded = true;
            context->wifi_ca_install_confirmed = false;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_CA_DOWNLOADED)
            );
            break;

        case APP_EVENT_TLS_CA_INSTALL_CONFIRMED:
            context->wifi_ca_downloaded = true;
            context->wifi_ca_install_confirmed = true;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_CA_CONFIRMED)
            );
            break;

        case APP_EVENT_TLS_VERIFY_READY:
            context->wifi_verify_ready = true;
            context->wifi_verify_result_received = false;
            context->wifi_verify_nonce = (uint32_t)event_value;
            context->wifi_verify_result_mask = 0;
            ui_set_notice_locked(
                ui_tr(UI_I18N_KEY_WIFI_NOTICE_HTTPS_READY)
            );
            break;

        case APP_EVENT_TLS_VERIFY_RESULT:
            context->wifi_verify_result_received = true;
            context->wifi_verify_result_mask = (uint32_t)event_value;

            if (
                (context->wifi_verify_result_mask &
                 APP_TLS_VERIFY_REQUIRED_MASK) ==
                APP_TLS_VERIFY_REQUIRED_MASK
            ) {
                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_WIFI_NOTICE_VERIFIED)
                );
            } else {
                ui_set_notice_locked(
                    ui_tr(UI_I18N_KEY_WIFI_NOTICE_VERIFY_INCOMPLETE)
                );
            }
            break;

        case APP_EVENT_POWER_BATTERY_ALERT:
            /* MAX-183: the header badge shows the state on every screen;
             * the notice adds the instruction where a screen has room. */
            switch (event_value) {
                case APP_POWER_ALERT_LOW:
                    ui_set_notice_locked(ui_tr(UI_I18N_KEY_POWER_ALERT_LOW));
                    break;
                case APP_POWER_ALERT_CRITICAL_GRACE:
                    ui_set_notice_locked(ui_tr(UI_I18N_KEY_POWER_ALERT_CRITICAL));
                    break;
                case APP_POWER_ALERT_CANCELLED:
                    ui_set_notice_locked(ui_tr(UI_I18N_KEY_POWER_ALERT_CANCELLED));
                    break;
                case APP_POWER_ALERT_SHUTDOWN:
                    ui_set_notice_locked(ui_tr(UI_I18N_KEY_POWER_ALERT_SHUTDOWN));
                    break;
                default:
                    return;
            }
            ui_refresh_header_locked();
            return;

        case APP_EVENT_STORAGE_RECOVERY_STARTED:
            context->storage_recovery_active = true;
            context->storage_recovery_failed = false;
            if (context->screen == UI_SCREEN_HOME) {
                ui_render_home_locked();
            } else if (context->screen == UI_SCREEN_OBJECTS) {
                context->objects_loaded = false;
                context->object_count = 0U;
                context->object_index = 0U;
                context->objects_mode = UI_OBJECTS_MENU;
                ui_set_notice_locked(ui_tr(UI_I18N_KEY_SHARED_CHECKING_STORAGE));
                ui_render_objects_locked();
            }
            return;

        case APP_EVENT_STORAGE_RECOVERY_FINISHED:
            context->storage_recovery_active = false;
            context->storage_recovery_failed = event_value != ESP_OK;

            /* Recovery can delete/repair objects outside object_service. Never
             * leave its copied TFT list selectable after completion. Reopening
             * Objects later uses the normal async list path; do not start a
             * second archive scan here while Web refresh is also being reopened. */
            context->objects_loaded = false;
            context->object_count = 0U;
            context->object_index = 0U;
            context->objects_mode = UI_OBJECTS_MENU;

            if (context->screen == UI_SCREEN_OBJECTS) {
                ui_set_notice_locked(
                    context->storage_recovery_failed
                        ? ui_tr(UI_I18N_KEY_STORAGE_RECOVERY_FAILED)
                        : ui_tr(UI_I18N_KEY_STORAGE_RECOVERY_COMPLETE)
                );
                ui_render_objects_locked();
            }
            else if (context->screen == UI_SCREEN_HOME) {
                ui_render_home_locked();
            }
            return;

        case APP_EVENT_KEYPAD:
        default:
            return;
    }

    ui_refresh_header_locked();

    if (context->screen == UI_SCREEN_WIFI) {
        ui_screen_render_locked(UI_SCREEN_WIFI);
    }
}
