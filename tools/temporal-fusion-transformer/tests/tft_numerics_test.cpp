#include <praesidium-humanitatis/tft.h>
#include <torch/cuda.h>
#include <torch/torch.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace praesidium_humanitatis;
using torch::indexing::Slice;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void close(const torch::Tensor& actual, const torch::Tensor& expected,
           const std::string& label, double atol = 2e-6, double rtol = 2e-5) {
  require(actual.sizes() == expected.sizes(), label + ": shape mismatch");
  require(torch::allclose(actual, expected, rtol, atol), label + ": max error " +
      std::to_string((actual - expected).abs().max().item<double>()));
}

Config config(double dropout = 0.0) {
  Config c;
  c.hidden_size = 8;
  c.attention_heads = 2;
  c.dropout = dropout;
  // Three variables avoid the degenerate one-variable softmax and the
  // near-discrete two-variable LayerNorm when checking gradient connectivity.
  c.static_continuous = c.past_continuous = c.future_continuous = 2;
  c.static_categorical_cardinalities = {3};
  c.past_categorical_cardinalities = {3};
  c.future_categorical_cardinalities = {3};
  return c;
}

Batch batch() {
  const auto ids = torch::TensorOptions().dtype(torch::kInt64);
  return {{torch::randn({3, 2}), torch::randint(3, {3, 1}, ids)},
          {torch::randn({3, 4, 2}), torch::randint(3, {3, 4, 1}, ids)},
          {torch::randn({3, 3, 2}), torch::randint(3, {3, 3, 1}, ids)}};
}

Batch convert(const Batch& b, torch::Device device, torch::Dtype dtype) {
  auto group = [&](const FeatureBatch& f) {
    return FeatureBatch{f.continuous.to(device, dtype).clone(),
                        f.categorical.to(device).clone()};
  };
  return {group(b.statics), group(b.past), group(b.future)};
}

Batch rows(const Batch& b, const torch::Tensor& indices) {
  auto group = [&](const FeatureBatch& f) {
    return FeatureBatch{f.continuous.index_select(0, indices),
                        f.categorical.index_select(0, indices)};
  };
  return {group(b.statics), group(b.past), group(b.future)};
}

void compare(const Output& a, const Output& b, const std::string& label,
             double atol = 2e-6, double rtol = 2e-5) {
  close(a.predictions, b.predictions, label + " predictions", atol, rtol);
  close(a.static_weights, b.static_weights, label + " static weights", atol, rtol);
  close(a.past_weights, b.past_weights, label + " past weights", atol, rtol);
  close(a.future_weights, b.future_weights, label + " future weights", atol, rtol);
  close(a.attention_weights, b.attention_weights, label + " attention", atol, rtol);
}

Output output_rows(const Output& o, const torch::Tensor& indices) {
  return {o.predictions.index_select(0, indices), o.static_weights.index_select(0, indices),
          o.past_weights.index_select(0, indices), o.future_weights.index_select(0, indices),
          o.attention_weights.index_select(0, indices)};
}

void test_batch_and_prefix(torch::Device device) {
  // Nonzero configured dropout verifies eval() reaches all nested modules.
  TFT model(config(0.35));
  model->to(device);
  model->eval();
  auto b = convert(batch(), device, torch::kFloat32);
  const auto ids = torch::TensorOptions().device(device).dtype(torch::kInt64);
  torch::NoGradGuard guard;
  const auto original = model->forward(b);
  compare(model->forward(b), original, "eval repeat", 0.0, 0.0);
  const auto order = torch::tensor({2, 0, 1}, ids);
  compare(model->forward(rows(b, order)), output_rows(original, order), "batch permutation");
  for (int64_t row = 0; row < 3; ++row) {
    const auto index = torch::tensor({row}, ids);
    compare(model->forward(rows(b, index)), output_rows(original, index), "singleton batch");
  }
  // Repeated calls must not carry recurrent state between independent windows.
  auto unrelated = convert(batch(), device, torch::kFloat32);
  model->forward(unrelated);
  compare(model->forward(b), original, "no state carried between calls", 0.0, 0.0);
  for (int64_t length = 1; length < 3; ++length) {
    auto prefix = b;
    prefix.future = {b.future.continuous.slice(1, 0, length),
                     b.future.categorical.slice(1, 0, length)};
    const auto shorter = model->forward(prefix);
    close(shorter.predictions, original.predictions.slice(1, 0, length), "prefix forecasts");
    close(shorter.attention_weights,
          original.attention_weights.slice(2, 0, length).slice(3, 0, 4 + length),
          "prefix attention");
  }
}

void test_causal_jacobian(torch::Device device) {
  TFT model(config());
  model->to(device);
  model->train(); // CUDA LSTM backward needs a training forward on cuDNN backends.
  auto b = convert(batch(), device, torch::kFloat32);
  b.future.continuous.set_requires_grad(true);
  for (int64_t horizon = 0; horizon < 3; ++horizon) {
    model->zero_grad();
    if (b.future.continuous.grad().defined()) b.future.continuous.grad().zero_();
    model->forward(b).predictions.index({0, horizon, 1}).backward();
    const auto g = b.future.continuous.grad();
    require(torch::isfinite(g).all().item<bool>(), "finite causal Jacobian");
    require(g.index({0, horizon}).abs().sum().item<double>() > 1e-8,
            "current known-future input must influence output");
    require(g.slice(0, 1).abs().max().item<double>() == 0.0,
            "other batch rows must have zero influence");
    if (horizon < 2) {
      require(g.slice(1, horizon + 1).abs().max().item<double>() == 0.0,
              "later future inputs must have exactly zero derivative");
    }
  }
}

void test_finite_differences(bool ordered_quantiles) {
  auto c = config();
  c.ordered_quantiles = ordered_quantiles;
  TFT model(c);
  model->to(torch::kFloat64);
  model->train();
  auto b = convert(batch(), torch::kCPU, torch::kFloat64);
  for (auto* input : {&b.statics.continuous, &b.past.continuous, &b.future.continuous})
    input->set_requires_grad(true);
  const auto objective_weights = torch::randn({3, 3, 3}, torch::kFloat64);
  const auto objective = [&]() {
    return (model->forward(b).predictions * objective_weights).mean();
  };
  objective().backward();
  std::map<std::string, double> block_norms;
  size_t checked = 0;
  double largest_error = 0.0;
  torch::NoGradGuard guard;
  const auto check = [&](torch::Tensor tensor, const std::string& name) {
    const auto gradient = tensor.grad();
    require(gradient.defined() && torch::isfinite(gradient).all().item<bool>(),
            "finite defined derivative: " + name);
    // A deterministic random direction covers all coordinates, including those
    // whose analytical gradient is zero. This does not reuse autograd to
    // compute the expected derivative.
    auto direction = torch::randn_like(tensor);
    direction /= direction.norm();
    const double analytic = (gradient * direction).sum().item<double>();
    const auto saved = tensor.clone();
    const double epsilon = 1e-5;
    tensor.copy_(saved + epsilon * direction);
    const double plus = objective().item<double>();
    tensor.copy_(saved - epsilon * direction);
    const double minus = objective().item<double>();
    tensor.copy_(saved);
    const double numeric = (plus - minus) / (2 * epsilon);
    const double error = std::abs(numeric - analytic);
    largest_error = std::max(largest_error, error);
    require(error <= 2e-7 + 2e-4 * std::abs(analytic),
            "finite-difference derivative: " + name + " analytic=" +
            std::to_string(analytic) + " numeric=" + std::to_string(numeric));
    ++checked;
  };
  for (const auto& parameter : model->named_parameters()) {
    const auto end = parameter.key().find('.', 8);
    const auto block = parameter.key().substr(0, end);
    require(parameter.value().grad().defined(), "registered parameter must receive a gradient");
    block_norms[block] += parameter.value().grad().abs().sum().item<double>();
    check(parameter.value(), parameter.key());
  }
  for (const auto& entry : block_norms) {
    require(entry.second > 1e-10, "disconnected or inactive block: " + entry.first);
  }
  check(b.statics.continuous, "static inputs");
  check(b.past.continuous, "past inputs");
  check(b.future.continuous, "future inputs");
  std::cout << "  " << (ordered_quantiles ? "ordered" : "raw")
            << " central differences: " << checked << " tensor directions; "
            << block_norms.size() << " active blocks; max absolute error="
            << largest_error << '\n';
}

void test_device_parity(bool ordered_quantiles) {
  auto c = config();
  c.ordered_quantiles = ordered_quantiles;
  TFT cpu(c), gpu(c);
  gpu->to(torch::Device(torch::kCUDA, 0));
  {
    torch::NoGradGuard guard;
    auto gpu_parameters = gpu->named_parameters();
    for (const auto& p : cpu->named_parameters()) gpu_parameters[p.key()].copy_(p.value());
  }
  cpu->train();
  gpu->train();
  auto host = batch();
  host.statics.continuous.set_requires_grad(true);
  host.past.continuous.set_requires_grad(true);
  host.future.continuous.set_requires_grad(true);
  // Detach to make device inputs independent leaves for derivative comparison.
  auto accelerator = convert(host, torch::Device(torch::kCUDA, 0), torch::kFloat32);
  for (auto* x : {&accelerator.statics.continuous, &accelerator.past.continuous,
                 &accelerator.future.continuous}) *x = x->detach().set_requires_grad(true);
  const auto expected = cpu->forward(host);
  const auto actual = gpu->forward(accelerator);
  const Output device_on_host{actual.predictions.cpu(), actual.static_weights.cpu(),
      actual.past_weights.cpu(), actual.future_weights.cpu(), actual.attention_weights.cpu()};
  compare(device_on_host, expected, "identical-weight CPU/CUDA forward", 2e-5, 2e-4);
  const auto weights = torch::randn_like(expected.predictions);
  (expected.predictions * weights).mean().backward();
  (actual.predictions * weights.to(actual.predictions.device())).mean().backward();
  const auto gpu_parameters = gpu->named_parameters();
  double max_error = 0.0;
  for (const auto& p : cpu->named_parameters()) {
    const auto device_gradient = gpu_parameters[p.key()].grad().cpu();
    close(device_gradient, p.value().grad(), "CPU/CUDA gradient " + p.key(), 3e-5, 2e-3);
    max_error = std::max(max_error, (device_gradient - p.value().grad()).abs().max().item<double>());
  }
  close(accelerator.statics.continuous.grad().cpu(), host.statics.continuous.grad(), "static input gradient parity", 3e-5, 2e-3);
  close(accelerator.past.continuous.grad().cpu(), host.past.continuous.grad(), "past input gradient parity", 3e-5, 2e-3);
  close(accelerator.future.continuous.grad().cpu(), host.future.continuous.grad(), "future input gradient parity", 3e-5, 2e-3);
  std::cout << "  " << (ordered_quantiles ? "ordered" : "raw")
            << " CPU/CUDA max prediction error="
            << (actual.predictions.cpu() - expected.predictions).abs().max().item<double>()
            << "; max parameter gradient error=" << max_error << '\n';
}

void test_ordered_head(torch::Device device) {
  const auto dtype = device.is_cuda() ? torch::kFloat32 : torch::kFloat64;
  const auto input = convert(batch(), device, dtype);
  const auto log2 = std::log(2.0);
  struct HeadCase {
    std::string name;
    std::vector<double> quantiles, biases, expected;
  };
  const std::vector<HeadCase> cases{
      // softplus(0)=log(2), softplus(log(3))=log(4),
      // softplus(log(7))=log(8). Expected values are hand-calculated.
      {"multiple outward gaps", {0.05, 0.25, 0.5, 0.75, 0.95},
       {0.0, std::log(3.0), 2.0, std::log(7.0), 0.0},
       {2.0 - 3.0 * log2, 2.0 - 2.0 * log2, 2.0,
        2.0 + 3.0 * log2, 2.0 + 4.0 * log2}},
      {"nearest center at last quantile", {0.1, 0.2, 0.3},
       {0.0, std::log(3.0), 2.0}, {2.0 - 3.0 * log2, 2.0 - 2.0 * log2, 2.0}},
      {"nearest center at first quantile", {0.6, 0.7, 0.8},
       {2.0, 0.0, std::log(3.0)}, {2.0, 2.0 + log2, 2.0 + 3.0 * log2}},
      {"equal-distance center selects lower index", {0.125, 0.375, 0.625, 0.875},
       {0.0, 2.0, 0.0, std::log(3.0)},
       {2.0 - log2, 2.0, 2.0 + log2, 2.0 + 3.0 * log2}},
      {"large finite gaps and equal adjacent outputs", {0.05, 0.25, 0.5, 0.75, 0.95},
       {1000.0, -1000.0, 2.0, 1000.0, -1000.0}, {-998.0, 2.0, 2.0, 1002.0, 1002.0}},
      {"singleton negative identity", {0.8}, {-1000.0}, {-1000.0}},
      {"singleton positive identity", {0.2}, {1000.0}, {1000.0}}};
  for (const auto& test : cases) {
    auto c = config();
    c.quantiles = test.quantiles;
    c.ordered_quantiles = true;
    TFT model(c);
    model->to(device, dtype);
    // Dropout is zero; train mode permits cuDNN backward on CUDA.
    model->train();
    auto parameters = model->named_parameters();
    {
      torch::NoGradGuard guard;
      parameters["network.projection.weight"].zero_();
      parameters["network.projection.bias"].copy_(torch::tensor(test.biases).to(device, dtype));
    }
    const auto predictions = model->forward(input).predictions;
    const auto expected = torch::tensor(test.expected).to(device, dtype)
                              .reshape({1, 1, -1}).expand_as(predictions);
    require(torch::isfinite(predictions).all().item<bool>(), test.name + ": finite predictions");
    close(predictions, expected, test.name, 2e-6, 2e-6);
    if (test.quantiles.size() > 1) {
      const auto count = static_cast<int64_t>(test.quantiles.size());
      require((predictions.slice(-1, 1, count) >= predictions.slice(-1, 0, count - 1))
                  .all().item<bool>(), test.name + ": nondecreasing quantiles");
    }
    predictions.sum().backward();
    require(torch::isfinite(parameters["network.projection.bias"].grad()).all().item<bool>(),
            test.name + ": finite head derivatives, including extreme gap logits");
  }
  std::cout << "  " << cases.size() << " hand-calculated ordered head cases passed\n";
}
} // namespace

int main(int argc, char** argv) {
  torch::Device device(torch::kCPU);
  if (argc == 3 && std::string(argv[1]) == "--device" && std::string(argv[2]) == "cuda") {
    if (!torch::cuda::is_available()) { std::cout << "SKIP: CUDA unavailable\n"; return 77; }
    device = torch::Device(torch::kCUDA, 0);
  } else if (!(argc == 1 || (argc == 3 && std::string(argv[1]) == "--device" &&
                            std::string(argv[2]) == "cpu"))) {
    std::cerr << "Usage: tft_numerics_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  std::vector<std::pair<std::string, std::function<void()>>> tests{
    {"batch isolation, eval determinism, prefix consistency", [&] { test_batch_and_prefix(device); }},
    {"causal and batch Jacobian", [&] { test_causal_jacobian(device); }},
    {"ordered head values, stability and singleton identity", [&] { test_ordered_head(device); }}
  };
  if (device.is_cpu()) {
    tests.push_back({"numerical derivatives and block connectivity", [] { test_finite_differences(false); }});
    tests.push_back({"ordered numerical derivatives and block connectivity", [] { test_finite_differences(true); }});
  } else {
    tests.push_back({"matched CPU/CUDA forward and backward", [] { test_device_parity(false); }});
    tests.push_back({"ordered matched CPU/CUDA forward and backward", [] { test_device_parity(true); }});
  }
  int failures = 0;
  for (const auto& test : tests) {
    torch::manual_seed(4171);
    try { test.second(); std::cout << "PASS: " << test.first << '\n'; }
    catch (const std::exception& e) { ++failures; std::cerr << "FAIL: " << test.first << ": " << e.what() << '\n'; }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " numerical tests passed on " << device << '\n';
  return failures ? 1 : 0;
}
