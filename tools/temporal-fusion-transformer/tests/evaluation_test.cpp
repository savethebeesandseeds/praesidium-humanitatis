#include <praesidium-humanitatis/evaluation.h>

#include <torch/cuda.h>
#include <torch/torch.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void require_close(double actual, double expected, const std::string& message) {
  require(std::isfinite(actual) && std::abs(actual - expected) < 1e-6, message);
}

void require_close(const torch::Tensor& actual, const torch::Tensor& expected,
                   const std::string& message) {
  require(actual.sizes() == expected.sizes(), message + " (shape)");
  require(torch::allclose(actual, expected, 1e-6, 1e-6), message);
}

template <typename Function>
void require_throws(Function&& function, const std::string& message) {
  bool threw = false;
  try {
    function();
  } catch (const std::exception&) {
    threw = true;
  }
  require(threw, message);
}

void test_hand_computed_metrics(const torch::Device& device) {
  // Four independent errors exercise both sides of pinball and an interval edge.
  for (const auto dtype : {torch::kFloat32, torch::kFloat64}) {
    const auto options = torch::TensorOptions().device(device).dtype(dtype);
    auto predictions = torch::tensor({0., 1., 2., 1., 2., 3.,
                                      -1., 0., 1., 0., 2., 4.}, options)
                           .reshape({2, 2, 3});
    predictions.set_requires_grad(true);
    auto targets = torch::tensor({1., 4., -1., 2.}, options).reshape({2, 2});
    targets.set_requires_grad(true);
    const auto metrics = praesidium_humanitatis::evaluate_forecasts(predictions, targets, {0.1, 0.5, 0.9});
    require_close(metrics.quantile_loss, 0.875, "hand-computed summed pinball");
    require_close(metrics.median_mae, 0.75, "hand-computed median MAE");
    require_close(metrics.interval_coverage, 0.75, "inclusive interval coverage");
    require_close(metrics.interval_width, 2.5, "mean interval width");
    require_close(metrics.crossing_rate, 0.0, "ordered predictions do not cross");
    require_close(metrics.quantile_loss_by_horizon,
                   torch::tensor({0.45, 1.3}, options), "pinball by horizon");
    require_close(metrics.median_mae_by_horizon,
                   torch::tensor({0.5, 1.0}, options), "MAE by horizon");
    require_close(metrics.interval_coverage_by_horizon,
                   torch::tensor({1.0, 0.5}, options), "coverage by horizon");
    for (const auto& tensor : {metrics.quantile_loss_by_horizon,
                              metrics.median_mae_by_horizon,
                              metrics.interval_coverage_by_horizon}) {
      require(tensor.device() == device && tensor.scalar_type() == dtype,
              "per-horizon metrics preserve input device and dtype");
      require(!tensor.requires_grad(), "evaluation must not retain autograd graphs");
    }
    require(!predictions.grad().defined() && !targets.grad().defined(),
            "evaluation must not populate input gradients");
  }
}

void test_crossed_intervals(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  const auto predictions = torch::tensor({0., 3., 2., 4., 5.,
                                          4., 3., 2., 1., 0.,
                                          1., 1., 1., 1., 1.}, options)
                               .reshape({1, 3, 5});
  const auto original = predictions.clone();
  const auto targets = torch::tensor({2., 2., 1.}, options).reshape({1, 3});
  const auto metrics = praesidium_humanitatis::evaluate_forecasts(predictions, targets,
                                               {0.1, 0.25, 0.5, 0.75, 0.9});
  require_close(metrics.crossing_rate, 2.0 / 3.0,
                 "crossing counts internal and outer inversions, but not ties");
  require_close(metrics.interval_width, 1.0 / 3.0,
                 "signed widths expose crossed endpoints");
  require_close(metrics.interval_coverage, 2.0 / 3.0,
                 "inverted outer interval cannot cover target");
  require_close(metrics.median_mae, 0.0, "median uses explicit middle quantile");
  require_close(metrics.quantile_loss_by_horizon,
                 torch::tensor({1.75, 5.1, 0.0}, options),
                 "crossed quantiles retain their raw pinball losses");
  require_close(predictions, original, "evaluation must not mutate or sort predictions");
}

void test_median_position(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  const auto predictions = torch::tensor({0., 2., 3., 4.}, options).reshape({1, 1, 4});
  const auto targets = torch::tensor({2.}, options).reshape({1, 1});
  const auto metrics = praesidium_humanitatis::evaluate_forecasts(predictions, targets, {0.1, 0.5, 0.7, 0.9});
  require_close(metrics.median_mae, 0.0, "median is located by quantile value");
}

void test_invalid_metrics(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  const auto predictions = torch::zeros({2, 3, 3}, options);
  const auto targets = torch::zeros({2, 3}, options);
  const std::vector<double> quantiles{0.1, 0.5, 0.9};
  const auto evaluate = [&](const torch::Tensor& p, const torch::Tensor& t) {
    praesidium_humanitatis::evaluate_forecasts(p, t, quantiles);
  };
  require_throws([&] { evaluate({}, targets); }, "reject undefined predictions");
  require_throws([&] { evaluate(predictions, {}); }, "reject undefined targets");
  require_throws([&] { evaluate(predictions.flatten(), targets); }, "reject prediction rank");
  require_throws([&] { evaluate(predictions, targets.unsqueeze(-1)); }, "reject target rank");
  require_throws([&] { evaluate(predictions, torch::zeros({1, 3}, options)); },
                 "reject broadcastable batch mismatch");
  require_throws([&] { evaluate(predictions, torch::zeros({2, 1}, options)); },
                 "reject broadcastable horizon mismatch");
  require_throws([&] { evaluate(torch::empty({0, 3, 3}, options),
                                torch::empty({0, 3}, options)); }, "reject zero batch");
  require_throws([&] { evaluate(torch::empty({2, 0, 3}, options),
                                torch::empty({2, 0}, options)); }, "reject zero horizon");
  require_throws([&] { evaluate(torch::empty({2, 3, 0}, options), targets); },
                 "reject zero quantile dimension");
  require_throws([&] { evaluate(predictions.to(torch::kInt64), targets.to(torch::kInt64)); },
                 "reject integer predictions");
  require_throws([&] { evaluate(predictions.to(torch::kFloat16), targets.to(torch::kFloat16)); },
                 "reject half precision");
  require_throws([&] { evaluate(predictions, targets.to(torch::kFloat64)); },
                 "reject mixed dtypes");
  require_throws([&] { evaluate(predictions.to_sparse(), targets); }, "reject sparse predictions");
  require_throws([&] { evaluate(predictions, targets.to_sparse()); }, "reject sparse targets");
  require_throws([&] { evaluate(torch::full_like(predictions,
                      std::numeric_limits<double>::quiet_NaN()), targets); },
                 "reject NaN prediction");
  require_throws([&] { evaluate(predictions, torch::full_like(targets,
                      std::numeric_limits<double>::infinity())); }, "reject infinite target");
  for (const auto& invalid : std::vector<std::vector<double>>{
           {}, {0.5}, {0.1, 0.9}, {0.1, 0.4, 0.9}, {0.5, 0.7, 0.9},
           {0.1, 0.3, 0.5}, {0.1, 0.5, 0.5}, {0.5, 0.1, 0.9},
           {0.0, 0.5, 0.9}, {0.1, 0.5, 1.0},
           {0.1, 0.5, std::numeric_limits<double>::infinity()},
           {0.1, 0.5, std::numeric_limits<double>::quiet_NaN()}}) {
    require_throws([&] {
      praesidium_humanitatis::evaluate_forecasts(torch::zeros({2, 3, static_cast<int64_t>(invalid.size())}, options),
                               targets, invalid);
    }, "reject invalid or incomplete quantile levels");
  }
  if (device.is_cuda()) {
    require_throws([&] { evaluate(predictions, targets.cpu()); }, "reject mixed devices");
  }
}

void test_persistence(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat64);
  auto last = torch::tensor({-2., 0., 3.}, options);
  last.set_requires_grad(true);
  auto predictions = praesidium_humanitatis::persistence_forecast(last, 2, 3);
  const auto expected = torch::tensor({-2., -2., -2., -2., -2., -2.,
                                       0., 0., 0., 0., 0., 0.,
                                       3., 3., 3., 3., 3., 3.}, options).reshape({3, 2, 3});
  require_close(predictions, expected, "repeat only last observation across forecast horizon");
  require(predictions.device() == device && predictions.scalar_type() == torch::kFloat64,
          "persistence preserves device and dtype");
  require(!predictions.requires_grad(), "persistence has no autograd history");
  const auto targets = last.detach().unsqueeze(1).expand({3, 2});
  const auto metrics = praesidium_humanitatis::evaluate_forecasts(predictions, targets, {0.1, 0.5, 0.9});
  require_close(metrics.quantile_loss, 0.0, "exact persistence pinball");
  require_close(metrics.interval_width, 0.0, "persistence has degenerate intervals");
  require_close(metrics.interval_coverage, 1.0, "degenerate intervals include exact targets");
  require_close(metrics.crossing_rate, 0.0, "equal quantiles are not crossed");
  predictions.fill_(100.0);
  require_close(last.detach(), torch::tensor({-2., 0., 3.}, options),
                 "returned forecasts own storage independently of input");
  require_close(praesidium_humanitatis::persistence_forecast(last.detach().slice(0, 0, 3, 2), 1, 1),
                 torch::tensor({-2., 3.}, options).reshape({2, 1, 1}),
                 "noncontiguous observations and one quantile are supported");
}

void test_metric_overflow(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  require_throws([&] {
    praesidium_humanitatis::evaluate_forecasts(torch::full({1, 1, 3}, 3e38, options),
                              torch::full({1, 1}, -3e38, options), {0.1, 0.5, 0.9});
  }, "reject finite inputs whose errors overflow");
  require_throws([&] {
    praesidium_humanitatis::evaluate_forecasts(torch::tensor({-3e38, 0., 3e38}, options).reshape({1, 1, 3}),
                              torch::zeros({1, 1}, options), {0.1, 0.5, 0.9});
  }, "reject finite inputs whose interval width overflows");
}

void test_invalid_persistence(const torch::Device& device) {
  const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
  const auto last = torch::zeros({2}, options);
  require_throws([&] { praesidium_humanitatis::persistence_forecast({}, 2, 3); }, "reject undefined observations");
  require_throws([&] { praesidium_humanitatis::persistence_forecast(last.unsqueeze(1), 2, 3); },
                 "reject rank-two observations");
  require_throws([&] { praesidium_humanitatis::persistence_forecast(torch::empty({0}, options), 2, 3); },
                 "reject empty observations");
  require_throws([&] { praesidium_humanitatis::persistence_forecast(last.to(torch::kInt64), 2, 3); },
                 "reject integer observations");
  require_throws([&] { praesidium_humanitatis::persistence_forecast(last.to_sparse(), 2, 3); },
                 "reject sparse observations");
  require_throws([&] { praesidium_humanitatis::persistence_forecast(torch::full_like(last,
                          std::numeric_limits<double>::infinity()), 2, 3); },
                 "reject nonfinite observations");
  for (const auto dimensions : std::vector<std::pair<int64_t, int64_t>>{
           {0, 3}, {-1, 3}, {2, 0}, {2, -1},
           {std::numeric_limits<int64_t>::max(), 3},
           {std::numeric_limits<int64_t>::max(), 1}}) {
    require_throws([&] { praesidium_humanitatis::persistence_forecast(last, dimensions.first, dimensions.second); },
                   "reject invalid forecast dimensions");
  }
}

}  // namespace

int main(int argc, char** argv) {
  torch::Device device(torch::kCPU);
  if (argc == 3 && std::string(argv[1]) == "--device") {
    const std::string requested(argv[2]);
    if (requested == "cuda") {
      if (!torch::cuda::is_available()) {
        std::cout << "SKIP: CUDA requested, but no CUDA device is available\n";
        return 77;
      }
      device = torch::Device(torch::kCUDA, 0);
    } else if (requested != "cpu") {
      std::cerr << "Usage: evaluation_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: evaluation_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"hand-computed metrics", [&] { test_hand_computed_metrics(device); }},
      {"crossed intervals", [&] { test_crossed_intervals(device); }},
      {"explicit median position", [&] { test_median_position(device); }},
      {"metric input validation", [&] { test_invalid_metrics(device); }},
      {"metric arithmetic overflow", [&] { test_metric_overflow(device); }},
      {"persistence baseline", [&] { test_persistence(device); }},
      {"persistence input validation", [&] { test_invalid_persistence(device); }},
  };
  int failures = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      std::cout << "PASS: " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size()
            << " tests passed on " << device << '\n';
  return failures == 0 ? 0 : 1;
}
