#pragma once

#include <cstdint>

namespace pb2::power {

enum class ShutdownState : std::uint8_t {
    Running = 0,
    StopRequested,
    StopAcquisition,
    FlushStorage,
    FinalizeSession,
    StopPru,
    SyncFilesystem,
    LinuxShutdown,
    SafePowerOff,
    Fault
};

enum class ShutdownEvent : std::uint8_t {
    RequestStop = 0,
    AcquisitionStopped,
    StorageFlushed,
    SessionFinalized,
    PruStopped,
    FilesystemSynced,
    KernelHalted,
    TimeoutOrError
};

class ShutdownFsm {
public:
    ShutdownState state() const noexcept { return state_; }
    bool is_safe_to_cut_power() const noexcept { return state_ == ShutdownState::SafePowerOff; }
    bool is_faulted() const noexcept { return state_ == ShutdownState::Fault; }

    bool on_event(ShutdownEvent ev) noexcept;
    void reset() noexcept { state_ = ShutdownState::Running; }

private:
    ShutdownState state_ = ShutdownState::Running;
};

} // namespace pb2::power
