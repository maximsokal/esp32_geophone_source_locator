#include "web_server_internal.h"

#include <inttypes.h>
#include <stdlib.h>

#include "localization_run_record.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "seismic_recorder.h"

/*
 * MAX-168: read-only listing of every localization run already persisted
 * for one session (by localization_calculate_run(), via /api/control's
 * calculate_source action). Reuses localization_run_record_to_json() for
 * each entry verbatim rather than re-deriving a response schema, so this
 * route can never drift from what localization_run_store actually wrote to
 * disk.
 *
 * MAX-148: also reports this session's own frozen per-pod sensor geometry
 * (X/Y/Z, leveled) and the raw CS pair evidence behind it (distance,
 * confidence, valid/attempts), alongside the runs -- the exact session-
 * frozen state a run was actually computed against, never the object's
 * current live topology. Both are best-effort: a read failure (e.g. a
 * session recorded before MAX-166/MAX-148) reports its own *_available ==
 * false rather than failing the whole request -- the run listing above
 * remains this route's primary payload.
 */

static esp_err_t web_localization_query_copy(
    httpd_req_t *request,
    char *query,
    size_t query_size
)
{
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U || query_length >= query_size) {
        return query_length == 0U ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_SIZE;
    }
    return httpd_req_get_url_query_str(request, query, query_size);
}

static esp_err_t web_localization_runs_request_options(
    httpd_req_t *request,
    char object_id[OBJECT_STORE_ID_SIZE],
    char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE]
)
{
    char query[160];
    if (web_localization_query_copy(request, query, sizeof(query)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        httpd_query_key_value(query, "object_id", object_id, OBJECT_STORE_ID_SIZE) != ESP_OK ||
        httpd_query_key_value(query, "session_id", session_id, LOCALIZATION_RUN_SESSION_ID_SIZE) != ESP_OK
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static int web_localization_http_status(esp_err_t err)
{
    switch (err) {
        case ESP_ERR_NOT_FOUND:
            return 404;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_INVALID_SIZE:
            return 422;
        case ESP_ERR_NO_MEM:
        default:
            return 500;
    }
}

static esp_err_t web_localization_runs_handler(httpd_req_t *request)
{
    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE] = {0};

    if (web_localization_runs_request_options(request, object_id, session_id) != ESP_OK) {
        return httpd_resp_send_err(request, 400, "object_id and session_id are required");
    }

    localization_run_record_t *records = calloc(
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, sizeof(*records)
    );
    if (records == NULL) {
        return httpd_resp_send_err(request, 500, "Out of memory");
    }

    size_t record_count = 0U;
    const esp_err_t list_err = localization_run_store_list(
        object_id, session_id, records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &record_count
    );
    if (list_err != ESP_OK) {
        free(records);
        return httpd_resp_send_err(
            request,
            web_localization_http_status(list_err),
            list_err == ESP_ERR_NOT_FOUND
                ? "No localization runs for this session"
                : "Localization run listing failed"
        );
    }

    object_store_geometry_t geometry;
    const bool geometry_available =
        seismic_recorder_read_session_geometry(object_id, session_id, &geometry) == ESP_OK;

    object_store_cs_pairs_t cs_pairs;
    const bool cs_pairs_available =
        seismic_recorder_read_session_cs_pairs(object_id, session_id, &cs_pairs) == ESP_OK;

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    web_json_writer_t writer;
    web_json_writer_init(&writer, request);
    web_json_writer_printf(
        &writer,
        "{\"object_id\":\"%s\",\"session_id\":\"%s\",",
        object_id,
        session_id
    );

    web_json_writer_printf(
        &writer, "\"geometry\":{\"available\":%s,\"pods\":[", geometry_available ? "true" : "false"
    );
    for (size_t pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) {
        if (pod > 0U) {
            web_json_writer_write(&writer, ",");
        }
        if (geometry_available) {
            /* Split across two printf calls: the worst-case single string
             * (large negative mm values + a full uint64 timestamp) can
             * exceed web_json_writer_printf()'s internal 128-byte buffer. */
            web_json_writer_printf(
                &writer,
                "{\"pod\":%u,\"computed\":%s,\"leveled\":%s,"
                "\"x_mm\":%" PRId32 ",\"y_mm\":%" PRId32 ",",
                (unsigned)(pod + 1U),
                geometry.pods[pod].computed ? "true" : "false",
                geometry.pods[pod].leveled ? "true" : "false",
                geometry.pods[pod].x_mm,
                geometry.pods[pod].y_mm
            );
            web_json_writer_printf(
                &writer,
                "\"z_mm\":%" PRId32 ",\"computed_at_ms\":%" PRIu64 "}",
                geometry.pods[pod].z_mm,
                geometry.pods[pod].computed_at_ms
            );
        } else {
            web_json_writer_printf(
                &writer, "{\"pod\":%u,\"computed\":false,\"leveled\":false}", (unsigned)(pod + 1U)
            );
        }
    }
    web_json_writer_write(&writer, "]},");

    /*
     * MAX-148: CS pair placement-confidence evidence frozen alongside the
     * geometry above -- same best-effort contract (cs_pairs_available ==
     * false for a session recorded before this existed, never a hard
     * failure of the whole response).
     */
    web_json_writer_printf(
        &writer, "\"cs_pairs\":{\"available\":%s,\"pairs\":[", cs_pairs_available ? "true" : "false"
    );
    const size_t cs_pair_count = cs_pairs_available ? cs_pairs.pair_count : 0U;
    for (size_t i = 0U; i < cs_pair_count; ++i) {
        if (i > 0U) {
            web_json_writer_write(&writer, ",");
        }
        const object_store_cs_pair_t *pair = &cs_pairs.pairs[i];
        web_json_writer_printf(
            &writer,
            "{\"node_a\":%u,\"node_b\":%u,\"valid\":%s,\"distance_mm\":%" PRId32 ",",
            (unsigned)pair->node_a,
            (unsigned)pair->node_b,
            pair->valid ? "true" : "false",
            pair->distance_mm
        );
        web_json_writer_printf(
            &writer,
            "\"confidence\":%u,\"attempts\":%u,\"valid_samples\":%u}",
            (unsigned)pair->confidence,
            (unsigned)pair->attempts,
            (unsigned)pair->valid_samples
        );
    }
    web_json_writer_write(&writer, "]},");

    web_json_writer_write(&writer, "\"runs\":[");
    for (size_t i = 0U; i < record_count; ++i) {
        if (i > 0U) {
            web_json_writer_write(&writer, ",");
        }

        char *record_json = NULL;
        if (localization_run_record_to_json(&records[i], &record_json) == ESP_OK && record_json != NULL) {
            web_json_writer_write(&writer, record_json);
        } else {
            web_json_writer_write(&writer, "null");
        }
        free(record_json);
    }

    web_json_writer_write(&writer, "]}");
    free(records);
    return web_json_writer_finish(&writer);
}

esp_err_t web_localization_api_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t uri = {
        .uri = "/api/localization/runs",
        .method = HTTP_GET,
        .handler = web_localization_runs_handler,
        .user_ctx = NULL,
    };
    return web_server_register_uri_handler(server, &uri);
}
