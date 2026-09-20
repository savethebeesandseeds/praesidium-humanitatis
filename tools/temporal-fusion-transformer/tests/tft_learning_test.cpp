#include <praesidium-humanitatis/tft.h>

#include <torch/cuda.h>
#include <torch/torch.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int64_t history = 6;
constexpr int64_t horizon = 3;
constexpr int64_t training_batch = 32;
constexpr int64_t validation_batch = 256;
constexpr int64_t training_steps = 300;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Example {
  praesidium_humanitatis::Batch input;
  torch::Tensor target;
};

// The target is present only in historical observations. Static and known-future
// covariates are independent noise, so the sine demo's direct future-feature
// shortcut is unavailable. This is a history-use sanity task: persistence solves
// it exactly, and success does not demonstrate long-range memory or superiority
// over a forecasting baseline.
Example generate(int64_t count, const torch::TensorOptions& options) {
  auto random = [&](torch::IntArrayRef shape) {
    return 2.0 * torch::rand(shape, options) - 1.0;
  };
  const auto ids = options.dtype(torch::kInt64);
  auto past = random({count, history, 1});
  auto target = past.select(1, history - 1).repeat({1, horizon});
  return {{{random({count, 1}), torch::empty({count, 0}, ids)},
           {past, torch::empty({count, history, 0}, ids)},
           {random({count, horizon, 1}), torch::empty({count, horizon, 0}, ids)}},
          target};
}

double median_mae(praesidium_humanitatis::TFT& model, const Example& example) {
  torch::NoGradGuard no_grad;
  model->eval();
  const auto predictions = model->forward(example.input).predictions;
  require(torch::isfinite(predictions).all().item<bool>(),
          "held-out predictions must be finite");
  return (predictions.select(-1, 1) - example.target).abs().mean().item<double>();
}

void test_history_learning(const torch::Device& device, int64_t seed) {
  torch::manual_seed(seed);
  praesidium_humanitatis::Config config;
  config.hidden_size = 16;
  config.attention_heads = 4;
  config.dropout = 0.0;
  config.static_continuous = 1;
  config.past_continuous = 1;
  config.future_continuous = 1;
  config.quantiles = {0.1, 0.5, 0.9};
  praesidium_humanitatis::TFT model(config);
  model->to(device);
  const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);

  // Separate fixed validation stream. It is never used for optimizer updates,
  // early stopping, seed selection, or choosing the acceptance thresholds below.
  torch::manual_seed(static_cast<uint64_t>(seed) ^ 0x5DEECE66DULL);
  const auto validation = generate(validation_batch, options);
  const double baseline = validation.target.abs().mean().item<double>();
  const double initial = median_mae(model, validation);
  torch::manual_seed(seed + 10000);
  torch::optim::Adam optimizer(model->parameters(), torch::optim::AdamOptions(0.003));
  model->train();
  for (int64_t step = 0; step < training_steps; ++step) {
    const auto sample = generate(training_batch, options);
    optimizer.zero_grad();
    auto loss = praesidium_humanitatis::quantile_loss(
        model->forward(sample.input).predictions, sample.target, config.quantiles);
    require(torch::isfinite(loss).item<bool>(), "training loss must be finite");
    loss.backward();
    const double norm = torch::nn::utils::clip_grad_norm_(model->parameters(), 1.0);
    require(std::isfinite(norm), "training gradient norm must be finite");
    optimizer.step();
  }

  const double trained = median_mae(model, validation);
  auto shuffled = validation;
  // Keep targets/static/future fixed and pair each sample with another sample's
  // entire historical sequence; this preserves the input's marginal distribution.
  shuffled.input.past.continuous = validation.input.past.continuous.roll({1}, {0});
  const double shuffled_mae = median_mae(model, shuffled);
  std::cout << "  seed=" << seed << " updates=" << training_steps
            << " initial_mae=" << initial << " held_out_mae=" << trained
            << " zero_baseline_mae=" << baseline
            << " shuffled_history_mae=" << shuffled_mae << std::endl;

  // These margins and the three seeds were fixed before running the experiment.
  require(trained < 0.25 && trained < 0.5 * baseline,
          "held-out history-copy MAE must be below 0.25 and half the zero baseline");
  require(shuffled_mae > 1.5 * trained,
          "shuffling history must worsen held-out MAE by at least 50 percent");
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
      std::cerr << "Usage: tft_learning_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: tft_learning_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  size_t failures = 0;
  const std::vector<int64_t> seeds{1729, 2718, 31415};
  for (const auto seed : seeds) {
    try {
      test_history_learning(device, seed);
      std::cout << "PASS: held-out history learning, seed " << seed << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: held-out history learning, seed " << seed << ": "
                << error.what() << '\n';
    }
  }
  std::cout << seeds.size() - failures << '/' << seeds.size()
            << " held-out learning seeds passed on " << device << '\n';
  return failures == 0 ? 0 : 1;
}
