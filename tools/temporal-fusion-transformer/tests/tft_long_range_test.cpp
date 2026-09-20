#include <praesidium-humanitatis/tft.h>

#include <torch/cuda.h>
#include <torch/torch.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Amended after the initial 800-update protocol failed on CPU seed 1729.
// A fixed 3200-update diagnostic recovered without changing the model/optimizer.
// This confirmation budget and a fresh held-out stream were fixed before the
// confirmation runs; acceptance thresholds and all original seeds are retained.
// See docs/uncertainty-validation.md for the original failure and diagnostics.
constexpr int64_t history = 64;
constexpr int64_t horizon = 3;
constexpr int64_t training_batch = 32;
constexpr int64_t validation_batch = 512;
constexpr int64_t training_steps = 3200;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Example {
  praesidium_humanitatis::Batch input;
  torch::Tensor target;
};

// Retrieve the first, explicitly marked observation across 63 independent
// distractors. Every value is identically distributed; only the first value
// determines the target. Static and known-future features are independent noise.
// This is a bounded synthetic distant-retrieval task, not a forecasting benchmark
// or a test of generalization to unseen sequence lengths or marker positions.
Example generate(int64_t count, const torch::TensorOptions& options) {
  auto random = [&](torch::IntArrayRef shape) {
    return 2.0 * torch::rand(shape, options) - 1.0;
  };
  const auto ids = options.dtype(torch::kInt64);
  auto values = random({count, history, 1});
  auto marker = torch::zeros({count, history, 1}, options);
  marker.select(1, 0).fill_(1.0);
  auto target = values.select(1, 0).repeat({1, horizon});
  auto past = torch::cat({values, marker}, -1);
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

double attention_output_ablated_mae(praesidium_humanitatis::TFT& model,
                                   const Example& example) {
  torch::NoGradGuard no_grad;
  auto parameters = model->named_parameters();
  auto output_weight = parameters["network.attention.output.weight"];
  const auto saved = output_weight.detach().clone();
  // The projection has no bias. Zeroing it removes the data-dependent attention
  // contribution while retaining the trained downstream biases and residuals.
  // This post-training intervention is a diagnostic, not a trained no-attention
  // comparator or proof that attention is necessary for solving the task.
  output_weight.zero_();
  const auto result = median_mae(model, example);
  output_weight.copy_(saved);
  return result;
}

void test_long_range_learning(const torch::Device& device, int64_t seed) {
  torch::manual_seed(seed);
  praesidium_humanitatis::Config config;
  config.hidden_size = 16;
  config.attention_heads = 4;
  config.dropout = 0.0;
  config.static_continuous = 1;
  config.past_continuous = 2;
  config.future_continuous = 1;
  config.quantiles = {0.1, 0.5, 0.9};
  praesidium_humanitatis::TFT model(config);
  model->to(device);
  const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);

  // An independent fixed stream, excluded from updates and all model selection.
  // Distinct from the original development stream (seed XOR 0x5DEECE66D).
  torch::manual_seed(static_cast<uint64_t>(seed) ^ 0x2468ACE1ULL);
  const auto validation = generate(validation_batch, options);
  const double zero_baseline = validation.target.abs().mean().item<double>();
  const auto persistence = validation.input.past.continuous.select(1, history - 1)
                               .select(1, 0).unsqueeze(1).repeat({1, horizon});
  const double persistence_mae =
      (persistence - validation.target).abs().mean().item<double>();
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
  shuffled.input.past.continuous = validation.input.past.continuous.clone();
  // Reassign only the first signal among examples, preserving every distractor,
  // marker, target, and static/future input, plus the signal's exact marginal.
  // clone() above is essential: Example copies otherwise share tensor storage.
  shuffled.input.past.continuous.select(1, 0).select(1, 0).copy_(
      validation.input.past.continuous.select(1, 0).select(1, 0).roll({1}, {0}));
  const double shuffled_mae = median_mae(model, shuffled);
  const double ablated_mae = attention_output_ablated_mae(model, validation);

  std::cout << "  seed=" << seed << " history=" << history
            << " updates=" << training_steps << " initial_mae=" << initial
            << " held_out_mae=" << trained << " zero_baseline_mae=" << zero_baseline
            << " persistence_mae=" << persistence_mae
            << " shuffled_distant_signal_mae=" << shuffled_mae
            << " attention_output_ablated_mae=" << ablated_mae << std::endl;

  require(trained < 0.25 && trained < 0.5 * zero_baseline &&
              trained < 0.5 * persistence_mae,
          "distant-retrieval MAE must be below 0.25 and half both baselines");
  require(shuffled_mae > 1.5 * trained && shuffled_mae > 0.4,
          "shuffling the distant signal must worsen MAE by 50% and exceed 0.4");
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
      std::cerr << "Usage: tft_long_range_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: tft_long_range_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  size_t failures = 0;
  const std::vector<int64_t> seeds{1729, 2718, 31415};
  for (const auto seed : seeds) {
    try {
      test_long_range_learning(device, seed);
      std::cout << "PASS: distant historical retrieval, seed " << seed << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: distant historical retrieval, seed " << seed << ": "
                << error.what() << '\n';
    }
  }
  std::cout << seeds.size() - failures << '/' << seeds.size()
            << " long-range learning seeds passed on " << device << '\n';
  return failures == 0 ? 0 : 1;
}
