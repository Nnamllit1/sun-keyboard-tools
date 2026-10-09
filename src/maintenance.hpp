#pragma once

namespace sun {
// Independent of Win32 so decisions about reboot boundaries are testable.
struct DriverFacts {
    bool service{}, running{}, pendingDelete{}, filter{}, binary{}, capture{}, registered{};
};
enum class DriverPhase { absent, active, restart, incomplete };
constexpr DriverPhase driverPhase(const DriverFacts& f) {
    if (f.pendingDelete) return DriverPhase::restart;
    if (f.service && f.running && f.filter && f.binary) return DriverPhase::active;
    if (f.service && f.filter && f.binary) return DriverPhase::restart;
    if (!f.service && (f.capture || f.binary)) return DriverPhase::restart;
    if (f.service || f.filter || f.binary || f.capture || f.registered) return DriverPhase::incomplete;
    return DriverPhase::absent;
}
constexpr bool mayInstallDriver(const DriverFacts& f) {
    return driverPhase(f) == DriverPhase::absent;
}
}
