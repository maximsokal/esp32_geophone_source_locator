#pragma once

#include "pb2/acquisition/mock_source.hpp"
#include "pru_adc_shared.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pb2::acquisition {

struct PruDrainStats {
    std::uint64_t blocks_read = 0;
    std::uint64_t frames_read = 0;
    std::uint64_t gap_blocks = 0;
    std::uint64_t jitter_warn_blocks = 0;
    std::uint64_t overrun_count = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t missed_deadlines = 0;
    std::uint32_t max_jitter_ticks = 0;
    std::uint32_t hw_error_flags = 0;
};

/// RAII MemoryBackend for PRU1 shared memory:
/// - external pointer (virtual/unit tests),
/// - binary file mmap (loading PRU simulator shared-memory dumps), or
/// - physical /dev/mem mmap (AM6232 ICSS_SHARED @ 0x30010000 / carveout).
class PruSharedMemory {
public:
    PruSharedMemory() = default;
    ~PruSharedMemory();

    PruSharedMemory(const PruSharedMemory&) = delete;
    PruSharedMemory& operator=(const PruSharedMemory&) = delete;

    bool attach_external(pb2_pru_shared_t* ptr) noexcept;
    bool open_file(const std::string& path, bool writable = false);
    bool open_dev_mem(std::uint64_t phys_addr, bool writable = true);
    void close() noexcept;

    bool is_open() const noexcept { return shm_ != nullptr; }
    pb2_pru_shared_t* get() const noexcept { return shm_; }

private:
    pb2_pru_shared_t* shm_ = nullptr;
    void* map_base_ = nullptr;
    std::size_t map_len_ = 0;
    int fd_ = -1;
    bool owns_mmap_ = false;
};

class PruRingConsumer {
public:
    explicit PruRingConsumer(pb2_pru_shared_t* shm) noexcept;

    bool is_valid() const noexcept;

    /// Send an FSM command (CONFIGURE/ARM/START/STOP/RESET_FAULT) and wait up to timeout_ms.
    bool send_command(pb2_pru_cmd_t cmd, std::uint32_t timeout_ms = 500U);

    /// Drain up to `max_blocks` (0 = unlimited) from the PBR2 ring into `out` as SampleFrames.
    /// Returns number of SampleFrames appended to `out`.
    std::size_t drain(std::size_t max_blocks, std::vector<SampleFrame>& out);

    PruDrainStats snapshot_stats() const noexcept;

private:
    pb2_pru_shared_t* shm_ = nullptr;
    PruDrainStats stats_{};
};

} // namespace pb2::acquisition
