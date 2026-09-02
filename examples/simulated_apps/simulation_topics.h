#pragma once

#include "industrial_bus/topic.h"

#include "cmp.pb.h"
#include "system.pb.h"

namespace industrial_bus::examples {

inline constexpr CommandTopic<cmp::v1::RunSimulation,
                              system::v1::CommandAck>
    run_simulation{"cmp.simulation.run",
                   "industrial_bus.cmp.v1.RunSimulation",
                   "industrial_bus.system.v1.CommandAck"};

inline constexpr EventTopic<cmp::v1::SimulationProgress>
    simulation_progress{"cmp.simulation.progress",
                        "industrial_bus.cmp.v1.SimulationProgress"};

inline constexpr EventTopic<cmp::v1::SimulationCompleted>
    simulation_completed{"cmp.simulation.completed",
                         "industrial_bus.cmp.v1.SimulationCompleted"};

inline constexpr EventTopic<cmp::v1::SimulationFailed>
    simulation_failed{"cmp.simulation.failed",
                      "industrial_bus.cmp.v1.SimulationFailed"};

}  // namespace industrial_bus::examples
