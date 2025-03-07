/*

This file is part of VROOM.

Copyright (c) 2015-2021, Julien Coupey.
All rights reserved (see LICENSE).

*/

#include <mutex>
#include <thread>

#include "algorithms/validation/check.h"
#include "problems/cvrp/cvrp.h"
#include "problems/vrptw/vrptw.h"
#if USE_LIBOSRM
#include "routing/libosrm_wrapper.h"
#endif
#include "routing/ors_wrapper.h"
#include "routing/osrm_routed_wrapper.h"
#include "routing/valhalla_wrapper.h"
#include "structures/vroom/input/input.h"
#include "utils/helpers.h"

namespace vroom {

Input::Input(unsigned amount_size, const io::Servers& servers, ROUTER router)
  : _start_loading(std::chrono::high_resolution_clock::now()),
    _no_addition_yet(true),
    _has_TW(false),
    _homogeneous_locations(true),
    _homogeneous_profiles(true),
    _geometry(false),
    _has_jobs(false),
    _has_shipments(false),
    _max_matrices_used_index(0),
    _all_locations_have_coords(true),
    _amount_size(amount_size),
    _zero(_amount_size),
    _servers(servers),
    _router(router) {
}

void Input::set_geometry(bool geometry) {
  _geometry = geometry;
}

void Input::add_routing_wrapper(const std::string& profile) {
  assert(std::find_if(_routing_wrappers.begin(),
                      _routing_wrappers.end(),
                      [&](const auto& wr) { return wr->profile == profile; }) ==
         _routing_wrappers.end());

  auto& routing_wrapper = _routing_wrappers.emplace_back();

  switch (_router) {
  case ROUTER::OSRM: {
    // Use osrm-routed.
    auto search = _servers.find(profile);
    if (search == _servers.end()) {
      throw Exception(ERROR::INPUT, "Invalid profile: " + profile + ".");
    }
    routing_wrapper =
      std::make_unique<routing::OsrmRoutedWrapper>(profile, search->second);
  } break;
  case ROUTER::LIBOSRM:
#if USE_LIBOSRM
    // Use libosrm.
    try {
      routing_wrapper = std::make_unique<routing::LibosrmWrapper>(profile);
    } catch (const osrm::exception& e) {
      throw Exception(ERROR::ROUTING, "Invalid profile: " + profile);
    }
#else
    // Attempt to use libosrm while compiling without it.
    throw Exception(ERROR::ROUTING,
                    "VROOM compiled without libosrm installed.");
#endif
    break;
  case ROUTER::ORS: {
    // Use ORS http wrapper.
    auto search = _servers.find(profile);
    if (search == _servers.end()) {
      throw Exception(ERROR::INPUT, "Invalid profile: " + profile + ".");
    }
    routing_wrapper =
      std::make_unique<routing::OrsWrapper>(profile, search->second);
  } break;
  case ROUTER::VALHALLA: {
    // Use Valhalla http wrapper.
    auto search = _servers.find(profile);
    if (search == _servers.end()) {
      throw Exception(ERROR::INPUT, "Invalid profile: " + profile + ".");
    }
    routing_wrapper =
      std::make_unique<routing::ValhallaWrapper>(profile, search->second);
  } break;
  }
}

void Input::check_job(Job& job) {
  // Ensure delivery size consistency.
  const auto& delivery_size = job.delivery.size();
  if (delivery_size != _amount_size) {
    throw Exception(ERROR::INPUT,
                    "Inconsistent delivery length: " +
                      std::to_string(delivery_size) + " instead of " +
                      std::to_string(_amount_size) + '.');
  }

  // Ensure pickup size consistency.
  const auto& pickup_size = job.pickup.size();
  if (pickup_size != _amount_size) {
    throw Exception(ERROR::INPUT,
                    "Inconsistent pickup length: " +
                      std::to_string(pickup_size) + " instead of " +
                      std::to_string(_amount_size) + '.');
  }

  // Ensure that skills or location index are either always or never
  // provided.
  bool has_location_index = job.location.user_index();
  if (_no_addition_yet) {
    _has_skills = !job.skills.empty();
    _no_addition_yet = false;
    _has_custom_location_index = has_location_index;
  } else {
    if (_has_skills != !job.skills.empty()) {
      throw Exception(ERROR::INPUT, "Missing skills.");
    }
    if (_has_custom_location_index != has_location_index) {
      throw Exception(ERROR::INPUT, "Missing location index.");
    }
  }

  // Check for time-windows.
  _has_TW |= (!(job.tws.size() == 1) or !job.tws[0].is_default());

  if (!job.location.user_index()) {
    // Index of job in the matrices is not specified in input, check
    // for already stored location or assign new index.
    auto search = _locations_to_index.find(job.location);
    if (search != _locations_to_index.end()) {
      // Using stored index for existing location.
      job.location.set_index(search->second);
    } else {
      // Append new location and store corresponding index.
      auto new_index = _locations.size();
      job.location.set_index(new_index);
      _locations.push_back(job.location);
      _locations_to_index.insert(std::make_pair(job.location, new_index));
    }
  } else {
    // All jobs have a location_index in input, we only store
    // locations in case one profile matrix is not provided in input
    // and need to be computed.
    auto search = _locations_to_index.find(job.location);
    if (search == _locations_to_index.end()) {
      _locations.push_back(job.location);
      _locations_to_index.insert(
        std::make_pair(job.location, _locations.size() - 1));
    }
  }

  _matrices_used_index.insert(job.index());
  _max_matrices_used_index = std::max(_max_matrices_used_index, job.index());
  _all_locations_have_coords =
    _all_locations_have_coords && job.location.has_coordinates();
}

void Input::add_job(const Job& job) {
  if (job.type != JOB_TYPE::SINGLE) {
    throw Exception(ERROR::INPUT, "Wrong job type.");
  }
  if (job_id_to_rank.find(job.id) != job_id_to_rank.end()) {
    throw Exception(ERROR::INPUT,
                    "Duplicate job id: " + std::to_string(job.id) + ".");
  }
  job_id_to_rank[job.id] = jobs.size();
  jobs.push_back(job);
  check_job(jobs.back());
  _has_jobs = true;
}

void Input::add_shipment(const Job& pickup, const Job& delivery) {
  if (pickup.priority != delivery.priority) {
    throw Exception(ERROR::INPUT, "Inconsistent shipment priority.");
  }
  if (!(pickup.pickup == delivery.delivery)) {
    throw Exception(ERROR::INPUT, "Inconsistent shipment amount.");
  }
  if (pickup.skills.size() != delivery.skills.size()) {
    throw Exception(ERROR::INPUT, "Inconsistent shipment skills.");
  }
  for (const auto s : pickup.skills) {
    if (delivery.skills.find(s) == delivery.skills.end()) {
      throw Exception(ERROR::INPUT, "Inconsistent shipment skills.");
    }
  }

  if (pickup.type != JOB_TYPE::PICKUP) {
    throw Exception(ERROR::INPUT, "Wrong pickup type.");
  }
  if (pickup_id_to_rank.find(pickup.id) != pickup_id_to_rank.end()) {
    throw Exception(ERROR::INPUT,
                    "Duplicate pickup id: " + std::to_string(pickup.id) + ".");
  }
  pickup_id_to_rank[pickup.id] = jobs.size();
  jobs.push_back(pickup);
  check_job(jobs.back());

  if (delivery.type != JOB_TYPE::DELIVERY) {
    throw Exception(ERROR::INPUT, "Wrong delivery type.");
  }
  if (delivery_id_to_rank.find(delivery.id) != delivery_id_to_rank.end()) {
    throw Exception(ERROR::INPUT,
                    "Duplicate delivery id: " + std::to_string(delivery.id) +
                      ".");
  }
  delivery_id_to_rank[delivery.id] = jobs.size();
  jobs.push_back(delivery);
  check_job(jobs.back());
  _has_shipments = true;
}

void Input::add_vehicle(const Vehicle& vehicle) {
  vehicles.push_back(vehicle);

  auto& current_v = vehicles.back();

  // Ensure amount size consistency.
  const auto& vehicle_amount_size = current_v.capacity.size();
  if (vehicle_amount_size != _amount_size) {
    throw Exception(ERROR::INPUT,
                    "Inconsistent capacity length: " +
                      std::to_string(vehicle_amount_size) + " instead of " +
                      std::to_string(_amount_size) + '.');
  }

  // Check for time-windows.
  _has_TW = _has_TW || !vehicle.tw.is_default();

  bool has_location_index = false;
  if (current_v.has_start()) {
    auto& start_loc = current_v.start.value();

    has_location_index = start_loc.user_index();

    if (!start_loc.user_index()) {
      // Index of start in the matrices is not specified in input,
      // check for already stored location or assign new index.
      assert(start_loc.has_coordinates());
      auto search = _locations_to_index.find(start_loc);
      if (search != _locations_to_index.end()) {
        // Using stored index for existing location.
        start_loc.set_index(search->second);
      } else {
        // Append new location and store corresponding index.
        auto new_index = _locations.size();
        start_loc.set_index(new_index);
        _locations.push_back(start_loc);
        _locations_to_index.insert(std::make_pair(start_loc, new_index));
      }
    } else {
      // All starts have a location_index in input, we only store
      // locations in case one profile matrix is not provided in input
      // and need to be computed.
      auto search = _locations_to_index.find(start_loc);
      if (search == _locations_to_index.end()) {
        _locations.push_back(start_loc);
        _locations_to_index.insert(
          std::make_pair(start_loc, _locations.size() - 1));
      }
    }

    _matrices_used_index.insert(start_loc.index());
    _max_matrices_used_index =
      std::max(_max_matrices_used_index, start_loc.index());
    _all_locations_have_coords =
      _all_locations_have_coords && start_loc.has_coordinates();
  }

  if (current_v.has_end()) {
    auto& end_loc = current_v.end.value();

    if (current_v.has_start() and
        (has_location_index != end_loc.user_index())) {
      // Start and end provided in a non-consistent manner with regard
      // to location index definition.
      throw Exception(ERROR::INPUT, "Missing start_index or end_index.");
    }

    has_location_index = end_loc.user_index();

    if (!end_loc.user_index()) {
      // Index of this end in the matrix was not specified upon
      // vehicle creation.
      assert(end_loc.has_coordinates());
      auto search = _locations_to_index.find(end_loc);
      if (search != _locations_to_index.end()) {
        // Using stored index for existing location.
        end_loc.set_index(search->second);
      } else {
        // Append new location and store corresponding index.
        auto new_index = _locations.size();
        end_loc.set_index(new_index);
        _locations.push_back(end_loc);
        _locations_to_index.insert(std::make_pair(end_loc, new_index));
      }
    } else {
      // All ends have a location_index in input, we only store
      // locations in case one profile matrix is not provided in input
      // and need to be computed.
      auto search = _locations_to_index.find(end_loc);
      if (search == _locations_to_index.end()) {
        _locations.push_back(end_loc);
        _locations_to_index.insert(
          std::make_pair(end_loc, _locations.size() - 1));
      }
    }

    _matrices_used_index.insert(end_loc.index());
    _max_matrices_used_index =
      std::max(_max_matrices_used_index, end_loc.index());
    _all_locations_have_coords =
      _all_locations_have_coords && end_loc.has_coordinates();
  }

  // Ensure that skills or location index are either always or never
  // provided.
  if (_no_addition_yet) {
    _has_skills = !current_v.skills.empty();
    _no_addition_yet = false;
    _has_custom_location_index = has_location_index;
  } else {
    if (_has_skills != !current_v.skills.empty()) {
      throw Exception(ERROR::INPUT, "Missing skills.");
    }
    if (_has_custom_location_index != has_location_index) {
      throw Exception(ERROR::INPUT, "Missing location index.");
    }
  }

  // Check for homogeneous locations among vehicles.
  if (vehicles.size() > 1) {
    _homogeneous_locations =
      _homogeneous_locations &&
      vehicles.front().has_same_locations(vehicles.back());
    _homogeneous_profiles = _homogeneous_profiles &&
                            vehicles.front().has_same_profile(vehicles.back());
  }

  _profiles.insert(current_v.profile);
}

void Input::set_matrix(const std::string& profile, Matrix<Cost>&& m) {
  _custom_matrices.insert(profile);
  _matrices.insert_or_assign(profile, m);
}

bool Input::has_skills() const {
  return _has_skills;
}

bool Input::has_jobs() const {
  return _has_jobs;
}

bool Input::has_shipments() const {
  return _has_shipments;
}

bool Input::has_homogeneous_locations() const {
  return _homogeneous_locations;
}

bool Input::has_homogeneous_profiles() const {
  return _homogeneous_profiles;
}

bool Input::vehicle_ok_with_vehicle(Index v1_index, Index v2_index) const {
  return _vehicle_to_vehicle_compatibility[v1_index][v2_index];
}

void Input::check_cost_bound(const Matrix<Cost>& matrix) const {
  // Check that we don't have any overflow while computing an upper
  // bound for solution cost.

  std::vector<Cost> max_cost_per_line(matrix.size(), 0);
  std::vector<Cost> max_cost_per_column(matrix.size(), 0);

  for (const auto i : _matrices_used_index) {
    for (const auto j : _matrices_used_index) {
      max_cost_per_line[i] = std::max(max_cost_per_line[i], matrix[i][j]);
      max_cost_per_column[j] = std::max(max_cost_per_column[j], matrix[i][j]);
    }
  }

  Cost jobs_departure_bound = 0;
  Cost jobs_arrival_bound = 0;
  for (const auto& j : jobs) {
    jobs_departure_bound =
      utils::add_without_overflow(jobs_departure_bound,
                                  max_cost_per_line[j.index()]);
    jobs_arrival_bound =
      utils::add_without_overflow(jobs_arrival_bound,
                                  max_cost_per_column[j.index()]);
  }

  Cost jobs_bound = std::max(jobs_departure_bound, jobs_arrival_bound);

  Cost start_bound = 0;
  Cost end_bound = 0;
  for (const auto& v : vehicles) {
    if (v.has_start()) {
      start_bound =
        utils::add_without_overflow(start_bound,
                                    max_cost_per_line[v.start.value().index()]);
    }
    if (v.has_end()) {
      end_bound =
        utils::add_without_overflow(end_bound,
                                    max_cost_per_column[v.end.value().index()]);
    }
  }

  Cost bound = utils::add_without_overflow(start_bound, jobs_bound);
  bound = utils::add_without_overflow(bound, end_bound);
}

void Input::set_skills_compatibility() {
  // Default to no restriction when no skills are provided.
  _vehicle_to_job_compatibility = std::vector<
    std::vector<unsigned char>>(vehicles.size(),
                                std::vector<unsigned char>(jobs.size(), true));
  if (_has_skills) {
    for (std::size_t v = 0; v < vehicles.size(); ++v) {
      const auto& v_skills = vehicles[v].skills;
      assert(!v_skills.empty());

      for (std::size_t j = 0; j < jobs.size(); ++j) {
        bool is_compatible = true;
        assert(!jobs[j].skills.empty());
        for (const auto& s : jobs[j].skills) {
          if (v_skills.find(s) == v_skills.end()) {
            is_compatible = false;
            break;
          }
        }
        _vehicle_to_job_compatibility[v][j] = is_compatible;
      }
    }
  }
}

void Input::set_extra_compatibility() {
  // Derive potential extra incompatibilities : jobs or shipments with
  // amount that does not fit into vehicle or that cannot be added to
  // an empty route for vehicle based on the timing constraints (when
  // they apply).
  for (std::size_t v = 0; v < vehicles.size(); ++v) {
    TWRoute empty_route(*this, v);
    for (Index j = 0; j < jobs.size(); ++j) {
      if (_vehicle_to_job_compatibility[v][j]) {
        bool is_compatible =
          empty_route.is_valid_addition_for_capacity(*this,
                                                     jobs[j].pickup,
                                                     jobs[j].delivery,
                                                     0);

        bool is_shipment_pickup = (jobs[j].type == JOB_TYPE::PICKUP);

        if (is_compatible and _has_TW) {
          if (jobs[j].type == JOB_TYPE::SINGLE) {
            is_compatible = is_compatible &&
                            empty_route.is_valid_addition_for_tw(*this, j, 0);
          } else {
            assert(is_shipment_pickup);
            std::vector<Index> p_d({j, static_cast<Index>(j + 1)});
            is_compatible =
              is_compatible && empty_route.is_valid_addition_for_tw(*this,
                                                                    p_d.begin(),
                                                                    p_d.end(),
                                                                    0,
                                                                    0);
          }
        }

        _vehicle_to_job_compatibility[v][j] = is_compatible;
        if (is_shipment_pickup) {
          // Skipping matching delivery which is next in line in jobs.
          _vehicle_to_job_compatibility[v][j + 1] = is_compatible;
          ++j;
        }
      }
    }
  }
}

void Input::set_vehicles_compatibility() {
  _vehicle_to_vehicle_compatibility =
    std::vector<std::vector<bool>>(vehicles.size(),
                                   std::vector<bool>(vehicles.size(), false));
  for (std::size_t v1 = 0; v1 < vehicles.size(); ++v1) {
    _vehicle_to_vehicle_compatibility[v1][v1] = true;
    for (std::size_t v2 = v1 + 1; v2 < vehicles.size(); ++v2) {
      for (std::size_t j = 0; j < jobs.size(); ++j) {
        if (_vehicle_to_job_compatibility[v1][j] and
            _vehicle_to_job_compatibility[v2][j]) {
          _vehicle_to_vehicle_compatibility[v1][v2] = true;
          _vehicle_to_vehicle_compatibility[v2][v1] = true;
          break;
        }
      }
    }
  }
}

void Input::set_vehicles_costs() {
  for (std::size_t v = 0; v < vehicles.size(); ++v) {
    auto& vehicle = vehicles[v];
    auto search = _matrices.find(vehicle.profile);
    assert(search != _matrices.end());
    vehicle.cost_wrapper.set_durations_matrix(&(search->second));
  }
}

void Input::set_matrices(unsigned nb_thread) {
  if (!_custom_matrices.empty() and !_has_custom_location_index) {
    throw Exception(ERROR::INPUT, "Missing location index.");
  }

  // Split computing matrices across threads based on number of
  // profiles.
  const auto nb_buckets =
    std::min(nb_thread, static_cast<unsigned>(_profiles.size()));

  std::vector<std::vector<std::string>>
    thread_profiles(nb_buckets, std::vector<std::string>());

  std::size_t t_rank = 0;
  for (const auto& profile : _profiles) {
    thread_profiles[t_rank % nb_buckets].push_back(profile);
    ++t_rank;
    if (_custom_matrices.find(profile) == _custom_matrices.end()) {
      // Matrix has not been manually set, create routing wrapper and
      // empty matrix to allow for concurrent modification later on.
      add_routing_wrapper(profile);
      assert(_matrices.find(profile) == _matrices.end());
      _matrices.emplace(profile, Matrix<Cost>());
    }
  }

  std::exception_ptr ep = nullptr;
  std::mutex ep_m;

  auto run_on_profiles = [&](const std::vector<std::string>& profiles) {
    try {
      for (const auto& profile : profiles) {
        auto p_m = _matrices.find(profile);
        assert(p_m != _matrices.end());

        if (p_m->second.size() == 0) {
          // Matrix not manually set so defined as empty above.
          if (_locations.size() == 1) {
            p_m->second = Matrix<Cost>({{0}});
          } else {
            auto rw = std::find_if(_routing_wrappers.begin(),
                                   _routing_wrappers.end(),
                                   [&](const auto& wr) {
                                     return wr->profile == profile;
                                   });
            assert(rw != _routing_wrappers.end());

            if (!_has_custom_location_index) {
              // Location indices are set based on order in _locations.
              p_m->second = (*rw)->get_matrix(_locations);
            } else {
              // Location indices are provided in input so we need an
              // indirection based on order in _locations.
              auto m = (*rw)->get_matrix(_locations);

              Matrix<Cost> full_m(_max_matrices_used_index + 1);
              for (Index i = 0; i < _locations.size(); ++i) {
                const auto& loc_i = _locations[i];
                for (Index j = 0; j < _locations.size(); ++j) {
                  full_m[loc_i.index()][_locations[j].index()] = m[i][j];
                }
              }

              p_m->second = std::move(full_m);
            }
          }
        }

        if (p_m->second.size() <= _max_matrices_used_index) {
          throw Exception(ERROR::INPUT,
                          "location_index exceeding matrix size for " +
                            profile + " profile.");
        }

        // Check for potential overflow in solution cost.
        check_cost_bound(p_m->second);
      }
    } catch (...) {
      ep_m.lock();
      ep = std::current_exception();
      ep_m.unlock();
    }
  };

  std::vector<std::thread> matrix_threads;

  for (const auto& profiles : thread_profiles) {
    matrix_threads.emplace_back(run_on_profiles, profiles);
  }

  for (auto& t : matrix_threads) {
    t.join();
  }

  if (ep != nullptr) {
    std::rethrow_exception(ep);
  }
}

std::unique_ptr<VRP> Input::get_problem() const {
  if (_has_TW) {
    return std::make_unique<VRPTW>(*this);
  } else {
    return std::make_unique<CVRP>(*this);
  }
}

Solution Input::solve(unsigned exploration_level,
                      unsigned nb_thread,
                      const std::vector<HeuristicParameters>& h_param) {
  if (_geometry and !_all_locations_have_coords) {
    // Early abort when info is required with missing coordinates.
    throw Exception(ERROR::INPUT,
                    "Route geometry request with missing coordinates.");
  }

  set_matrices(nb_thread);
  set_vehicles_costs();

  // Fill vehicle/job compatibility matrices.
  set_skills_compatibility();
  set_extra_compatibility();
  set_vehicles_compatibility();

  // Load relevant problem.
  auto instance = get_problem();
  _end_loading = std::chrono::high_resolution_clock::now();

  auto loading = std::chrono::duration_cast<std::chrono::milliseconds>(
                   _end_loading - _start_loading)
                   .count();

  // Solve.
  auto sol = instance->solve(exploration_level, nb_thread, h_param);

  // Update timing info.
  sol.summary.computing_times.loading = loading;

  _end_solving = std::chrono::high_resolution_clock::now();
  sol.summary.computing_times.solving =
    std::chrono::duration_cast<std::chrono::milliseconds>(_end_solving -
                                                          _end_loading)
      .count();

  if (_geometry) {
    for (auto& route : sol.routes) {
      const auto& profile = route.profile;
      auto rw =
        std::find_if(_routing_wrappers.begin(),
                     _routing_wrappers.end(),
                     [&](const auto& wr) { return wr->profile == profile; });
      if (rw == _routing_wrappers.end()) {
        throw Exception(ERROR::INPUT,
                        "Route geometry request with non-routable profile " +
                          profile + ".");
      }
      (*rw)->add_route_info(route);

      sol.summary.distance += route.distance;
    }

    _end_routing = std::chrono::high_resolution_clock::now();
    auto routing = std::chrono::duration_cast<std::chrono::milliseconds>(
                     _end_routing - _end_solving)
                     .count();

    sol.summary.computing_times.routing = routing;
  }

  return sol;
}

Solution Input::check(unsigned nb_thread) {
#if USE_LIBGLPK
  if (_geometry and !_all_locations_have_coords) {
    // Early abort when info is required with missing coordinates.
    throw Exception(ERROR::INPUT,
                    "Route geometry request with missing coordinates.");
  }

  // Set all ranks for vehicles steps.
  std::unordered_set<Id> planned_job_ids;
  std::unordered_set<Id> planned_pickup_ids;
  std::unordered_set<Id> planned_delivery_ids;

  for (Index v = 0; v < vehicles.size(); ++v) {
    auto& current_vehicle = vehicles[v];

    for (auto& step : current_vehicle.steps) {
      if (step.type == STEP_TYPE::BREAK) {
        auto search = current_vehicle.break_id_to_rank.find(step.id);
        if (search == current_vehicle.break_id_to_rank.end()) {
          throw Exception(ERROR::INPUT,
                          "Invalid break id " + std::to_string(step.id) +
                            " for vehicle " +
                            std::to_string(current_vehicle.id) + ".");
        }
        step.rank = search->second;
      }

      if (step.type == STEP_TYPE::JOB) {
        switch (step.job_type) {
        case JOB_TYPE::SINGLE: {
          auto search = job_id_to_rank.find(step.id);
          if (search == job_id_to_rank.end()) {
            throw Exception(ERROR::INPUT,
                            "Invalid job id " + std::to_string(step.id) +
                              " for vehicle " +
                              std::to_string(current_vehicle.id) + ".");
          }
          step.rank = search->second;

          auto planned_job = planned_job_ids.find(step.id);
          if (planned_job != planned_job_ids.end()) {
            throw Exception(ERROR::INPUT,
                            "Duplicate job id " + std::to_string(step.id) +
                              " in input steps for vehicle " +
                              std::to_string(current_vehicle.id) + ".");
          }
          planned_job_ids.insert(step.id);
          break;
        }
        case JOB_TYPE::PICKUP: {
          auto search = pickup_id_to_rank.find(step.id);
          if (search == pickup_id_to_rank.end()) {
            throw Exception(ERROR::INPUT,
                            "Invalid pickup id " + std::to_string(step.id) +
                              " for vehicle " +
                              std::to_string(current_vehicle.id) + ".");
          }
          step.rank = search->second;

          auto planned_pickup = planned_pickup_ids.find(step.id);
          if (planned_pickup != planned_pickup_ids.end()) {
            throw Exception(ERROR::INPUT,
                            "Duplicate pickup id " + std::to_string(step.id) +
                              " in input steps for vehicle " +
                              std::to_string(current_vehicle.id) + ".");
          }
          planned_pickup_ids.insert(step.id);
          break;
        }
        case JOB_TYPE::DELIVERY: {
          auto search = delivery_id_to_rank.find(step.id);
          if (search == delivery_id_to_rank.end()) {
            throw Exception(ERROR::INPUT,
                            "Invalid delivery id " + std::to_string(step.id) +
                              " for vehicle " +
                              std::to_string(current_vehicle.id) + ".");
          }
          step.rank = search->second;

          auto planned_delivery = planned_delivery_ids.find(step.id);
          if (planned_delivery != planned_delivery_ids.end()) {
            throw Exception(ERROR::INPUT,
                            "Duplicate delivery id " + std::to_string(step.id) +
                              " in input steps for vehicle " +
                              std::to_string(current_vehicle.id) + ".");
          }
          planned_delivery_ids.insert(step.id);
          break;
        }
        }
      }
    }
  }

  // TODO we don't need the whole matrix here.
  set_matrices(nb_thread);
  set_vehicles_costs();

  // Fill basic skills compatibility matrix.
  set_skills_compatibility();

  _end_loading = std::chrono::high_resolution_clock::now();

  auto loading = std::chrono::duration_cast<std::chrono::milliseconds>(
                   _end_loading - _start_loading)
                   .count();

  // Check.
  auto sol = validation::check_and_set_ETA(*this, nb_thread);

  // Update timing info.
  sol.summary.computing_times.loading = loading;

  _end_solving = std::chrono::high_resolution_clock::now();
  sol.summary.computing_times.solving =
    std::chrono::duration_cast<std::chrono::milliseconds>(_end_solving -
                                                          _end_loading)
      .count();

  if (_geometry) {
    for (auto& route : sol.routes) {
      const auto& profile = route.profile;
      auto rw =
        std::find_if(_routing_wrappers.begin(),
                     _routing_wrappers.end(),
                     [&](const auto& wr) { return wr->profile == profile; });
      if (rw == _routing_wrappers.end()) {
        throw Exception(ERROR::INPUT,
                        "Route geometry request with non-routable profile " +
                          profile + ".");
      }
      (*rw)->add_route_info(route);

      sol.summary.distance += route.distance;
    }

    _end_routing = std::chrono::high_resolution_clock::now();
    auto routing = std::chrono::duration_cast<std::chrono::milliseconds>(
                     _end_routing - _end_solving)
                     .count();

    sol.summary.computing_times.routing = routing;
  }

  return sol;
#else
  // Attempt to use libglpk while compiling without it.
  throw Exception(ERROR::INPUT, "VROOM compiled without libglpk installed.");
  // Silence -Wunused-parameter warning.
  (void)nb_thread;
#endif
}

} // namespace vroom
