#include "praesidium-humanitatis/calibration.h"
#include "praesidium-humanitatis/checkpoint.h"
#include "praesidium-humanitatis/evaluation.h"

#include <torch/cuda.h>
#include <torch/version.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace praesidium_humanitatis;
constexpr int64_t history = 16, horizon = 4, batch_size = 32;
constexpr int64_t calibration_size = 1024, test_size = 4096;
constexpr double noise = 0.03, miscoverage = 0.2;
const std::vector<double> quantiles{0.1, 0.5, 0.9};

struct Options {
  std::string device = "cpu", output;
  int64_t seed = 20260916, steps = 800;
  bool ordered = false;
};

Options parse(int argc, char** argv) {
  Options o;
  std::set<std::string> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!seen.insert(arg).second) throw std::invalid_argument("Duplicate argument: " + arg);
    if (arg == "--ordered-quantiles") { o.ordered = true; continue; }
    if (arg != "--device" && arg != "--seed" && arg != "--steps" && arg != "--output-dir")
      throw std::invalid_argument("Unknown argument: " + arg);
    if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
    const std::string value = argv[i];
    if (arg == "--device") o.device = value;
    if (arg == "--output-dir") o.output = value;
    if (arg == "--seed" || arg == "--steps") {
      size_t used = 0;
      const auto n = std::stoll(value, &used);
      if (used != value.size() || n < 1 || n > (arg == "--steps" ? 100000 : 1000000000))
        throw std::invalid_argument("Invalid integer for " + arg);
      if (arg == "--seed") o.seed = n; else o.steps = n;
    }
  }
  if (o.output.empty() || (o.device != "cpu" && o.device != "cuda"))
    throw std::invalid_argument("Usage: tft_uncertainty --output-dir NEW_DIR [--device cpu|cuda] [--seed N] [--steps N] [--ordered-quantiles]");
  return o;
}

struct Example { Batch input; torch::Tensor target, mean; };

Example generate(int64_t count, const torch::TensorOptions& options) {
  auto amplitude = 0.5 + torch::rand({count, 1}, options);
  auto phase = 6.283185307179586 * torch::rand({count, 1}, options);
  auto time = torch::arange(history + horizon, options).unsqueeze(0) * 0.25 + phase;
  auto mean = amplitude * time.sin();
  auto observed = mean + noise * torch::randn({count, history + horizon}, options);
  auto past = torch::stack({observed.slice(1, 0, history), time.slice(1, 0, history).sin(),
                            time.slice(1, 0, history).cos()}, -1);
  auto future = torch::stack({time.slice(1, history).sin(), time.slice(1, history).cos()}, -1);
  auto ids = options.dtype(torch::kInt64);
  return {{{amplitude, torch::empty({count, 0}, ids)},
           {past, torch::empty({count, history, 0}, ids)},
           {future, torch::empty({count, horizon, 0}, ids)}},
          observed.slice(1, history), mean.slice(1, history)};
}

PredictionInterval interval(const torch::Tensor& prediction) {
  return {prediction.select(-1, 0), prediction.select(-1, 2)};
}

torch::Tensor predict(TFT& model, const Example& example) {
  torch::NoGradGuard guard;
  model->eval();
  // Bound inference memory independently of the held-out sample count.
  std::vector<torch::Tensor> results;
  for (int64_t offset = 0; offset < example.target.size(0); offset += 128) {
    auto slice = [&](const FeatureBatch& f) {
      return FeatureBatch{f.continuous.slice(0, offset, offset + 128),
                          f.categorical.slice(0, offset, offset + 128)};
    };
    results.push_back(model->forward({slice(example.input.statics), slice(example.input.past),
                                      slice(example.input.future)}).predictions);
  }
  return torch::cat(results);
}

void values(std::ostream& out, const torch::Tensor& tensor) {
  const auto flattened = tensor.to(torch::kCPU).to(torch::kFloat64).flatten();
  out << '[';
  for (int64_t i = 0; i < flattened.numel(); ++i) {
    if (i) out << ',';
    out << flattened[i].item<double>();
  }
  out << ']';
}

void forecast_json(std::ostream& out, const torch::Tensor& prediction, const Example& test) {
  const auto m = evaluate_forecasts(prediction, test.target, quantiles);
  out << "{\"quantile_loss\":" << m.quantile_loss << ",\"median_mae\":" << m.median_mae
      << ",\"coverage\":" << m.interval_coverage << ",\"width\":" << m.interval_width
      << ",\"crossing_rate\":" << m.crossing_rate << ",\"coverage_by_horizon\":";
  values(out, m.interval_coverage_by_horizon);
  out << ",\"median_mae_by_horizon\":"; values(out, m.median_mae_by_horizon);
  out << ",\"quantile_empirical_cdf\":";
  // These correspond to requested quantile probabilities; CQR does not modify
  // them or turn calibrated endpoints into new quantile forecasts.
  values(out, test.target.unsqueeze(-1).le(prediction).to(torch::kFloat64).mean(std::vector<int64_t>{0, 1}));
  out << '}';
}

void print(const char* name, const torch::Tensor& prediction, const Example& test) {
  const auto m = evaluate_forecasts(prediction, test.target, quantiles);
  std::cout << name << " mae=" << m.median_mae << " pinball=" << m.quantile_loss
            << " coverage=" << m.interval_coverage << " width=" << m.interval_width
            << " crossing=" << m.crossing_rate << std::endl;
}
} // namespace

int main(int argc, char** argv) {
  try {
    const auto o = parse(argc, argv);
    if (o.device == "cuda" && !torch::cuda::is_available())
      throw std::runtime_error("CUDA requested but unavailable");
    const auto directory = std::filesystem::path(o.output);
    if (directory.has_parent_path()) std::filesystem::create_directories(directory.parent_path());
    if (!std::filesystem::create_directory(directory))
      throw std::runtime_error("Output directory already exists; existing experiments are preserved");
    const torch::Device device(o.device);
    torch::set_num_threads(1);
    torch::manual_seed(o.seed);
    Config c;
    c.past_continuous = 3;
    c.future_continuous = 2;
    c.ordered_quantiles = o.ordered;
    TFT model(c);
    model->to(device);
    torch::optim::Adam optimizer(model->parameters(), torch::optim::AdamOptions(0.003));
    const auto options = torch::TensorOptions().device(device).dtype(torch::kFloat32);
    // Splits are independent windows. Neither calibration nor test labels are
    // used for optimization, hyperparameter choice, or early stopping. The
    // 80-step checkpoint is a predeclared comparison, not a selection criterion.
    const uint64_t calibration_seed = static_cast<uint64_t>(o.seed) ^ 0xC411B4A7ULL;
    const uint64_t test_seed = static_cast<uint64_t>(o.seed) ^ 0x7E57B10CULL;
    torch::manual_seed(calibration_seed);
    const auto calibration = generate(calibration_size, options);
    torch::manual_seed(test_seed);
    const auto test = generate(test_size, options);
    torch::Tensor at80;
    std::cout << "device=" << o.device << " seed=" << o.seed << " ordered=" << o.ordered
              << " fixed_updates=" << o.steps << std::endl;
    for (int64_t step = 0; step < o.steps; ++step) {
      // Matches the existing demo's update protocol. Matched raw/ordered runs
      // share initial raw projection weights, training batches and dropout RNG.
      torch::manual_seed(o.seed + step);
      model->train();
      const auto sample = generate(batch_size, options);
      optimizer.zero_grad();
      const auto loss = quantile_loss(model->forward(sample.input).predictions, sample.target, quantiles);
      loss.backward();
      const auto norm = torch::nn::utils::clip_grad_norm_(model->parameters(), 1.0);
      TORCH_CHECK(std::isfinite(norm), "Nonfinite training gradient");
      optimizer.step();
      if (step + 1 == 80) {
        at80 = predict(model, test);
        print("test_at_80", at80, test);
      }
      if ((step + 1) % 200 == 0) std::cout << "step=" << step + 1 << " train_loss=" << loss.item<double>() << std::endl;
    }
    const auto prediction = predict(model, test);
    const auto cal_prediction = predict(model, calibration);
    print("test_final", prediction, test);
    if (o.ordered) {
      TORCH_CHECK(evaluate_forecasts(prediction, test.target, quantiles).crossing_rate == 0.0,
                  "Ordered head produced crossing quantiles");
    }
    // A Gaussian oracle is available only because the generator is known.
    const auto q = torch::tensor(quantiles, options);
    const auto oracle = test.mean.unsqueeze(-1) + noise * std::sqrt(2.0) * torch::erfinv(2.0 * q - 1.0);
    const auto persistence = persistence_forecast(test.input.past.continuous.select(1, history - 1).select(1, 0), horizon, 3);
    print("gaussian_oracle", oracle, test);
    // Raw crossing outputs remain visible. Calibration rejects reversed outer
    // endpoints; it never sorts or silently repairs an independent-head model.
    const bool can_calibrate = interval(cal_prediction).lower.le(interval(cal_prediction).upper).all().item<bool>() &&
                               interval(prediction).lower.le(interval(prediction).upper).all().item<bool>();
    CQRCalibration fitted;
    IntervalMetrics calibrated_metrics{};
    if (can_calibrate) {
      fitted = fit_cqr(interval(cal_prediction), calibration.target, miscoverage);
      calibrated_metrics = evaluate_intervals(apply_cqr(interval(prediction), fitted), test.target);
      std::cout << "calibrated_test coverage=" << calibrated_metrics.coverage
                << " width=" << calibrated_metrics.mean_width << std::endl;
    } else std::cout << "calibration unavailable: reversed outer endpoints (raw results retained)" << std::endl;

    TrainingProgress progress;
    progress.completed_steps = o.steps;
    progress.seed = o.seed;
    progress.data_id = "praesidium-humanitatis.synthetic.sine.v1";
    save_checkpoint((directory / "model.pt").string(), model, optimizer, progress);
    std::ofstream out(directory / "report.json");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::setprecision(12) << "{\"format_version\":1,\"data_id\":\"praesidium-humanitatis.synthetic.sine.v1\",\"noise_std\":" << noise
        << ",\"libtorch\":\"" << TORCH_VERSION
        << "\",\"device\":\"" << o.device << "\",\"seed\":" << o.seed
        << ",\"ordered_quantiles\":" << (o.ordered ? "true" : "false")
        << ",\"completed_steps\":" << o.steps << ",\"history\":" << history << ",\"horizon\":" << horizon
        << ",\"batch_size\":" << batch_size << ",\"calibration_size\":" << calibration_size
        << ",\"test_size\":" << test_size << ",\"calibration_seed\":" << calibration_seed
        << ",\"test_seed\":" << test_seed << ",\"miscoverage\":" << miscoverage
        << ",\"quantiles\":[0.1,0.5,0.9],\"test_at_80\":";
    if (at80.defined()) forecast_json(out, at80, test); else out << "null";
    out << ",\"test_final\":"; forecast_json(out, prediction, test);
    out << ",\"gaussian_oracle\":"; forecast_json(out, oracle, test);
    out << ",\"persistence\":"; forecast_json(out, persistence, test);
    out << ",\"calibrated_interval\":";
    if (can_calibrate) {
      out << "{\"method\":\"outward_only_split_cqr_per_horizon\",\"correction\":";
      values(out, fitted.correction);
      out << ",\"coverage\":" << calibrated_metrics.coverage << ",\"width\":" << calibrated_metrics.mean_width
          << ",\"coverage_by_horizon\":"; values(out, calibrated_metrics.coverage_by_horizon);
      out << ",\"width_by_horizon\":"; values(out, calibrated_metrics.width_by_horizon);
      out << '}';
    } else out << "null";
    out << "}\n";
    out.close();
    std::cout << "report=" << (directory / "report.json") << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "tft_uncertainty: " << e.what() << '\n';
    return 1;
  }
}
