#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <unistd.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#include "storage_manager.h"
#include "storage_manager_internal.h"


#define STORAGE_DIRECTORY_TEST_ROOT "storage_manager_directory_contract_tmp"
#define STORAGE_DIRECTORY_TEST_FILE STORAGE_DIRECTORY_TEST_ROOT "/existing.txt"


static esp_err_t fake_operation_begin_result;
static unsigned fake_operation_begin_calls;
static unsigned fake_operation_end_calls;


esp_err_t storage_manager_operation_begin(void)
{
    ++fake_operation_begin_calls;
    return fake_operation_begin_result;
}


void storage_manager_operation_end(void)
{
    ++fake_operation_end_calls;
}


#include "../storage_manager_directory.c"


static void reset_fake_state(void)
{
    fake_operation_begin_result = ESP_OK;
    fake_operation_begin_calls = 0U;
    fake_operation_end_calls = 0U;
}


static void remove_test_tree(void)
{
    (void)remove(STORAGE_DIRECTORY_TEST_FILE);
    (void)rmdir(STORAGE_DIRECTORY_TEST_ROOT);
}


static void test_exclusive_create_rejects_existing_paths(void)
{
    remove_test_tree();
    reset_fake_state();

    assert(storage_manager_create_directory_exclusive(STORAGE_DIRECTORY_TEST_ROOT) == ESP_OK);
    assert(storage_manager_create_directory_exclusive(STORAGE_DIRECTORY_TEST_ROOT) == ESP_ERR_INVALID_STATE);
    assert(fake_operation_begin_calls == 2U);
    assert(fake_operation_end_calls == 2U);

    struct stat info;
    assert(stat(STORAGE_DIRECTORY_TEST_ROOT, &info) == 0);
    assert(S_ISDIR(info.st_mode));
    remove_test_tree();
}


static void test_exclusive_create_rejects_existing_file(void)
{
    remove_test_tree();
    assert(mkdir(STORAGE_DIRECTORY_TEST_ROOT, 0775) == 0);
    FILE *file = fopen(STORAGE_DIRECTORY_TEST_FILE, "wb");
    assert(file != NULL);
    assert(fclose(file) == 0);

    reset_fake_state();
    assert(storage_manager_create_directory_exclusive(STORAGE_DIRECTORY_TEST_FILE) == ESP_ERR_INVALID_STATE);
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 1U);
    remove_test_tree();
}


static void test_exclusive_create_preserves_operation_guard_failure(void)
{
    remove_test_tree();
    reset_fake_state();
    fake_operation_begin_result = ESP_ERR_INVALID_STATE;

    assert(storage_manager_create_directory_exclusive(STORAGE_DIRECTORY_TEST_ROOT) == ESP_ERR_INVALID_STATE);
    assert(fake_operation_begin_calls == 1U);
    assert(fake_operation_end_calls == 0U);
    remove_test_tree();
}


static void test_exclusive_create_rejects_invalid_arguments(void)
{
    reset_fake_state();
    assert(storage_manager_create_directory_exclusive(NULL) == ESP_ERR_INVALID_ARG);
    assert(fake_operation_begin_calls == 0U);
    assert(fake_operation_end_calls == 0U);
}


int main(void)
{
    test_exclusive_create_rejects_invalid_arguments();
    test_exclusive_create_rejects_existing_paths();
    test_exclusive_create_rejects_existing_file();
    test_exclusive_create_preserves_operation_guard_failure();
    puts("storage manager directory contract tests passed");
    return 0;
}
