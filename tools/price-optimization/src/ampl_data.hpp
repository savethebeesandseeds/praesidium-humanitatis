// SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
#pragma once
#include "ph/price/engine.hpp"
namespace ph::price::detail {
// Only numeric request values are serialized; strings cannot become AMPL code.
std::string ampl_data(const Request& request);
}
