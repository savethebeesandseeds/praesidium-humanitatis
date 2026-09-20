// SPDX-License-Identifier: MIT
#include "simulation.hpp"
#include <iostream>
#include <iterator>
int main() {
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  std::cout << ph::exchange::run_json(input) << '\n';
}
