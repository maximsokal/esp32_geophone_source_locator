from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_fatfs_keeps_internal_ram_pressure_policy_explicit():
    sdkconfig = read("sdkconfig.defaults")

    # ESP-IDF 5.5.4 names this option EXTRAM. FatFs ff_memalloc() prefers
    # PSRAM and falls back to internal RAM, preserving scarce internal heap.
    assert "CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y" in sdkconfig
    assert "CONFIG_FATFS_PER_FILE_CACHE=y" in sdkconfig


def test_fastseek_stays_disabled_for_reopen_per_chunk_reader():
    sdkconfig = read("sdkconfig.defaults")
    reader = read("components/seismic_recorder/seismic_recorder_reader.c")

    # In ESP-IDF 5.5.4, FASTSEEK builds a CLMT on every read-only open().
    # This reader intentionally reopens the file for every bounded SD window,
    # so enabling FASTSEEK would rebuild the cluster map per chunk.
    assert "# CONFIG_FATFS_USE_FASTSEEK is not set" in sdkconfig
    assert "CONFIG_FATFS_USE_FASTSEEK=y" not in sdkconfig

    read_at = function_body(
        reader,
        "static esp_err_t seismic_recorder_reader_read_at(",
        "static esp_err_t seismic_recorder_cache_reader_read_at(",
    )
    begin = read_at.index("storage_manager_operation_begin()")
    open_file = read_at.index('fopen(reader->path, "rb")', begin)
    seek = read_at.index("fseek(file, (long)offset_bytes, SEEK_SET)", open_file)
    read_file = read_at.index("fread(buffer, 1U, size_bytes, file)", seek)
    close_file = read_at.index("fclose(file)", read_file)
    end = read_at.index("storage_manager_operation_end()", close_file)

    assert begin < open_file < seek < read_file < close_file < end


def test_recorder_keeps_explicit_sync_policy_instead_of_syncing_every_write():
    sdkconfig = read("sdkconfig.defaults")
    filesystem = read("components/seismic_recorder/seismic_recorder_fs.c")

    assert "# CONFIG_FATFS_IMMEDIATE_FSYNC is not set" in sdkconfig
    assert "seismic_recorder_fs_flush(" in filesystem
    assert "seismic_recorder_fs_sync(" in filesystem
    assert "fsync(fileno(samples_file))" in filesystem


def test_no_fatfs_file_lock_dependency_is_hidden_by_reader_windows():
    sdkconfig = read("sdkconfig.defaults")
    reader = read("components/seismic_recorder/seismic_recorder_reader.c")

    assert "CONFIG_FATFS_FS_LOCK=0" in sdkconfig
    assert "SEISMIC_RECORDER_READ_STAGING_FRAMES  1024U" in read(
        "components/seismic_recorder/seismic_recorder_internal.h"
    )
    assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in reader
