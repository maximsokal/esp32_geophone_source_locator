#include "pb2/power/shutdown_fsm.hpp"

namespace pb2::power {

bool ShutdownFsm::on_event(ShutdownEvent ev) noexcept {
    if (ev == ShutdownEvent::TimeoutOrError) {
        state_ = ShutdownState::Fault;
        return true;
    }

    switch (state_) {
        case ShutdownState::Running:
            if (ev == ShutdownEvent::RequestStop) {
                state_ = ShutdownState::StopAcquisition;
                return true;
            }
            return false;
        case ShutdownState::StopRequested:
            state_ = ShutdownState::StopAcquisition;
            return true;
        case ShutdownState::StopAcquisition:
            if (ev == ShutdownEvent::AcquisitionStopped) {
                state_ = ShutdownState::FlushStorage;
                return true;
            }
            return false;
        case ShutdownState::FlushStorage:
            if (ev == ShutdownEvent::StorageFlushed) {
                state_ = ShutdownState::FinalizeSession;
                return true;
            }
            return false;
        case ShutdownState::FinalizeSession:
            if (ev == ShutdownEvent::SessionFinalized) {
                state_ = ShutdownState::StopPru;
                return true;
            }
            return false;
        case ShutdownState::StopPru:
            if (ev == ShutdownEvent::PruStopped) {
                state_ = ShutdownState::SyncFilesystem;
                return true;
            }
            return false;
        case ShutdownState::SyncFilesystem:
            if (ev == ShutdownEvent::FilesystemSynced) {
                state_ = ShutdownState::LinuxShutdown;
                return true;
            }
            return false;
        case ShutdownState::LinuxShutdown:
            if (ev == ShutdownEvent::KernelHalted) {
                state_ = ShutdownState::SafePowerOff;
                return true;
            }
            return false;
        case ShutdownState::SafePowerOff:
        case ShutdownState::Fault:
            return false;
    }
    return false;
}

} // namespace pb2::power
