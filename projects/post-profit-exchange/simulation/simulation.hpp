// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace ph::exchange {
// Strict JSON command interface, shared by the native CLI and WebAssembly.
std::string run_json(const std::string& command);
}
extern "C" const char* exchange_run(const char* command);
