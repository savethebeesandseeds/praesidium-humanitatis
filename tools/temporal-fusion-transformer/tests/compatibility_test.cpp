#include "praesidium-humanitatis/checkpoint.h"

#include <torch/cuda.h>
#include <torch/serialize.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace praesidium_humanitatis;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename F>
void require_throws(F&& action, const std::string& message) {
  try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}

void close(const torch::Tensor& actual, const torch::Tensor& expected,
           const std::string& message, double tolerance = 1e-9) {
  require(actual.sizes() == expected.sizes() &&
              torch::allclose(actual, expected, tolerance, tolerance), message);
}

struct Files {
  std::filesystem::path directory;
  Files() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 100; ++i) {
      auto candidate = std::filesystem::temp_directory_path() /
          ("praesidium-compatibility-" + std::to_string(stamp) + "-" + std::to_string(i));
      if (std::filesystem::create_directory(candidate)) {
        directory = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("Cannot reserve compatibility test directory");
  }
  ~Files() {
    // Only this fixture's exclusively created directory is removed.
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::string file(const std::string& name) const { return (directory / name).string(); }
};

Config configuration(bool tied = true) {
  Config c;
  c.hidden_size = 8;
  c.attention_heads = 2;
  c.dropout = 0.0;
  c.static_continuous = 1;
  c.past_continuous = 3;
  c.future_continuous = 2;
  c.static_categorical_cardinalities = {3};
  c.past_categorical_cardinalities = {4, 5};
  c.future_categorical_cardinalities = {4, 5};
  if (tied) {
    c.layer_norm_epsilon = 1e-3;
    c.future_continuous_past_indices = {1, -1};
    c.future_categorical_past_indices = {0, -1};
  }
  return c;
}

Batch batch_for(const Config& c, torch::Device device) {
  auto floats = torch::TensorOptions().dtype(torch::kFloat64).device(device);
  auto ints = floats.dtype(torch::kInt64);
  const auto categories = [&](const std::vector<int64_t>& counts,
                               std::vector<int64_t> shape) {
    std::vector<torch::Tensor> columns;
    for (auto count : counts) columns.push_back(torch::randint(count, shape, ints));
    return torch::stack(columns, -1);
  };
  return {{torch::randn({3, c.static_continuous}, floats),
           categories(c.static_categorical_cardinalities, {3})},
          {torch::randn({3, 4, c.past_continuous}, floats),
           categories(c.past_categorical_cardinalities, {3, 4})},
          {torch::randn({3, 3, c.future_continuous}, floats),
           categories(c.future_categorical_cardinalities, {3, 3})}};
}

TFT model_for(const Config& c, torch::Device device) {
  auto model = TFT(c);
  model->to(device, torch::kFloat64);
  return model;
}

void copy_parameters(const TFT& destination, const TFT& source) {
  torch::NoGradGuard guard;
  const auto values = source->named_parameters();
  for (const auto& entry : destination->named_parameters()) {
    entry.value().copy_(values[entry.key()]);
  }
}

void compare_models(const TFT& a, const TFT& b, double tolerance = 1e-9) {
  const auto expected = b->named_parameters();
  require(a->named_parameters().size() == expected.size(), "Parameter count changed");
  for (const auto& entry : a->named_parameters())
    close(entry.value(), expected[entry.key()], entry.key(), tolerance);
}

void validate_unique_parameters(const TFT& model) {
  std::unordered_set<void*> seen;
  for (const auto& parameter : model->parameters())
    require(seen.insert(parameter.unsafeGetTensorImpl()).second,
            "Shared parameter is registered more than once");
}

void test_sharing(torch::Device device) {
  const auto config = configuration();
  auto tied = model_for(config, device);
  auto separate_config = config;
  separate_config.future_continuous_past_indices.clear();
  separate_config.future_categorical_past_indices.clear();
  auto separate = model_for(separate_config, device);
  const auto tied_parameters = tied->named_parameters();
  const auto separate_parameters = separate->named_parameters();
  require(tied_parameters.size() + 3 == separate_parameters.size(),
          "Tying one Linear and one Embedding must remove exactly three tensors");
  require(!tied_parameters.contains("network.future_embedding.continuous_0.weight") &&
              !tied_parameters.contains("network.future_embedding.categorical_0.weight"),
          "Future aliases must not own duplicate parameters");
  require(tied_parameters.contains("network.future_embedding.continuous_1.weight") &&
              tied_parameters.contains("network.future_embedding.categorical_1.weight"),
          "-1 entries must retain independent future parameters");
  validate_unique_parameters(tied);
  const std::vector<std::pair<std::string, std::string>> aliases{
      {"network.future_embedding.continuous_0.weight", "network.past_embedding.continuous_1.weight"},
      {"network.future_embedding.continuous_0.bias", "network.past_embedding.continuous_1.bias"},
      {"network.future_embedding.categorical_0.weight", "network.past_embedding.categorical_0.weight"}};
  {
    torch::NoGradGuard guard;
    for (const auto& entry : tied_parameters) separate_parameters[entry.key()].copy_(entry.value());
    for (const auto& alias : aliases)
      separate_parameters[alias.first].copy_(tied_parameters[alias.second]);
  }
  auto batch = batch_for(config, device);
  std::map<std::string, torch::Tensor> diagnostics{{"stale", torch::zeros({1})}};
  auto tied_prediction = tied->forward(batch, &diagnostics).predictions;
  require(diagnostics.count("stale") == 0 && diagnostics.size() == 20,
          "Optional diagnostics must replace stale tensors");
  close(diagnostics.at("raw_projection"), tied_prediction,
        "Raw projection diagnostic must precede the optional head");
  diagnostics.at("past_embedded").retain_grad();
  diagnostics.at("future_embedded").retain_grad();
  diagnostics.at("selection_context").retain_grad();
  auto separate_prediction = separate->forward(batch).predictions;
  close(tied_prediction, separate_prediction, "Mapped forward differs from equal untied weights");
  tied_prediction.square().mean().backward();
  separate_prediction.square().mean().backward();
  for (const std::string name : {"past_embedded", "future_embedded", "selection_context"}) {
    require(diagnostics.at(name).grad().defined() &&
                torch::isfinite(diagnostics.at(name).grad()).all().item<bool>(),
            "Diagnostics must retain live graph tensors: " + name);
  }
  for (const auto& entry : tied_parameters) {
    auto expected = separate_parameters[entry.key()].grad().clone();
    for (const auto& alias : aliases)
      if (alias.second == entry.key()) expected += separate_parameters[alias.first].grad();
    close(entry.value().grad(), expected, "Shared gradient must sum both paths: " + entry.key());
  }
  for (const auto& alias : aliases) {
    require(separate_parameters[alias.first].grad().abs().sum().item<double>() > 1e-12,
            "Future path must contribute to shared gradient");
    require(separate_parameters[alias.second].grad().abs().sum().item<double>() > 1e-12,
            "Historical path must contribute to shared gradient");
  }

  const auto shared = tied_parameters[aliases.front().second];
  const auto before = shared.detach().clone();
  const auto gradient = shared.grad().clone();
  torch::optim::Adam optimizer(tied->parameters(), torch::optim::AdamOptions(0.004).eps(1e-6));
  optimizer.step();
  close(shared, before - 0.004 * gradient / (gradient.abs() + 1e-6),
        "Shared tensor must receive exactly one first Adam update");
  const auto& state = static_cast<const torch::optim::AdamParamState&>(
      *optimizer.state().at(shared.unsafeGetTensorImpl()));
  require(state.step() == 1, "Shared optimizer state must advance once");

  // Multiple future columns may refer to the same historical variable.
  auto repeated_config = config;
  repeated_config.future_continuous_past_indices = {1, 1};
  repeated_config.future_categorical_past_indices = {0, 0};
  repeated_config.future_categorical_cardinalities = {4, 4};
  auto repeated = model_for(repeated_config, device);
  validate_unique_parameters(repeated);
  require(repeated->named_parameters().size() + 6 == separate_parameters.size(),
          "Repeated aliases must still register each past module only once");
  repeated->forward(batch_for(repeated_config, device)).predictions.sum().backward();
  require(torch::isfinite(repeated->named_parameters()[aliases.front().second].grad()).all().item<bool>(),
          "Repeated sharing must have finite accumulated gradients");
}

void test_epsilon(torch::Device device) {
  auto config = configuration(false);
  auto legacy = model_for(config, device);
  config.layer_norm_epsilon = 1e-3;
  auto reference = model_for(config, device);
  copy_parameters(reference, legacy);
  int norms = 0;
  for (const auto& module : reference->modules()) {
    if (auto norm = std::dynamic_pointer_cast<torch::nn::LayerNormImpl>(module)) {
      ++norms;
      require(norm->options.eps() == config.layer_norm_epsilon,
              "Every LayerNorm must receive configured epsilon");
      auto x = torch::linspace(-0.01, 0.01, norm->weight.numel(), norm->weight.options()).unsqueeze(0);
      auto centered = x - x.mean(-1, true);
      auto expected = centered / (centered.square().mean(-1, true) + config.layer_norm_epsilon).sqrt();
      expected = expected * norm->weight + norm->bias;
      close(norm->forward(x), expected, "LayerNorm must use epsilon inside its square root");
    }
  }
  require(norms > 10, "Fixture must exercise all categories of LayerNorm");
  auto batch = batch_for(config, device);
  require(!torch::allclose(reference->forward(batch).predictions,
                          legacy->forward(batch).predictions, 1e-10, 1e-10),
          "Non-default epsilon must affect complete model behavior");

  // Explicitly independent mappings must preserve initialization and topology.
  auto empty = configuration(false);
  auto explicit_independent = empty;
  explicit_independent.future_continuous_past_indices = {-1, -1};
  explicit_independent.future_categorical_past_indices = {-1, -1};
  torch::manual_seed(47);
  auto a = model_for(empty, device);
  torch::manual_seed(47);
  auto b = model_for(explicit_independent, device);
  compare_models(a, b, 0.0);
  close(a->forward(batch).predictions, b->forward(batch).predictions,
        "Explicit -1 mappings must preserve independent behavior", 0.0);
}

void test_validation() {
  const std::vector<std::function<void(Config&)>> invalid{
      [](auto& c) { c.layer_norm_epsilon = 0.0; },
      [](auto& c) { c.layer_norm_epsilon = -1e-5; },
      [](auto& c) { c.layer_norm_epsilon = std::numeric_limits<double>::infinity(); },
      [](auto& c) { c.layer_norm_epsilon = std::numeric_limits<double>::quiet_NaN(); },
      [](auto& c) { c.future_continuous_past_indices = {0}; },
      [](auto& c) { c.future_continuous_past_indices = {-2, -1}; },
      [](auto& c) { c.future_continuous_past_indices = {3, -1}; },
      [](auto& c) { c.future_categorical_past_indices = {0}; },
      [](auto& c) { c.future_categorical_past_indices = {-2, -1}; },
      [](auto& c) { c.future_categorical_past_indices = {2, -1}; },
      [](auto& c) { c.future_categorical_past_indices = {1, -1}; },
      [](auto& c) { c.past_continuous = 0; },
      [](auto& c) { c.past_categorical_cardinalities.clear(); }};
  for (const auto& change : invalid) {
    auto config = configuration();
    change(config);
    require_throws([&] { config.validate(); }, "Invalid reference configuration accepted");
  }
}

TrainingSession session_for(const Config& config, torch::Device device) {
  TrainingSession session;
  session.model = model_for(config, device);
  session.optimizer = std::make_unique<torch::optim::Adam>(
      session.model->parameters(), torch::optim::AdamOptions(0.002));
  session.progress.seed = 9107;
  session.progress.data_id = "compatibility-fixed-batch";
  return session;
}

void update(TrainingSession& session, const Batch& batch) {
  torch::manual_seed(session.progress.seed + session.progress.completed_steps);
  session.model->train();
  session.optimizer->zero_grad();
  const auto target = batch.future.continuous.select(-1, 0);
  quantile_loss(session.model->forward(batch).predictions, target,
                session.model->config.quantiles).backward();
  session.optimizer->step();
  ++session.progress.completed_steps;
}

void compare_sessions(const TrainingSession& a, const TrainingSession& b) {
  compare_models(a.model, b.model);
  require(a.progress.completed_steps == b.progress.completed_steps, "Progress must round trip");
  const auto ap = a.model->parameters(), bp = b.model->parameters();
  require(a.optimizer->state().size() == b.optimizer->state().size(), "Adam topology changed");
  for (size_t i = 0; i < ap.size(); ++i) {
    const auto& as = static_cast<const torch::optim::AdamParamState&>(
        *a.optimizer->state().at(ap[i].unsafeGetTensorImpl()));
    const auto& bs = static_cast<const torch::optim::AdamParamState&>(
        *b.optimizer->state().at(bp[i].unsafeGetTensorImpl()));
    require(as.step() == bs.step(), "Adam update count changed");
    close(as.exp_avg(), bs.exp_avg(), "Adam first moment changed");
    close(as.exp_avg_sq(), bs.exp_avg_sq(), "Adam second moment changed");
  }
}

// Historical config schemas are written independently. This cannot accidentally
// pass merely because the format-3 writer and reader share the same omission.
void write_legacy(const std::string& path, const TrainingSession& session, int64_t version) {
  using torch::serialize::OutputArchive;
  const auto& c = session.model->config;
  OutputArchive archive;
  archive.write("format_version", c10::IValue(version));
  archive.write("model_dtype", c10::IValue("float64"));
  archive.write("model_training", c10::IValue(session.model->is_training()));
  const auto parameters = session.model->parameters();
  archive.write("model_parameter_count", c10::IValue(static_cast<int64_t>(parameters.size())));
  archive.write("completed_steps", c10::IValue(session.progress.completed_steps));
  archive.write("seed", c10::IValue(session.progress.seed));
  archive.write("gradient_clip_norm", c10::IValue(session.progress.gradient_clip_norm));
  archive.write("data_id", c10::IValue(session.progress.data_id));
  OutputArchive config(archive.compilation_unit());
  config.write("hidden_size", c10::IValue(c.hidden_size));
  config.write("attention_heads", c10::IValue(c.attention_heads));
  config.write("dropout", c10::IValue(c.dropout));
  config.write("static_continuous", c10::IValue(c.static_continuous));
  config.write("past_continuous", c10::IValue(c.past_continuous));
  config.write("future_continuous", c10::IValue(c.future_continuous));
  config.write("static_categorical_cardinalities", torch::tensor(c.static_categorical_cardinalities, torch::kInt64), true);
  config.write("past_categorical_cardinalities", torch::tensor(c.past_categorical_cardinalities, torch::kInt64), true);
  config.write("future_categorical_cardinalities", torch::tensor(c.future_categorical_cardinalities, torch::kInt64), true);
  config.write("quantiles", torch::tensor(c.quantiles, torch::kFloat64), true);
  if (version == 2) config.write("ordered_quantiles", c10::IValue(c.ordered_quantiles));
  archive.write("config", config);
  OutputArchive adam(archive.compilation_unit()), defaults(archive.compilation_unit());
  adam.write("type", c10::IValue("Adam"));
  session.optimizer->defaults().serialize(defaults);
  adam.write("defaults", defaults);
  adam.write("group_count", c10::IValue(int64_t{1}));
  OutputArchive group(archive.compilation_unit()), options(archive.compilation_unit());
  session.optimizer->param_groups().front().options().serialize(options);
  group.write("options", options);
  group.write("parameter_indices", torch::arange(static_cast<int64_t>(parameters.size()), torch::kInt64), true);
  for (size_t i = 0; i < parameters.size(); ++i) {
    OutputArchive state(archive.compilation_unit());
    session.optimizer->state().at(parameters[i].unsafeGetTensorImpl())->serialize(state);
    group.write("state_" + std::to_string(i), state);
  }
  adam.write("group_0", group);
  archive.write("optimizer", adam);
  OutputArchive model(archive.compilation_unit());
  session.model->save(model);
  archive.write("model", model);
  archive.save_to(path);
}

void test_checkpoint(torch::Device device, bool fully_shared = false) {
  Files files;
  auto config = configuration();
  config.dropout = 0.2;
  config.ordered_quantiles = true;
  if (fully_shared) {
    config.future_continuous_past_indices = {1, 2};
    config.future_categorical_past_indices = {0, 1};
  }
  auto original = session_for(config, device);
  auto batch = batch_for(config, device);
  update(original, batch);
  save_checkpoint(files.file("shared.pt"), original.model, *original.optimizer, original.progress);
  torch::serialize::InputArchive metadata;
  metadata.load_from(files.file("shared.pt"), torch::kCPU);
  c10::IValue version;
  metadata.read("format_version", version);
  require(version.isInt() && version.toInt() == 3, "Reference options require format 3");
  auto restored = load_checkpoint(files.file("shared.pt"), device);
  const auto& actual = restored.model->config;
  require(actual.layer_norm_epsilon == config.layer_norm_epsilon &&
              actual.future_continuous_past_indices == config.future_continuous_past_indices &&
              actual.future_categorical_past_indices == config.future_categorical_past_indices &&
              actual.ordered_quantiles, "Reference config must survive checkpoint loading");
  validate_unique_parameters(restored.model);
  compare_sessions(restored, original);
  update(original, batch);
  update(restored, batch);
  compare_sessions(restored, original);
  original.model->eval();
  restored.model->eval();
  close(restored.model->forward(batch).predictions, original.model->forward(batch).predictions,
        "Shared modules must stay shared through checkpoint continuation");
}

void test_legacy(torch::Device device) {
  Files files;
  for (const auto version : {int64_t{1}, int64_t{2}}) {
    auto config = configuration(false);
    config.dropout = 0.2;
    config.ordered_quantiles = version == 2;
    auto original = session_for(config, device);
    auto batch = batch_for(config, device);
    update(original, batch);
    write_legacy(files.file("legacy.pt"), original, version);
    auto restored = load_checkpoint(files.file("legacy.pt"), device);
    require(restored.model->config.layer_norm_epsilon == 1e-5 &&
                restored.model->config.future_continuous_past_indices.empty() &&
                restored.model->config.future_categorical_past_indices.empty(),
            "Legacy config must retain epsilon and independent embeddings");
    require(restored.model->config.ordered_quantiles == config.ordered_quantiles,
            "Legacy output mode must be retained");
    compare_sessions(restored, original);
    save_checkpoint(files.file("migrated.pt"), restored.model, *restored.optimizer, restored.progress);
    auto migrated = load_checkpoint(files.file("migrated.pt"), device);
    update(original, batch);
    update(migrated, batch);
    compare_sessions(migrated, original);
  }
}
}  // namespace

int main(int argc, char** argv) {
  torch::Device device(torch::kCPU);
  if (argc == 3 && std::string(argv[1]) == "--device") {
    if (std::string(argv[2]) == "cuda") {
      if (!torch::cuda::is_available()) return 77;
      device = torch::Device(torch::kCUDA, 0);
    } else if (std::string(argv[2]) != "cpu") return 2;
  } else if (argc != 1) return 2;
  torch::set_num_threads(1);
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"shared forward, gradients and single optimizer update", [&] { test_sharing(device); }},
      {"LayerNorm epsilon and independent defaults", [&] { test_epsilon(device); }},
      {"reference configuration validation", test_validation},
      {"shared format-3 checkpoint and dropout continuation", [&] {
         test_checkpoint(device);
         test_checkpoint(device, true);  // Future group has no owned parameters.
       }},
      {"historical format-1/2 loading and migration", [&] { test_legacy(device); }}};
  int failures = 0;
  for (const auto& test : tests) {
    torch::manual_seed(7319);
    try {
      test.second();
      std::cout << "PASS: " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " compatibility groups passed on " << device << '\n';
  return failures == 0 ? 0 : 1;
}
