#include "pb2/acquisition/pru_ring_consumer.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace pb2::acquisition {

namespace {

inline std::uint32_t load_acquire_u32(const volatile std::uint32_t* ptr) noexcept {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

inline void store_release_u32(volatile std::uint32_t* ptr, std::uint32_t val) noexcept {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

inline std::uint64_t load_relaxed_u64(const volatile std::uint64_t* ptr) noexcept {
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

} // namespace

PruSharedMemory::~PruSharedMemory() {
    close();
}

bool PruSharedMemory::attach_external(pb2_pru_shared_t* ptr) noexcept {
    close();
    if (!ptr) {
        return false;
    }
    shm_ = ptr;
    return true;
}

bool PruSharedMemory::open_file(const std::string& path, bool writable) {
    close();
    const int flags = writable ? O_RDWR : O_RDONLY;
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < sizeof(pb2_pru_shared_t)) {
        ::close(fd);
        return false;
    }

    const int prot = PROT_READ | PROT_WRITE;
    const int map_flags = writable ? MAP_SHARED : MAP_PRIVATE;
    void* mapped = ::mmap(nullptr, sizeof(pb2_pru_shared_t), prot, map_flags, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    map_base_ = mapped;
    map_len_ = sizeof(pb2_pru_shared_t);
    owns_mmap_ = true;
    shm_ = static_cast<pb2_pru_shared_t*>(mapped);
    return true;
}

bool PruSharedMemory::open_uio(const std::string& uio_dev, std::size_t shared_offset) {
    close();
    const int fd = ::open(uio_dev.c_str(), O_RDWR | O_SYNC);
    if (fd < 0) {
        return false;
    }
    const std::size_t map_len = shared_offset + PB2_PRU_SHARED_MEM_SIZE;
    void* mapped = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    map_base_ = mapped;
    map_len_ = map_len;
    owns_mmap_ = true;
    shm_ = reinterpret_cast<pb2_pru_shared_t*>(static_cast<std::uint8_t*>(mapped) + shared_offset);
    return true;
}

bool PruSharedMemory::open_dev_mem(std::uint64_t phys_addr, bool writable) {
    close();
    const long page_size_raw = ::sysconf(_SC_PAGESIZE);
    if (page_size_raw <= 0) {
        return false;
    }
    const std::uint64_t page_size = static_cast<std::uint64_t>(page_size_raw);
    const std::uint64_t page_base = phys_addr & ~(page_size - 1ULL);
    const std::uint64_t page_off = phys_addr - page_base;
    const std::size_t map_len = static_cast<std::size_t>(page_off + sizeof(pb2_pru_shared_t));

    const int fd = ::open("/dev/mem", (writable ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0) {
        return false;
    }

    const int prot = writable ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* mapped = ::mmap(nullptr, map_len, prot, MAP_SHARED, fd, static_cast<off_t>(page_base));
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    map_base_ = mapped;
    map_len_ = map_len;
    owns_mmap_ = true;
    shm_ = reinterpret_cast<pb2_pru_shared_t*>(static_cast<std::uint8_t*>(mapped) + page_off);
    return true;
}

void PruSharedMemory::close() noexcept {
    if (owns_mmap_ && map_base_ != nullptr && map_base_ != MAP_FAILED) {
        ::munmap(map_base_, map_len_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    shm_ = nullptr;
    map_base_ = nullptr;
    map_len_ = 0;
    fd_ = -1;
    owns_mmap_ = false;
}

PruRingConsumer::PruRingConsumer(pb2_pru_shared_t* shm) noexcept
    : shm_(shm) {}

bool PruRingConsumer::is_valid() const noexcept {
    if (!shm_) {
        return false;
    }
    return load_acquire_u32(&shm_->magic) == PB2_PRU_RING_MAGIC &&
           shm_->version == PB2_PRU_RING_VERSION &&
           shm_->channel_count == PB2_PRU_CHANNEL_COUNT;
}

pb2_pru_state_t PruRingConsumer::state() const noexcept {
    if (!shm_) {
        return PB2_PRU_STATE_FAULT;
    }
    return static_cast<pb2_pru_state_t>(load_acquire_u32(&shm_->state));
}

std::uint32_t PruRingConsumer::hw_error_flags() const noexcept {
    if (!shm_) {
        return PB2_PRU_ERR_INVALID_CONFIG;
    }
    return load_acquire_u32(&shm_->hw_error_flags);
}

bool PruRingConsumer::send_command(pb2_pru_cmd_t cmd,
                                   pb2_pru_state_t expected_state,
                                   std::uint32_t timeout_ms) {
    if (!is_valid()) {
        return false;
    }
    const std::uint32_t next_seq = load_acquire_u32(&shm_->cmd_seq) + 1U;
    shm_->cmd = static_cast<std::uint32_t>(cmd);
    store_release_u32(&shm_->cmd_seq, next_seq);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (load_acquire_u32(&shm_->last_cmd_ack) == next_seq) {
            return state() == expected_state;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return load_acquire_u32(&shm_->last_cmd_ack) == next_seq && state() == expected_state;
}

bool PruRingConsumer::start_acquisition(std::uint32_t sample_rate_hz,
                                        AdcWiringProfile profile,
                                        std::uint32_t iep_clock_hz,
                                        std::uint32_t session_epoch,
                                        std::uint32_t timeout_ms) {
    if (!is_valid()) {
        return false;
    }
    const auto cur = state();
    if (cur == PB2_PRU_STATE_FAULT) {
        if (!send_command(PB2_PRU_CMD_RESET_FAULT, PB2_PRU_STATE_IDLE, timeout_ms)) {
            return false;
        }
    }
    shm_->profile = static_cast<std::uint8_t>(profile);
    shm_->adc_owner = PB2_PRU_OWNER_BEAGLE;
    shm_->sample_rate_hz = sample_rate_hz;
    shm_->iep_clock_hz = iep_clock_hz;
    shm_->session_epoch = session_epoch;

    if (!send_command(PB2_PRU_CMD_CONFIGURE, PB2_PRU_STATE_CONFIGURED, timeout_ms)) {
        return false;
    }
    if (!send_command(PB2_PRU_CMD_ARM, PB2_PRU_STATE_ARMED, timeout_ms)) {
        return false;
    }
    return send_command(PB2_PRU_CMD_START, PB2_PRU_STATE_ACQUIRING, timeout_ms);
}

bool PruRingConsumer::stop_acquisition(std::uint32_t timeout_ms) {
    if (!is_valid()) {
        return false;
    }
    const auto cur = state();
    if (cur == PB2_PRU_STATE_STOPPED || cur == PB2_PRU_STATE_IDLE) {
        return true;
    }
    return send_command(PB2_PRU_CMD_STOP, PB2_PRU_STATE_STOPPED, timeout_ms);
}

std::size_t PruRingConsumer::drain(std::size_t max_blocks, std::vector<SampleFrame>& out) {
    if (!is_valid()) {
        return 0;
    }

    const std::uint32_t iep_hz = (shm_->iep_clock_hz != 0U) ? shm_->iep_clock_hz : 200000000U;
    const std::uint32_t sample_rate_hz = (shm_->sample_rate_hz != 0U) ? shm_->sample_rate_hz : 200000U;
    const std::uint64_t dt_us_num = 1000000ULL;

    std::uint32_t r = load_acquire_u32(&shm_->read_block_idx) & (PB2_PRU_BLOCK_CAPACITY - 1U);
    const std::uint32_t w = load_acquire_u32(&shm_->write_block_idx) & (PB2_PRU_BLOCK_CAPACITY - 1U);

    std::size_t blocks_drained = 0;
    std::size_t frames_appended = 0;

    while (r != w && (max_blocks == 0 || blocks_drained < max_blocks)) {
        const pb2_pru_block_t& blk = shm_->blocks[r];
        const std::uint16_t fc = (blk.frame_count <= PB2_PRU_BLOCK_FRAMES)
                                     ? blk.frame_count
                                     : static_cast<std::uint16_t>(PB2_PRU_BLOCK_FRAMES);

        if ((blk.flags & PB2_PRU_BLK_FLAG_GAP_BEFORE) != 0U) {
            stats_.gap_blocks++;
        }
        if ((blk.flags & PB2_PRU_BLK_FLAG_JITTER_WARN) != 0U) {
            stats_.jitter_warn_blocks++;
        }

        const std::uint64_t base_us =
            (blk.first_iep_ticks / iep_hz) * 1000000ULL +
            ((blk.first_iep_ticks % iep_hz) * 1000000ULL) / static_cast<std::uint64_t>(iep_hz);
        for (std::uint16_t i = 0; i < fc; ++i) {
            SampleFrame frame{};
            frame.source_sample_index = blk.first_sample_index + i;
            frame.timestamp_us = base_us + (static_cast<std::uint64_t>(i) * dt_us_num) / sample_rate_hz;
            frame.channels[0] = blk.channels[i][0];
            frame.channels[1] = blk.channels[i][1];
            frame.channels[2] = blk.channels[i][2];
            frame.channels[3] = blk.channels[i][3];
            frame.flags = (i == 0U && (blk.flags & PB2_PRU_BLK_FLAG_GAP_BEFORE) != 0U)
                              ? PB2_PRU_BLK_FLAG_GAP_BEFORE
                              : 0U;
            out.push_back(frame);
            frames_appended++;
        }

        r = (r + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U);
        blocks_drained++;
        stats_.blocks_read++;
        stats_.frames_read += fc;
    }

    store_release_u32(&shm_->read_block_idx, r);

    stats_.overrun_count = load_relaxed_u64(&shm_->overrun_count);
    stats_.dropped_frames = load_relaxed_u64(&shm_->dropped_frames);
    stats_.missed_deadlines = load_relaxed_u64(&shm_->missed_deadlines);
    stats_.max_jitter_ticks = load_acquire_u32(&shm_->max_jitter_ticks);
    stats_.hw_error_flags = load_acquire_u32(&shm_->hw_error_flags);

    return frames_appended;
}

PruDrainStats PruRingConsumer::snapshot_stats() const noexcept {
    PruDrainStats snap = stats_;
    if (shm_) {
        snap.overrun_count = load_relaxed_u64(&shm_->overrun_count);
        snap.dropped_frames = load_relaxed_u64(&shm_->dropped_frames);
        snap.missed_deadlines = load_relaxed_u64(&shm_->missed_deadlines);
        snap.max_jitter_ticks = load_acquire_u32(&shm_->max_jitter_ticks);
        snap.hw_error_flags = load_acquire_u32(&shm_->hw_error_flags);
    }
    return snap;
}

} // namespace pb2::acquisition
