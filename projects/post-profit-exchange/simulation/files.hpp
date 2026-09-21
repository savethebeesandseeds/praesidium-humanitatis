// SPDX-License-Identifier: MIT
#pragma once
namespace ph::exchange::files {
// Native CLI only; the standalone browser has no filesystem access.
int cli(int argc, char** argv);
}
