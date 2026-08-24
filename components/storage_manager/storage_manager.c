#include "storage_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


#define STORAGE_PATH_BUFFER_SIZE  256


static const char *TAG =
    "STORAGE_MANAGER";


static SemaphoreHandle_t s_mutex =
    NULL;


static bool s_ready =
    false;


static esp_err_t storage_manager_lock_ready(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    if (!s_ready) {

        xSemaphoreGive(
            s_mutex
        );

        return ESP_ERR_INVALID_STATE;
    }


    return ESP_OK;
}


static void storage_manager_unlock(void)
{
    if (s_mutex != NULL) {

        xSemaphoreGive(
            s_mutex
        );
    }
}


static esp_err_t storage_manager_ensure_directory_locked(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    struct stat info;


    /*
     * First check whether the directory already exists.
     */
    if (
        stat(
            path,
            &info
        ) == 0
    ) {
        if (
            S_ISDIR(
                info.st_mode
            )
        ) {
            return ESP_OK;
        }


        ESP_LOGE(
            TAG,
            "Path exists but is not a directory: %s",
            path
        );


        return ESP_FAIL;
    }


    /*
     * FATFS VFS does not always report ENOENT for a
     * missing path. Try mkdir() regardless of stat errno.
     */
    errno =
        0;


    if (
        mkdir(
            path,
            0775
        ) == 0
    ) {
        ESP_LOGI(
            TAG,
            "Created directory: %s",
            path
        );


        return ESP_OK;
    }


    int mkdir_errno =
        errno;


    /*
     * Another operation may already have created it.
     * Verify that the existing path really is a directory.
     */
    if (
        mkdir_errno ==
        EEXIST
    ) {
        if (
            stat(
                path,
                &info
            ) == 0 &&
            S_ISDIR(
                info.st_mode
            )
        ) {
            return ESP_OK;
        }


        ESP_LOGE(
            TAG,
            "Existing path is not a directory: %s",
            path
        );


        return ESP_FAIL;
    }


    ESP_LOGE(
        TAG,
        "mkdir failed for %s, errno=%d",
        path,
        mkdir_errno
    );


    return ESP_FAIL;
}


static esp_err_t storage_manager_sidecar_path(
    const char *path,
    const char *suffix,
    char *result,
    size_t result_size
)
{
    if (
        path == NULL ||
        suffix == NULL ||
        result == NULL ||
        result_size == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    int written =
        snprintf(
            result,
            result_size,
            "%s%s",
            path,
            suffix
        );


    if (
        written < 0 ||
        written >=
            (int)result_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    return ESP_OK;
}


static esp_err_t storage_manager_recover_backup_locked(
    const char *path
)
{
    struct stat info;


    if (
        stat(
            path,
            &info
        ) == 0
    ) {
        return ESP_OK;
    }


    char backup_path[
        STORAGE_PATH_BUFFER_SIZE
    ];


    esp_err_t err =
        storage_manager_sidecar_path(
            path,
            ".bak",
            backup_path,
            sizeof(backup_path)
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Backup path is too long"
        );


        return err;
    }


    if (
        stat(
            backup_path,
            &info
        ) != 0
    ) {
        return ESP_OK;
    }


    ESP_LOGW(
        TAG,
        "Recovering backup: %s",
        path
    );


    if (
        rename(
            backup_path,
            path
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to recover backup for %s, errno=%d",
            path,
            errno
        );


        return ESP_FAIL;
    }


    return ESP_OK;
}


esp_err_t storage_manager_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }


    struct stat mount_info;


    if (
        stat(
            STORAGE_MANAGER_MOUNT_POINT,
            &mount_info
        ) != 0 ||
        !S_ISDIR(
            mount_info.st_mode
        )
    ) {
        ESP_LOGE(
            TAG,
            "SD mount point is not available"
        );


        return ESP_ERR_INVALID_STATE;
    }


    if (s_mutex == NULL) {

        s_mutex =
            xSemaphoreCreateMutex();


        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    esp_err_t err =
        storage_manager_ensure_directory_locked(
            STORAGE_MANAGER_ROOT_PATH
        );


    if (err == ESP_OK) {

        err =
            storage_manager_ensure_directory_locked(
                STORAGE_MANAGER_OBJECTS_PATH
            );
    }


    if (err == ESP_OK) {

        s_ready =
            true;
    }


    xSemaphoreGive(
        s_mutex
    );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to initialize storage layout"
        );


        return err;
    }


    ESP_LOGI(
        TAG,
        "Storage ready: %s",
        STORAGE_MANAGER_ROOT_PATH
    );


    return ESP_OK;
}


esp_err_t storage_manager_shutdown(void)
{
    if (s_mutex == NULL) {
        return ESP_OK;
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    s_ready =
        false;


    xSemaphoreGive(
        s_mutex
    );


    ESP_LOGI(
        TAG,
        "Storage manager stopped"
    );


    return ESP_OK;
}


bool storage_manager_is_ready(void)
{
    return s_ready;
}


esp_err_t storage_manager_create_directory(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    esp_err_t err =
        storage_manager_lock_ready();


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );


        return err;
    }


    err =
        storage_manager_ensure_directory_locked(
            path
        );


    storage_manager_unlock();


    return err;
}


bool storage_manager_path_exists(
    const char *path
)
{
    if (path == NULL) {
        return false;
    }


    esp_err_t err =
        storage_manager_lock_ready();


    if (err != ESP_OK) {
        return false;
    }


    struct stat info;


    bool exists =
        stat(
            path,
            &info
        ) == 0;


    storage_manager_unlock();


    return exists;
}


esp_err_t storage_manager_atomic_write_text(
    const char *path,
    const char *text
)
{
    if (
        path == NULL ||
        text == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    esp_err_t err =
        storage_manager_lock_ready();


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );


        return err;
    }


    char temp_path[
        STORAGE_PATH_BUFFER_SIZE
    ];


    char backup_path[
        STORAGE_PATH_BUFFER_SIZE
    ];


    err =
        storage_manager_sidecar_path(
            path,
            ".tmp",
            temp_path,
            sizeof(temp_path)
        );


    if (err == ESP_OK) {

        err =
            storage_manager_sidecar_path(
                path,
                ".bak",
                backup_path,
                sizeof(backup_path)
            );
    }


    if (err != ESP_OK) {

        storage_manager_unlock();


        return err;
    }


    err =
        storage_manager_recover_backup_locked(
            path
        );


    if (err != ESP_OK) {

        storage_manager_unlock();


        return err;
    }


    remove(
        temp_path
    );


    FILE *file =
        fopen(
            temp_path,
            "w"
        );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open temporary file: %s",
            temp_path
        );


        storage_manager_unlock();


        return ESP_FAIL;
    }


    bool write_ok =
        fputs(
            text,
            file
        ) >= 0;


    if (
        write_ok &&
        fflush(file) != 0
    ) {
        write_ok =
            false;
    }


    if (write_ok) {

        int descriptor =
            fileno(file);


        if (
            descriptor >= 0 &&
            fsync(descriptor) != 0
        ) {
            write_ok =
                false;
        }
    }


    if (
        fclose(file) != 0
    ) {
        write_ok =
            false;
    }


    if (!write_ok) {

        ESP_LOGE(
            TAG,
            "Failed to write temporary file: %s",
            temp_path
        );


        remove(
            temp_path
        );


        storage_manager_unlock();


        return ESP_FAIL;
    }


    struct stat info;


    bool had_original =
        stat(
            path,
            &info
        ) == 0;


    if (had_original) {

        remove(
            backup_path
        );


        if (
            rename(
                path,
                backup_path
            ) != 0
        ) {
            ESP_LOGE(
                TAG,
                "Failed to create backup for %s",
                path
            );


            remove(
                temp_path
            );


            storage_manager_unlock();


            return ESP_FAIL;
        }
    }


    if (
        rename(
            temp_path,
            path
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to replace file: %s",
            path
        );


        if (had_original) {

            rename(
                backup_path,
                path
            );
        }


        remove(
            temp_path
        );


        storage_manager_unlock();


        return ESP_FAIL;
    }


    if (had_original) {

        remove(
            backup_path
        );
    }


    storage_manager_unlock();


    return ESP_OK;
}


esp_err_t storage_manager_read_text(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    if (
        path == NULL ||
        buffer == NULL ||
        buffer_size < 2
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    esp_err_t err =
        storage_manager_lock_ready();


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );


        return err;
    }


    err =
        storage_manager_recover_backup_locked(
            path
        );


    if (err != ESP_OK) {

        storage_manager_unlock();


        return err;
    }


    FILE *file =
        fopen(
            path,
            "r"
        );


    if (file == NULL) {

        int open_errno =
            errno;


        storage_manager_unlock();


        if (
            open_errno ==
            ENOENT
        ) {
            return ESP_ERR_NOT_FOUND;
        }


        return ESP_ERR_NOT_FOUND;
    }


    size_t count =
        fread(
            buffer,
            1,
            buffer_size - 1,
            file
        );


    if (
        ferror(file)
    ) {
        fclose(
            file
        );


        storage_manager_unlock();


        return ESP_FAIL;
    }


    buffer[count] =
        '\0';


    /*
     * If the buffer was filled completely, check whether
     * additional data is still present.
     */
    if (
        count ==
        buffer_size - 1
    ) {
        int next_character =
            fgetc(
                file
            );


        if (
            next_character !=
            EOF
        ) {
            fclose(
                file
            );


            storage_manager_unlock();


            ESP_LOGE(
                TAG,
                "Text file is larger than buffer: %s",
                path
            );


            return ESP_ERR_INVALID_SIZE;
        }
    }


    fclose(
        file
    );


    storage_manager_unlock();


    return ESP_OK;
}