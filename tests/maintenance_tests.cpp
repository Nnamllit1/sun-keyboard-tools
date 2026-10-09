#include "maintenance.hpp"
#include <iostream>
int main() {
    using sun::DriverFacts; using sun::DriverPhase;
    struct Case { DriverFacts facts; DriverPhase phase; bool install; };
    const Case cases[] {
        {{}, DriverPhase::absent, true},
        {{true,true,false,true,true,true}, DriverPhase::active,false},
        {{true,true,false,true,true,false}, DriverPhase::active,false},
        {{true,false,false,true,true,false}, DriverPhase::restart,false},
        {{false,false,true,false,true,true}, DriverPhase::restart,false},
        {{false,false,false,false,true,false}, DriverPhase::restart,false},
        {{false,false,false,false,false,true}, DriverPhase::restart,false},
        {{true,false,false,false,false,false}, DriverPhase::incomplete,false},
        {{false,false,false,true,false,false}, DriverPhase::incomplete,false},
        {{false,false,false,false,false,false,true}, DriverPhase::incomplete,false}
    };
    for (const auto& test: cases) {
        if (sun::driverPhase(test.facts)!=test.phase || sun::mayInstallDriver(test.facts)!=test.install) {
            std::cerr<<"Unsafe driver maintenance decision\n";return 1;
        }
    }
    std::cout<<"10 driver maintenance cases passed\n";
}
