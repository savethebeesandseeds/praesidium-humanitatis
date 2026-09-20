#include "praesidium-humanitatis/tft.h"
#include "../replication/reference_optimizer.h"
#include "../replication/reference_loss.h"

#include <torch/torch.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace praesidium_humanitatis;
using torch::Tensor;
namespace fs = std::filesystem;

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;
  do {
    const auto end = value.find(delimiter, start);
    parts.push_back(value.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  } while (true);
  return parts;
}

std::vector<int64_t> integers(const std::string& value) {
  std::vector<int64_t> result;
  if (!value.empty()) for (const auto& part : split(value, ',')) result.push_back(std::stoll(part));
  return result;
}

class Fixture {
 public:
  explicit Fixture(const fs::path& root) {
    const uint16_t endian_test = 1;
    TORCH_CHECK(*reinterpret_cast<const uint8_t*>(&endian_test) == 1, "Fixture requires little endian host");
    std::ifstream manifest(root / "manifest.tsv");
    std::string line;
    TORCH_CHECK(std::getline(manifest, line) && line == "kind\tname\tdtype\tshape\tfile", "Invalid fixture manifest");
    while (std::getline(manifest, line)) {
      if (line.empty()) continue;
      const auto fields = split(line, '\t');
      TORCH_CHECK(fields.size() == 5, "Invalid fixture record: ", line);
      const auto shape = integers(fields[3]);
      TORCH_CHECK(fields[2] == "f32" || fields[2] == "i64", "Unsupported fixture dtype");
      auto tensor = torch::empty(shape, fields[2] == "f32" ? torch::kFloat32 : torch::kInt64);
      std::ifstream file(root / fields[4], std::ios::binary);
      const auto bytes = static_cast<std::streamsize>(tensor.numel() * tensor.element_size());
      file.read(static_cast<char*>(tensor.data_ptr()), bytes);
      TORCH_CHECK(file && file.peek() == std::char_traits<char>::eof(), "Invalid tensor binary: ", fields[4]);
      TORCH_CHECK(values.emplace(fields[0] + "/" + fields[1], tensor).second, "Duplicate fixture record");
    }
    std::ifstream configuration(root / "config.tsv");
    TORCH_CHECK(configuration.good(), "Missing fixture config.tsv");
    while (std::getline(configuration, line)) {
      const auto fields = split(line, '\t');
      TORCH_CHECK(fields.size() == 2, "Invalid fixture config record");
      config.emplace(fields[0], fields[1]);
    }
  }

  const Tensor& get(const std::string& kind, const std::string& name) const { return values.at(kind + "/" + name); }
  std::map<std::string, Tensor> values;
  std::map<std::string, std::string> config;
};

class Comparisons {
 public:
  void check(const std::string& name, const Tensor& actual_input, const Tensor& expected,
             double atol = 2e-5, double rtol = 2e-4) {
    ++checks;
    auto actual = actual_input.detach().cpu();
    TORCH_CHECK(actual.sizes() == expected.sizes(), "Shape mismatch ", name, ": ", actual.sizes(), " vs ", expected.sizes());
    if (actual.numel() == 0) return;
    const auto difference = (actual - expected).abs();
    const auto allowed = atol + rtol * expected.abs();
    const auto ratio = (difference / allowed).max().item<double>();
    const auto maximum = difference.max().item<double>();
    const auto valid = torch::isfinite(actual).all().item<bool>() && ratio <= 1.;
    max_difference = std::max(max_difference, maximum);
    max_ratio = std::max(max_ratio, ratio);
    if (!valid) {
      ++failures;
      std::cerr << "FAIL " << name << " max_abs=" << maximum << " tolerance_ratio=" << ratio << '\n';
    }
  }
  int64_t checks = 0, failures = 0;
  double max_difference = 0., max_ratio = 0.;
};

int run(const fs::path& directory, const torch::Device& device) {
  torch::set_num_threads(1);
  at::globalContext().setAllowTF32CuBLAS(false);
  at::globalContext().setAllowTF32CuDNN(false);
  Fixture fixture(directory);
  Config config;
  config.hidden_size = std::stoll(fixture.config.at("hidden_size"));
  config.attention_heads = std::stoll(fixture.config.at("attention_heads"));
  config.dropout = 0.; config.layer_norm_epsilon = .001;
  config.static_continuous = std::stoll(fixture.config.at("static_continuous"));
  config.past_continuous = std::stoll(fixture.config.at("past_continuous"));
  config.future_continuous = std::stoll(fixture.config.at("future_continuous"));
  config.static_categorical_cardinalities = integers(fixture.config.at("static_categorical_cardinalities"));
  config.past_categorical_cardinalities = integers(fixture.config.at("past_categorical_cardinalities"));
  config.future_categorical_cardinalities = integers(fixture.config.at("future_categorical_cardinalities"));
  config.future_continuous_past_indices = integers(fixture.config.at("future_continuous_past_indices"));
  config.future_categorical_past_indices = integers(fixture.config.at("future_categorical_past_indices"));
  TFT model(config);
  model->to(device);
  // CUDA cuDNN backward requires a training-mode recurrent forward. Dropout is
  // explicitly zero in this fixture, so mode does not introduce randomness.
  model->train();
  auto parameters = model->named_parameters();
  {
    torch::NoGradGuard guard;
    std::set<std::string> expected;
    for (const auto& item : fixture.values) {
      if (item.first.rfind("parameter/", 0) == 0 || item.first.rfind("frozen_parameter/", 0) == 0) {
        const auto name = item.first.substr(item.first.find('/') + 1);
        TORCH_CHECK(parameters.contains(name), "Fixture parameter absent from model: ", name);
        TORCH_CHECK(parameters[name].sizes() == item.second.sizes(), "Parameter shape mismatch: ", name);
        parameters[name].copy_(item.second);
        if (item.first.rfind("frozen_parameter/", 0) == 0) parameters[name].set_requires_grad(false);
        expected.insert(name);
      }
    }
    TORCH_CHECK(expected.size() == parameters.size(), "Fixture must map every C++ parameter exactly once");
  }
  const auto input = [&](const std::string& name, bool gradient) {
    auto value = fixture.get("input", name).to(device).clone();
    if (gradient) value.set_requires_grad(true);
    return value;
  };
  Batch batch{{input("static_continuous", true), input("static_categorical", false)},
              {input("past_continuous", true), input("past_categorical", false)},
              {input("future_continuous", true), input("future_categorical", false)}};
  std::map<std::string, Tensor> diagnostics;
  const auto output = model->forward(batch, &diagnostics);
  replication::retain_embedding_gradients(diagnostics);
  const auto loss = replication::reference_quantile_loss(output.predictions, fixture.get("input", "targets").to(device), config.quantiles);
  Comparisons comparisons;
  for (const auto& pair : std::vector<std::pair<std::string, Tensor>>{
      {"predictions", output.predictions}, {"static_weights", output.static_weights},
      {"past_weights", output.past_weights}, {"future_weights", output.future_weights},
      {"attention_weights", output.attention_weights}, {"loss", loss}})
    comparisons.check("output/" + pair.first, pair.second, fixture.get("output", pair.first));
  for (const auto& item : fixture.values) {
    if (item.first.rfind("activation/", 0) == 0) {
      const auto name = item.first.substr(11);
      comparisons.check(item.first, diagnostics.at(name), item.second);
    }
  }
  loss.backward();
  const auto norms = replication::sparse_embedding_norms(config, diagnostics);
  for (const auto& pair : std::vector<std::pair<std::string, Tensor>>{
      {"static_continuous", batch.statics.continuous}, {"past_continuous", batch.past.continuous},
      {"future_continuous", batch.future.continuous}}) {
    const auto actual = pair.second.numel() ? pair.second.grad() : torch::empty_like(pair.second);
    comparisons.check("input_gradient/" + pair.first, actual, fixture.get("input_gradient", pair.first));
  }
  for (const auto& item : parameters) {
    if (!item.value().requires_grad()) continue;
    comparisons.check("gradient/" + item.key(), item.value().grad(), fixture.get("gradient", item.key()));
    const auto magnitude = norms.count(item.key()) ? norms.at(item.key()) : item.value().grad().norm();
    comparisons.check("gradient_norm/" + item.key(), magnitude, fixture.get("gradient_norm", item.key()));
  }
  replication::ReferenceAdam optimizer;
  optimizer.step(parameters, norms);
  for (const auto& item : parameters) {
    if (!item.value().requires_grad()) continue;
    comparisons.check("updated_parameter/" + item.key(), item.value(), fixture.get("updated_parameter", item.key()), 2e-6, 2e-4);
  }
  for (int step = 2; step <= 3; ++step) {
    if (step == 2) {
      // Resume the exact tested optimizer state once; later reference updates
      // therefore also validate serialization of moments and iteration count.
      std::stringstream checkpoint;
      torch::serialize::OutputArchive saved;
      optimizer.save(saved);
      saved.save_to(checkpoint);
      torch::serialize::InputArchive loaded;
      loaded.load_from(checkpoint);
      optimizer.load(loaded, parameters);
    }
    const auto prefix = "step" + std::to_string(step) + "_";
    const auto step_input = [&](const std::string& name) { return fixture.get(prefix + "input", name).to(device).clone(); };
    Batch next_batch{{step_input("static_continuous"), step_input("static_categorical")},
                     {step_input("past_continuous"), step_input("past_categorical")},
                     {step_input("future_continuous"), step_input("future_categorical")}};
    model->zero_grad();
    std::map<std::string, Tensor> next_diagnostics;
    const auto next_output = model->forward(next_batch, &next_diagnostics);
    replication::retain_embedding_gradients(next_diagnostics);
    const auto next_loss = replication::reference_quantile_loss(next_output.predictions, fixture.get(prefix + "input", "targets").to(device), config.quantiles);
    for (const auto& pair : std::vector<std::pair<std::string, Tensor>>{
        {"predictions", next_output.predictions}, {"static_weights", next_output.static_weights},
        {"past_weights", next_output.past_weights}, {"future_weights", next_output.future_weights},
        {"attention_weights", next_output.attention_weights}, {"loss", next_loss}})
      comparisons.check(prefix + "output/" + pair.first, pair.second, fixture.get(prefix + "output", pair.first));
    next_loss.backward();
    const auto next_norms = replication::sparse_embedding_norms(config, next_diagnostics);
    for (const auto& item : parameters) {
      if (!item.value().requires_grad()) continue;
      comparisons.check(prefix + "gradient/" + item.key(), item.value().grad(), fixture.get(prefix + "gradient", item.key()));
      const auto magnitude = next_norms.count(item.key()) ? next_norms.at(item.key()) : item.value().grad().norm();
      comparisons.check(prefix + "gradient_norm/" + item.key(), magnitude, fixture.get(prefix + "gradient_norm", item.key()));
    }
    optimizer.step(parameters, next_norms);
    for (const auto& item : parameters) {
      if (!item.value().requires_grad()) continue;
      comparisons.check(prefix + "updated_parameter/" + item.key(), item.value(), fixture.get(prefix + "updated_parameter", item.key()), 2e-6, 2e-4);
    }
  }
  torch::OrderedDict<std::string, Tensor> probe_parameters;
  for (const auto* name : {"dense_large", "dense_tiny", "sparse"})
    probe_parameters.insert(name, fixture.get("optimizer_probe_initial", name).to(device).clone().set_requires_grad(true));
  replication::ReferenceAdam probe_optimizer;
  for (int step = 1; step <= 3; ++step) {
    const auto prefix = "optimizer_probe" + std::to_string(step) + "_";
    for (const auto* name : {"dense_large", "dense_tiny"})
      probe_parameters[name].mutable_grad() = fixture.get(prefix + "gradient", name).to(device).clone();
    const auto occurrence_values = fixture.get(prefix + "gradient", "sparse_values").to(device);
    const auto occurrence_ids = fixture.get(prefix + "gradient", "sparse_indices").to(device);
    auto table_gradient = torch::zeros_like(probe_parameters["sparse"]);
    table_gradient.index_add_(0, occurrence_ids, occurrence_values);
    probe_parameters["sparse"].mutable_grad() = table_gradient;
    probe_optimizer.step(probe_parameters, {{"sparse", occurrence_values.norm()}});
    for (const auto& item : probe_parameters)
      comparisons.check(prefix + "updated_parameter/" + item.key(), item.value(),
                        fixture.get(prefix + "updated_parameter", item.key()), 2e-6, 2e-4);
  }
  auto probe_predictions = fixture.get("loss_probe", "predictions").to(device).clone().set_requires_grad(true);
  const auto probe_targets = fixture.get("loss_probe", "targets").to(device);
  const auto probe_loss = replication::reference_quantile_loss(probe_predictions, probe_targets, config.quantiles);
  comparisons.check("loss_probe/loss", probe_loss, fixture.get("loss_probe", "loss"));
  probe_loss.backward();
  comparisons.check("loss_probe/prediction_gradient", probe_predictions.grad(), fixture.get("loss_probe", "prediction_gradient"));
  std::cout << std::setprecision(9) << "{\"fixture\":\"" << directory.filename().string()
            << "\",\"device\":\"" << device.str() << "\",\"checks\":" << comparisons.checks << ",\"failures\":" << comparisons.failures
            << ",\"max_absolute_difference\":" << comparisons.max_difference
            << ",\"max_tolerance_ratio\":" << comparisons.max_ratio << "}\n";
  return comparisons.failures == 0 ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    TORCH_CHECK(argc == 2 || (argc == 4 && std::string(argv[2]) == "--device"),
                "Usage: compare_reference FIXTURE_DIRECTORY [--device cpu|cuda]");
    const std::string device_name = argc == 4 ? argv[3] : "cpu";
    TORCH_CHECK(device_name == "cpu" || device_name == "cuda", "Device must be cpu or cuda");
    if (device_name == "cuda" && !torch::cuda::is_available()) {
      std::cerr << "CUDA unavailable; parity not established on CUDA\n";
      return 77;
    }
    return run(argv[1], torch::Device(device_name));
  } catch (const std::exception& error) {
    std::cerr << "Reference parity error: " << error.what() << '\n';
    return 2;
  }
}
