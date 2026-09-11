#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#else
#include <unistd.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#include "storage_manager.h"


#define STORAGE_TREE_TEST_ROOT "storage_manager_tree_contract_tmp"
#define STORAGE_TREE_TEST_FILE STORAGE_TREE_TEST_ROOT "/payload.bin"
#define STORAGE_TREE_TEST_SUBDIR STORAGE_TREE_TEST_ROOT "/sub"
#define STORAGE_TREE_TEST_NESTED_FILE STORAGE_TREE_TEST_SUBDIR "/nested.bin"


typedef enum
{
    STORAGE_TREE_FAULT_NONE = 0,
    STORAGE_TREE_FAULT_READDIR,
    STORAGE_TREE_FAULT_CLOSEDIR,

} storage_tree_fault_t;


static storage_tree_fault_t fault_kind;
static unsigned fault_call;
static int fault_errno;
static unsigned readdir_calls;
static unsigned closedir_calls;
static esp_err_t operation_begin_result;
static unsigned operation_begin_calls;
static unsigned operation_end_calls;


static void reset_fault(void)
{
    fault_kind = STORAGE_TREE_FAULT_NONE;
    fault_call = 0U;
    fault_errno = 0;
    readdir_calls = 0U;
    closedir_calls = 0U;
}


static void cleanup_test_tree(void);


static DIR *storage_tree_test_opendir(const char *path)
{
    return opendir(path);
}


static struct dirent *storage_tree_test_readdir(DIR *directory)
{
    ++readdir_calls;
    if (
        fault_kind == STORAGE_TREE_FAULT_READDIR &&
        readdir_calls == fault_call
    ) {
        errno = fault_errno;
        return NULL;
    }

    return readdir(directory);
}


static int storage_tree_test_closedir(DIR *directory)
{
    ++closedir_calls;
    const int result = closedir(directory);
    if (
        fault_kind == STORAGE_TREE_FAULT_CLOSEDIR &&
        closedir_calls == fault_call
    ) {
        errno = fault_errno;
        return result == 0 ? -1 : result;
    }

    return result;
}


static void write_test_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}


static void make_test_tree(bool with_file)
{
    cleanup_test_tree();
    assert(mkdir(STORAGE_TREE_TEST_ROOT, 0775) == 0);
    if (with_file) {
        write_test_file(STORAGE_TREE_TEST_FILE, "payload");
    }
}


static void make_nested_test_tree(void)
{
    cleanup_test_tree();
    assert(mkdir(STORAGE_TREE_TEST_ROOT, 0775) == 0);
    write_test_file(STORAGE_TREE_TEST_FILE, "payload");
    assert(mkdir(STORAGE_TREE_TEST_SUBDIR, 0775) == 0);
    write_test_file(STORAGE_TREE_TEST_NESTED_FILE, "nested");
}


static bool test_path_is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}


static bool test_path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}


static void cleanup_test_tree(void)
{
    reset_fault();
    (void)remove(STORAGE_TREE_TEST_NESTED_FILE);
    (void)rmdir(STORAGE_TREE_TEST_SUBDIR);
    (void)remove(STORAGE_TREE_TEST_FILE);
    (void)rmdir(STORAGE_TREE_TEST_ROOT);
}


esp_err_t storage_manager_operation_begin(void)
{
    ++operation_begin_calls;
    return operation_begin_result;
}


void storage_manager_operation_end(void)
{
    ++operation_end_calls;
}


void vTaskDelay(unsigned long ticks)
{
    (void)ticks;
}


#define opendir storage_tree_test_opendir
#define readdir storage_tree_test_readdir
#define closedir storage_tree_test_closedir
#include "../storage_manager_tree.c"
#undef opendir
#undef readdir
#undef closedir


static void reset_operation_state(void)
{
    operation_begin_result = ESP_OK;
    operation_begin_calls = 0U;
    operation_end_calls = 0U;
}


static void test_readdir_error_preserves_tree(void)
{
    cleanup_test_tree();
    make_test_tree(true);
    reset_operation_state();
    reset_fault();
    fault_kind = STORAGE_TREE_FAULT_READDIR;
    fault_call = 1U;
    fault_errno = EIO;

    assert(storage_manager_remove_tree(STORAGE_TREE_TEST_ROOT) == ESP_FAIL);
    assert(test_path_is_directory(STORAGE_TREE_TEST_ROOT));
    assert(operation_begin_calls == 1U);
    assert(operation_end_calls == 1U);

    cleanup_test_tree();
}


static void test_readdir_enoent_is_traversal_failure(void)
{
    make_test_tree(true);
    reset_operation_state();
    reset_fault();
    fault_kind = STORAGE_TREE_FAULT_READDIR;
    fault_call = 1U;
    fault_errno = ENOENT;

    assert(storage_manager_remove_tree(STORAGE_TREE_TEST_ROOT) == ESP_FAIL);
    assert(test_path_is_directory(STORAGE_TREE_TEST_ROOT));
    assert(operation_begin_calls == 1U);
    assert(operation_end_calls == 1U);

    cleanup_test_tree();
}


static void test_closedir_error_preserves_tree(void)
{
    cleanup_test_tree();
    make_test_tree(false);
    reset_operation_state();
    reset_fault();
    fault_kind = STORAGE_TREE_FAULT_CLOSEDIR;
    fault_call = 1U;
    fault_errno = EIO;

    assert(storage_manager_remove_tree(STORAGE_TREE_TEST_ROOT) == ESP_FAIL);
    assert(test_path_is_directory(STORAGE_TREE_TEST_ROOT));
    assert(operation_begin_calls == 1U);
    assert(operation_end_calls == 1U);

    cleanup_test_tree();
}


static void test_recursive_remove_happy_path(void)
{
    make_nested_test_tree();
    reset_operation_state();
    reset_fault();

    assert(storage_manager_remove_tree(STORAGE_TREE_TEST_ROOT) == ESP_OK);
    assert(!test_path_exists(STORAGE_TREE_TEST_ROOT));
    assert(!test_path_exists(STORAGE_TREE_TEST_FILE));
    assert(!test_path_exists(STORAGE_TREE_TEST_SUBDIR));
    assert(!test_path_exists(STORAGE_TREE_TEST_NESTED_FILE));
    assert(operation_begin_calls == 1U);
    assert(operation_end_calls == 1U);
}


static void test_operation_guard_failure_leaves_tree(void)
{
    cleanup_test_tree();
    make_test_tree(false);
    reset_operation_state();
    operation_begin_result = ESP_ERR_INVALID_STATE;

    assert(storage_manager_remove_tree(STORAGE_TREE_TEST_ROOT) == ESP_ERR_INVALID_STATE);
    assert(test_path_is_directory(STORAGE_TREE_TEST_ROOT));
    assert(operation_begin_calls == 1U);
    assert(operation_end_calls == 0U);

    cleanup_test_tree();
}


int main(void)
{
    test_readdir_error_preserves_tree();
    test_readdir_enoent_is_traversal_failure();
    test_closedir_error_preserves_tree();
    test_recursive_remove_happy_path();
    test_operation_guard_failure_leaves_tree();
    puts("storage manager tree contract tests passed");
    return 0;
}
