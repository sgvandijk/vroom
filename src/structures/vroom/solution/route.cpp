/*

This file is part of VROOM.

Copyright (c) 2015-2021, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include "structures/vroom/solution/route.h"

namespace vroom {

Route::Route() {
}

Route::Route(Id vehicle,
             std::vector<Step>&& steps,
             Cost cost,
             Duration service,
             Duration duration,
             Duration waiting_time,
             Priority priority,
             const Amount& delivery,
             const Amount& pickup,
             const std::string& profile,
             const std::string& description,
             const Violations&& violations)
  : vehicle(vehicle),
    steps(std::move(steps)),
    cost(cost),
    service(service),
    duration(duration),
    waiting_time(waiting_time),
    priority(priority),
    delivery(delivery),
    pickup(pickup),
    profile(profile),
    description(description),
    violations(std::move(violations)),
    distance(0) {
}

} // namespace vroom
