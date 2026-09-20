#include <praesidium-humanitatis/tft.h>

#include <torch/cuda.h>
#include <torch/torch.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using torch::indexing::Slice;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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

void require_close(const torch::Tensor& actual, const torch::Tensor& expected,
                   const std::string& message, double atol = 1e-5,
                   double rtol = 1e-5) {
  require(actual.sizes() == expected.sizes(), message + " (shape)");
  require(torch::allclose(actual, expected, rtol, atol), message);
}

void require_finite(const torch::Tensor& tensor, const std::string& message) {
  require(tensor.defined(), message + " (undefined)");
  require(torch::isfinite(tensor).all().item<bool>(), message);
}

praesidium_humanitatis::Config make_config() {
  praesidium_humanitatis::Config config;
  config.hidden_size = 8;
  config.attention_heads = 2;
  config.dropout = 0.0;
  config.static_continuous = 1;
  config.past_continuous = 2;
  config.future_continuous = 1;
  config.static_categorical_cardinalities = {3};
  config.past_categorical_cardinalities = {4};
  config.future_categorical_cardinalities = {5};
  config.quantiles = {0.1, 0.5, 0.9};
  return config;
}

torch::Tensor categories(const std::vector<int64_t>& cardinalities,
                         const std::vector<int64_t>& leading_shape,
                         const torch::Device& device) {
  const auto options = torch::TensorOptions().dtype(torch::kInt64).device(device);
  std::vector<torch::Tensor> columns;
  for (const auto cardinality : cardinalities) {
    columns.push_back(torch::randint(cardinality, leading_shape, options));
  }
  if (!columns.empty()) {
    return torch::stack(columns, -1);
  }
  auto shape = leading_shape;
  shape.push_back(0);
  return torch::empty(shape, options);
}

praesidium_humanitatis::Batch make_batch(const praesidium_humanitatis::Config& config, const torch::Device& device,
                       int64_t batch_size = 6, int64_t encoder_length = 4,
                       int64_t horizon = 3) {
  const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
  praesidium_humanitatis::Batch batch;
  batch.statics.continuous =
      torch::randn({batch_size, config.static_continuous}, options);
  batch.statics.categorical =
      categories(config.static_categorical_cardinalities, {batch_size}, device);
  batch.past.continuous =
      torch::randn({batch_size, encoder_length, config.past_continuous}, options);
  batch.past.categorical = categories(config.past_categorical_cardinalities,
                                      {batch_size, encoder_length}, device);
  batch.future.continuous =
      torch::randn({batch_size, horizon, config.future_continuous}, options);
  batch.future.categorical = categories(config.future_categorical_cardinalities,
                                        {batch_size, horizon}, device);
  return batch;
}

praesidium_humanitatis::TFT make_model(const praesidium_humanitatis::Config& config, const torch::Device& device) {
  praesidium_humanitatis::TFT model(config);
  model->to(device);
  return model;
}

void check_probabilities(const torch::Tensor& weights,
                         const std::string& name) {
  require_finite(weights, name + " must be finite");
  require((weights >= 0).all().item<bool>(), name + " must be nonnegative");
  require_close(weights.sum(-1), torch::ones_like(weights.sum(-1)),
                name + " must sum to one");
}

void test_shapes_and_interpretability(const torch::Device& device) {
  const auto config = make_config();
  const auto batch = make_batch(config, device);
  auto model = make_model(config, device);
  model->eval();
  torch::NoGradGuard no_grad;
  const auto output = model->forward(batch);
  require(output.predictions.sizes() == torch::IntArrayRef({6, 3, 3}),
          "prediction shape must be batch, horizon, quantile");
  require(output.static_weights.sizes() == torch::IntArrayRef({6, 2}),
          "static variable selection shape");
  require(output.past_weights.sizes() == torch::IntArrayRef({6, 4, 3}),
          "past variable selection shape");
  require(output.future_weights.sizes() == torch::IntArrayRef({6, 3, 2}),
          "future variable selection shape");
  require(output.attention_weights.sizes() == torch::IntArrayRef({6, 2, 3, 7}),
          "attention shape must expose decoder queries and all keys");
  require(output.predictions.device() == device, "predictions must stay on device");
  require_finite(output.predictions, "predictions must be finite");
  check_probabilities(output.static_weights, "static selection");
  check_probabilities(output.past_weights, "past selection");
  check_probabilities(output.future_weights, "future selection");
  check_probabilities(output.attention_weights, "attention");
  for (int64_t query = 0; query < 2; ++query) {
    const auto forbidden = output.attention_weights.index(
        {Slice(), Slice(), query, Slice(4 + query + 1, 7)});
    require(forbidden.abs().max().item<double>() == 0.0,
            "future attention keys must have exactly zero probability");
  }
}

void test_causality(const torch::Device& device) {
  const auto config = make_config();
  auto model = make_model(config, device);
  model->eval();
  const auto batch = make_batch(config, device);
  auto perturbed = batch;
  perturbed.future.continuous = batch.future.continuous.clone();
  perturbed.future.categorical = batch.future.categorical.clone();
  perturbed.future.continuous.index_put_({Slice(), 2, Slice()}, 100.0);
  perturbed.future.categorical.index_put_(
      {Slice(), 2, Slice()},
      torch::remainder(batch.future.categorical.index({Slice(), 2, Slice()}) + 1, 5));
  torch::NoGradGuard no_grad;
  const auto baseline = model->forward(batch);
  const auto changed = model->forward(perturbed);
  require_close(changed.predictions.index({Slice(), Slice(0, 2), Slice()}),
                baseline.predictions.index({Slice(), Slice(0, 2), Slice()}),
                "a later future covariate must not affect earlier predictions");
  require(!torch::allclose(changed.predictions.index({Slice(), 2, Slice()}),
                           baseline.predictions.index({Slice(), 2, Slice()}),
                           1e-5, 1e-6),
          "future covariates must actually influence their prediction");
}

void test_interpretable_attention(const torch::Device& device) {
  const auto config = make_config();
  auto model = make_model(config, device);
  model->eval();
  const auto batch = make_batch(config, device);
  torch::NoGradGuard no_grad;
  auto parameters = model->named_parameters();
  size_t value_projections = 0;
  for (const auto& parameter : parameters) {
    if (parameter.key().find("network.attention.value") == 0 &&
        parameter.key().find("weight") != std::string::npos) {
      ++value_projections;
      require(parameter.value().sizes() ==
                  torch::IntArrayRef({config.hidden_size / config.attention_heads,
                                      config.hidden_size}),
              "attention must share a value projection of head width");
    }
  }
  require(value_projections == 1, "attention must register exactly one value projection");

  const auto baseline = model->forward(batch);
  // Swapping a whole head only changes its position in the returned diagnostic
  // tensor: the forecast must be invariant to the order of averaged heads.
  for (const std::string projection : {"query_", "key_"}) {
    const auto first_name = "network.attention." + projection + "0.weight";
    const auto second_name = "network.attention." + projection + "1.weight";
    const auto original_first = parameters[first_name].clone();
    parameters[first_name].copy_(parameters[second_name]);
    parameters[second_name].copy_(original_first);
  }
  const auto permuted = model->forward(batch);
  require_close(permuted.predictions, baseline.predictions,
                "averaged attention must be invariant to head permutation");
  require_close(permuted.attention_weights.select(1, 0),
                baseline.attention_weights.select(1, 1),
                "permuting heads must swap their returned weights");
  require_close(permuted.attention_weights.select(1, 1),
                baseline.attention_weights.select(1, 0),
                "permuting heads must preserve each head's attention");

  for (int64_t head = 0; head < config.attention_heads; ++head) {
    for (const std::string projection : {"query_", "key_"}) {
      parameters["network.attention." + projection + std::to_string(head) + ".weight"].zero_();
    }
  }
  const auto uniform = model->forward(batch);
  const int64_t history = batch.past.continuous.size(1);
  const int64_t horizon = batch.future.continuous.size(1);
  const int64_t total = history + horizon;
  for (int64_t query = 0; query < horizon; ++query) {
    const int64_t permitted = history + query + 1;
    const auto weights = uniform.attention_weights.select(2, query);
    auto expected = torch::zeros_like(weights);
    expected.index_put_({Slice(), Slice(), Slice(0, permitted)},
                        1.0 / static_cast<double>(permitted));
    require_close(weights, expected,
                  "zero query/key projections must give uniform causal attention");
    if (permitted < total) {
      require(weights.index({Slice(), Slice(), Slice(permitted, total)})
                      .abs().max().item<double>() == 0.0,
              "uniform attention must still exclude future keys exactly");
    }
  }
}

void test_feature_groups(const torch::Device& device) {
  auto continuous_config = make_config();
  continuous_config.static_categorical_cardinalities.clear();
  continuous_config.past_categorical_cardinalities.clear();
  continuous_config.future_categorical_cardinalities.clear();
  auto categorical_config = make_config();
  categorical_config.static_continuous = 0;
  categorical_config.past_continuous = 0;
  categorical_config.future_continuous = 0;
  torch::NoGradGuard no_grad;
  for (const auto& config : {continuous_config, categorical_config}) {
    auto model = make_model(config, device);
    model->eval();
    const auto output = model->forward(make_batch(config, device, 1, 1, 1));
    require(output.predictions.sizes() == torch::IntArrayRef({1, 1, 3}),
            "single timestep and zero-width unused feature types must work");
    require_finite(output.predictions, "single timestep output must be finite");
    check_probabilities(output.static_weights, "single static selection");
  }
}

void test_invalid_config() {
  const auto valid = make_config();
  valid.validate();
  auto reject = [&](const std::function<void(praesidium_humanitatis::Config&)>& mutate,
                    const std::string& name) {
    auto config = valid;
    mutate(config);
    require_throws([&] { config.validate(); }, "must reject " + name);
  };
  reject([](auto& c) { c.hidden_size = 0; }, "zero hidden size");
  reject([](auto& c) { c.attention_heads = 0; }, "zero attention heads");
  reject([](auto& c) { c.attention_heads = 3; }, "indivisible attention size");
  reject([](auto& c) { c.dropout = -0.1; }, "negative dropout");
  reject([](auto& c) { c.dropout = 1.0; }, "dropout of one");
  reject([](auto& c) { c.dropout = std::numeric_limits<double>::quiet_NaN(); },
         "NaN dropout");
  reject([](auto& c) { c.past_continuous = -1; }, "negative feature count");
  reject([](auto& c) { c.future_categorical_cardinalities = {0}; },
         "zero categorical cardinality");
  reject([](auto& c) {
    c.static_continuous = 0;
    c.static_categorical_cardinalities.clear();
  }, "empty static feature group");
  reject([](auto& c) {
    c.future_continuous = 0;
    c.future_categorical_cardinalities.clear();
  }, "empty future feature group");
  reject([](auto& c) { c.quantiles.clear(); }, "empty quantiles");
  reject([](auto& c) { c.quantiles = {0.0, 0.5}; }, "zero quantile");
  reject([](auto& c) { c.quantiles = {0.5, 1.0}; }, "unit quantile");
  reject([](auto& c) { c.quantiles = {0.9, 0.1}; }, "unsorted quantiles");
  reject([](auto& c) { c.quantiles = {0.5, 0.5}; }, "duplicate quantiles");
  reject([](auto& c) {
    c.quantiles = {std::numeric_limits<double>::quiet_NaN()};
  }, "nonfinite quantiles");
}

void test_invalid_inputs(const torch::Device& device) {
  const auto config = make_config();
  const auto valid = make_batch(config, device);
  auto model = make_model(config, device);
  model->eval();
  auto reject = [&](const std::function<void(praesidium_humanitatis::Batch&)>& mutate,
                    const std::string& name) {
    auto batch = valid;
    mutate(batch);
    require_throws([&] { model->forward(batch); }, "must reject " + name);
  };
  reject([](auto& b) { b.statics.continuous = torch::Tensor(); },
         "undefined input");
  reject([](auto& b) { b.past.continuous = b.past.continuous.select(1, 0); },
         "wrong temporal rank");
  reject([](auto& b) { b.statics.continuous = b.statics.continuous.slice(0, 0, 2); },
         "inconsistent batch sizes");
  reject([](auto& b) { b.future.continuous = b.future.continuous.slice(1, 0, 0); },
         "empty forecast horizon");
  reject([](auto& b) { b.past.continuous = b.past.continuous.slice(1, 0, 0); },
         "empty history");
  reject([](auto& b) { b.past.categorical = b.past.categorical.slice(1, 0, 2); },
         "misaligned temporal feature lengths");
  reject([](auto& b) {
    b.past.continuous = torch::cat({b.past.continuous, b.past.continuous}, -1);
  }, "wrong feature count");
  reject([](auto& b) { b.future.continuous = b.future.continuous.to(torch::kFloat64); },
         "mixed continuous dtypes");
  reject([](auto& b) { b.future.continuous = b.future.continuous.to(torch::kInt64); },
         "integer continuous inputs");
  reject([](auto& b) { b.past.categorical = b.past.categorical.to(torch::kFloat32); },
         "floating categorical inputs");
  reject([](auto& b) { b.statics.categorical = torch::full_like(b.statics.categorical, -1); },
         "negative categorical IDs");
  reject([](auto& b) { b.future.categorical = torch::full_like(b.future.categorical, 5); },
         "categorical IDs outside vocabulary");
  reject([](auto& b) {
    b.past.continuous = torch::full_like(
        b.past.continuous, std::numeric_limits<float>::quiet_NaN());
  }, "NaN continuous inputs");
  reject([](auto& b) {
    b.future.continuous = torch::full_like(
        b.future.continuous, std::numeric_limits<float>::infinity());
  }, "infinite continuous inputs");
  if (device.is_cuda()) {
    reject([](auto& b) { b.future.categorical = b.future.categorical.cpu(); },
           "mixed input devices");
  }
}

void test_quantile_loss(const torch::Device& device) {
  const auto options = torch::TensorOptions().dtype(torch::kFloat32).device(device);
  auto predictions = torch::tensor({0.0f, 2.0f, 4.0f}, options)
                         .reshape({1, 1, 3}).set_requires_grad(true);
  const auto targets = torch::full({1, 1}, 3.0, options);
  const std::vector<double> quantiles{0.1, 0.5, 0.9};
  const auto loss = praesidium_humanitatis::quantile_loss(predictions, targets, quantiles);
  require(std::abs(loss.item<double>() - 0.9) < 1e-6,
          "pinball loss must sum quantiles and mean batch/horizon");
  loss.backward();
  require_close(predictions.grad(),
                torch::tensor({-0.1f, -0.5f, 0.1f}, options).reshape({1, 1, 3}),
                "pinball loss derivative");
  require_close(praesidium_humanitatis::quantile_loss(predictions.detach().repeat({2, 4, 1}),
                                   targets.repeat({2, 4}), quantiles),
                loss.detach(), "pinball loss must be independent of sample count");
  require_throws([&] { praesidium_humanitatis::quantile_loss(predictions, targets, {0.5}); },
                 "must reject mismatched quantile count");
  require_throws([&] {
    praesidium_humanitatis::quantile_loss(predictions, targets.unsqueeze(-1), quantiles);
  }, "must reject malformed targets");
  require_throws([&] {
    praesidium_humanitatis::quantile_loss(predictions,
                        torch::full_like(targets, std::numeric_limits<float>::quiet_NaN()),
                        quantiles);
  }, "must reject nonfinite targets");
}

void test_backward(const torch::Device& device) {
  const auto config = make_config();
  auto model = make_model(config, device);
  const auto batch = make_batch(config, device);
  const auto targets = torch::randn({6, 3}, batch.past.continuous.options());
  const auto loss = praesidium_humanitatis::quantile_loss(model->forward(batch).predictions,
                                       targets, config.quantiles);
  require_finite(loss, "training loss must be finite");
  loss.backward();
  double gradient_norm = 0.0;
  size_t nonzero_parameters = 0;
  const auto parameters = model->named_parameters();
  require(!parameters.is_empty(), "model must register trainable parameters");
  for (const auto& parameter : parameters) {
    const auto gradient = parameter.value().grad();
    require_finite(gradient, "gradient for " + parameter.key());
    const double magnitude = gradient.abs().sum().item<double>();
    gradient_norm += magnitude;
    nonzero_parameters += magnitude > 0.0;
  }
  require(std::isfinite(gradient_norm) && gradient_norm > 0.0,
          "backward must produce finite nonzero gradients");
  require(nonzero_parameters * 2 >= parameters.size(),
          "backward must reach the majority of model parameters");
}

void test_checkpoint(const torch::Device& device) {
  const auto config = make_config();
  auto original = make_model(config, device);
  original->eval();
  const auto batch = make_batch(config, device);
  torch::NoGradGuard no_grad;
  const auto expected = original->forward(batch);
  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  torch::serialize::OutputArchive output_archive;
  original->save(output_archive);
  output_archive.save_to(stream);
  stream.seekg(0);
  auto restored = make_model(config, device);
  torch::serialize::InputArchive input_archive;
  input_archive.load_from(stream, device);
  restored->load(input_archive);
  restored->eval();
  const auto actual = restored->forward(batch);
  const double tolerance = device.is_cuda() ? 1e-5 : 0.0;
  require_close(actual.predictions, expected.predictions,
                "checkpoint must preserve predictions", tolerance, tolerance);
  require_close(actual.attention_weights, expected.attention_weights,
                "checkpoint must preserve attention", tolerance, tolerance);
  require_close(actual.static_weights, expected.static_weights,
                "checkpoint must preserve variable selection", tolerance, tolerance);
}

void test_double_precision(const torch::Device& device) {
  const auto config = make_config();
  auto model = make_model(config, device);
  model->to(torch::kFloat64);
  model->eval();
  auto batch = make_batch(config, device, 2, 2, 2);
  batch.statics.continuous = batch.statics.continuous.to(torch::kFloat64);
  batch.past.continuous = batch.past.continuous.to(torch::kFloat64);
  batch.future.continuous = batch.future.continuous.to(torch::kFloat64);
  torch::NoGradGuard no_grad;
  const auto predictions = model->forward(batch).predictions;
  require(predictions.scalar_type() == torch::kFloat64,
          "converted model must support double precision");
  require_finite(predictions, "double precision output");
}

void test_fixed_batch_overfit(const torch::Device& device) {
  const auto config = make_config();
  auto model = make_model(config, device);
  const auto batch = make_batch(config, device, 6, 4, 2);
  const auto targets =
      0.3 * batch.past.continuous.index({Slice(), 3, 0}).unsqueeze(-1) +
      0.4 * batch.future.continuous.select(-1, 0) +
      0.1 * batch.statics.continuous.select(-1, 0).unsqueeze(-1);
  torch::optim::Adam optimizer(model->parameters(), torch::optim::AdamOptions(0.015));
  const double initial = praesidium_humanitatis::quantile_loss(model->forward(batch).predictions,
                                            targets, config.quantiles).item<double>();
  double final = initial;
  for (int step = 0; step < 80; ++step) {
    optimizer.zero_grad();
    const auto loss = praesidium_humanitatis::quantile_loss(model->forward(batch).predictions,
                                         targets, config.quantiles);
    require_finite(loss, "overfit loss");
    loss.backward();
    optimizer.step();
  }
  {
    torch::NoGradGuard no_grad;
    final = praesidium_humanitatis::quantile_loss(model->forward(batch).predictions,
                               targets, config.quantiles).item<double>();
  }
  std::cout << "  fixed-batch loss: " << initial << " -> " << final << '\n';
  require(final < initial * 0.5,
          "optimizer must reduce fixed-batch loss by at least half");
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
      std::cerr << "Usage: tft_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: tft_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"shapes and interpretation weights", [&] { test_shapes_and_interpretability(device); }},
      {"causal future isolation", [&] { test_causality(device); }},
      {"interpretable attention semantics", [&] { test_interpretable_attention(device); }},
      {"single-variable and zero-width feature types", [&] { test_feature_groups(device); }},
      {"configuration validation", test_invalid_config},
      {"input validation", [&] { test_invalid_inputs(device); }},
      {"pinball loss and derivative", [&] { test_quantile_loss(device); }},
      {"full-model backward", [&] { test_backward(device); }},
      {"checkpoint round trip", [&] { test_checkpoint(device); }},
      {"double precision", [&] { test_double_precision(device); }},
      {"fixed-batch training", [&] { test_fixed_batch_overfit(device); }},
  };
  int failures = 0;
  for (const auto& test : tests) {
    torch::manual_seed(2026);
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
