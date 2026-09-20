// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
#include "ph/price/engine.hpp"

namespace ph::price::example {
inline Request synthetic_request(Timestamp now) {
  Request request;
  request.request_id = "synthetic-cooperative-001";
  request.currency = "EUR";
  request.forecast_source = "synthetic fixture; no empirical elasticity claim";
  request.worker_policy_reference = "synthetic worker assembly resolution 001";
  request.as_of = now;
  request.valid_until = now + 300;
  request.max_input_age_seconds = 120;
  request.worker_wage_floor = 800;
  request.operating_cost = 100;
  request.reserve_floor = 100;
  request.scenarios = {{"usual", 0.6}, {"low-demand", 0.4}};
  request.products = {
      {"bread", 100, 200, 220, 20, 1000,
       {{180, {20, 14}}, {200, {16, 12}}, {220, {12, 8}}, {240, {20, 18}}}},
      {"beans", 150, 300, 330, 15, 1000,
       {{270, {13, 9}}, {300, {12, 8}}, {330, {8, 5}}}}
  };
  return request;
}
}
