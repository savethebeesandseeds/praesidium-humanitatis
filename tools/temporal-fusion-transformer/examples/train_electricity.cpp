#include "praesidium-humanitatis/tft.h"
#include "../replication/electricity_data.h"
#include "../replication/reference_optimizer.h"
#include "../replication/reference_loss.h"

#include <torch/cuda.h>
#include <torch/serialize.h>
#include <torch/version.h>
#include <ATen/Context.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#ifdef __linux__
#include <sys/resource.h>
#endif

namespace {
using namespace praesidium_humanitatis;
namespace rep = praesidium_humanitatis::replication;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr int64_t batch_size = 64, max_epochs = 100, patience = 5;
constexpr double minimum_delta = 1e-4;

struct Options {
  fs::path data, initialization, orders, output;
  fs::path verify_data_output;
  std::string mode = "full", device = "cpu";
  int64_t seed = -1, epochs = 100, threads = 1, train_windows = -1, valid_windows = -1;
  int64_t profile_batches = 4;
  bool help = false, explicit_epochs = false;
};

Options parse(int argc, char** argv) {
  Options o;
  std::set<std::string> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string key(argv[i]);
    if (key == "--help") { o.help = true; continue; }
    TORCH_CHECK(seen.insert(key).second && i + 1 < argc, "Duplicate flag or missing value: ", key);
    const std::string value(argv[++i]);
    if (key == "--data") o.data = value;
    else if (key == "--initialization") o.initialization = value;
    else if (key == "--orders" || key == "--order-dir") o.orders = value;
    else if (key == "--output") o.output = value;
    else if (key == "--verify-data-output") o.verify_data_output = value;
    else if (key == "--mode") o.mode = value;
    else if (key == "--device") o.device = value;
    else if (key == "--seed") o.seed = rep::integer(value, key);
    else if (key == "--threads") o.threads = rep::integer(value, key);
    else if (key == "--epochs") { o.epochs = rep::integer(value, key); o.explicit_epochs = true; }
    else if (key == "--train-windows") o.train_windows = rep::integer(value, key);
    else if (key == "--valid-windows") o.valid_windows = rep::integer(value, key);
    else if (key == "--profile-batches") o.profile_batches = rep::integer(value, key);
    else TORCH_CHECK(false, "Unknown argument: ", key);
  }
  if (o.help) return o;
  if (!o.verify_data_output.empty()) {
    TORCH_CHECK(!o.data.empty() && o.seed >= 0 && o.seed <= 0xffffffffLL && o.output.empty(),
                "Data verification requires --data and --seed, without --output");
    TORCH_CHECK(!fs::exists(o.verify_data_output), "Verification output already exists");
    return o;
  }
  TORCH_CHECK(!o.data.empty() && !o.initialization.empty() && !o.orders.empty() &&
                  !o.output.empty() && o.seed >= 0 && o.seed <= 0xffffffffLL,
              "--data, --initialization, --orders, --output and --seed are required");
  TORCH_CHECK(o.mode == "full" || o.mode == "smoke" || o.mode == "profile", "Invalid --mode");
  TORCH_CHECK(o.device == "cpu" || o.device == "cuda", "Invalid --device");
  TORCH_CHECK(o.threads >= 1 && o.threads <= 32, "--threads must be in [1,32]");
  if (o.mode != "full" && !o.explicit_epochs) o.epochs = 1;
  TORCH_CHECK(o.epochs >= 1 && o.epochs <= max_epochs, "--epochs must be in [1,100]");
  if (o.mode == "full") {
    TORCH_CHECK(o.epochs == max_epochs && (o.train_windows == -1 || o.train_windows == 450000) &&
                    (o.valid_windows == -1 || o.valid_windows == 50000),
                "Full protocol fixes 100 maximum epochs, 450000 training and 50000 validation windows");
    const std::set<int64_t> seeds{20260918, 20261918, 20262918, 20263918, 20264918};
    TORCH_CHECK(seeds.count(o.seed), "Full protocol requires one of its five predeclared seeds");
    o.train_windows = 450000; o.valid_windows = 50000;
  } else {
    if (o.train_windows == -1) o.train_windows = o.mode == "profile" ? 256 : 128;
    if (o.valid_windows == -1) o.valid_windows = 64;
    TORCH_CHECK(o.train_windows >= 1 && o.train_windows <= 450000 &&
                    o.valid_windows >= 1 && o.valid_windows <= 50000,
                "Invalid smoke/profile sample count");
  }
  TORCH_CHECK(o.profile_batches >= 1 && o.profile_batches <= 1000, "Invalid profile batch limit");
  if (o.mode == "profile") o.epochs = 1;
  TORCH_CHECK(!fs::exists(o.output), "Output already exists; choose a new directory: ", o.output.string());
  return o;
}

std::string read_text(const fs::path& file) {
  std::ifstream input(file);
  TORCH_CHECK(input.good(), "Cannot read ", file.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string json_string(const std::string& text, const std::string& key) {
  std::smatch match;
  TORCH_CHECK(std::regex_search(text, match, std::regex("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"")),
              "Missing string in provenance metadata: ", key);
  return match[1].str();
}

std::string quote(const std::string& text) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') output << '\\' << c;
    else if (c < 0x20) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
    else output << c;
  }
  output << '"';
  return output.str();
}

double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
void synchronize(torch::Device device) { if (device.is_cuda()) torch::cuda::synchronize(); }

int64_t peak_host_bytes() {
#ifdef __linux__
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) return static_cast<int64_t>(usage.ru_maxrss) * 1024;
#endif
  return -1;
}

Config configuration() {
  Config config;
  config.hidden_size = 160; config.attention_heads = 4; config.dropout = 0.1;
  config.static_continuous = 0; config.static_categorical_cardinalities = {370};
  config.past_continuous = 4; config.future_continuous = 3;
  config.future_continuous_past_indices = {0, 1, 2};
  config.layer_norm_epsilon = 1e-3; config.ordered_quantiles = false;
  return config;
}

void initialize(const Options& o, TFT& model, torch::Device device) {
  const auto settings = rep::read_settings(o.initialization / "config.tsv");
  for (const auto& expected : std::map<std::string, std::string>{
           {"hidden_size", "160"}, {"attention_heads", "4"}, {"static_continuous", "0"},
           {"past_continuous", "4"}, {"future_continuous", "3"},
           {"static_categorical_cardinalities", "370"}, {"past_categorical_cardinalities", ""},
           {"future_categorical_cardinalities", ""}, {"future_continuous_past_indices", "0,1,2"},
           {"future_categorical_past_indices", ""}})
    TORCH_CHECK(settings.at(expected.first) == expected.second, "Initialization schema mismatch: ", expected.first);
  const auto metadata = read_text(o.initialization / "metadata.json");
  TORCH_CHECK(std::regex_search(metadata, std::regex("\"initialization\"\\s*:\\s*\"keras\"")) &&
                  std::regex_search(metadata, std::regex("\"preset\"\\s*:\\s*\"electricity\"")),
              "Benchmark initialization must come from --preset electricity --initialization keras");
  std::smatch match;
  TORCH_CHECK(std::regex_search(metadata, match, std::regex("\"seed\"\\s*:\\s*([0-9]+)")) &&
                  rep::integer(match[1].str(), "initialization seed") == o.seed,
              "Initialization and training seeds must match");
  TORCH_CHECK(metadata.find("53f0046e12b9b79ffea096516b2278774e65d760df684f97600341b9abef9a2c") != std::string::npos,
              "Initialization must identify the pinned upstream model hash");
  TORCH_CHECK(json_string(metadata, "reference_commit") == "5b09c22d73a9d35eb6c5d2a99b95677a45053466",
              "Initialization must identify the pinned upstream revision");
  std::ifstream manifest(o.initialization / "manifest.tsv");
  std::string line;
  TORCH_CHECK(std::getline(manifest, line) && line == "kind\tname\tdtype\tshape\tfile",
              "Invalid initialization manifest");
  auto parameters = model->named_parameters();
  std::set<std::string> loaded, frozen;
  torch::NoGradGuard guard;
  while (std::getline(manifest, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto cells = rep::fields(line, '\t');
    TORCH_CHECK(cells.size() == 5, "Invalid initialization record");
    if (cells[0] != "parameter" && cells[0] != "frozen_parameter") continue;
    TORCH_CHECK(parameters.contains(cells[1]) && loaded.insert(cells[1]).second,
                "Unknown or duplicate initialization parameter: ", cells[1]);
    auto value = rep::read_binary(o.initialization, cells[4], cells[2], cells[3]);
    TORCH_CHECK(value.scalar_type() == torch::kFloat32 && value.sizes() == parameters[cells[1]].sizes() &&
                    torch::isfinite(value).all().item<bool>(), "Invalid initialized weights: ", cells[1]);
    parameters[cells[1]].copy_(value.to(device));
    if (cells[0] == "frozen_parameter") {
      TORCH_CHECK(value.eq(0).all().item<bool>(), "Frozen LSTM redundant bias must be zero");
      parameters[cells[1]].set_requires_grad(false);
      frozen.insert(cells[1]);
    }
  }
  TORCH_CHECK(loaded.size() == parameters.size(), "Initialization does not cover every parameter");
  TORCH_CHECK(frozen == std::set<std::string>({"network.encoder.bias_hh_l0", "network.decoder.bias_hh_l0"}),
              "Only redundant LSTM biases may be frozen");
}

std::vector<int64_t> sequential(int64_t begin, int64_t count) {
  std::vector<int64_t> indices(static_cast<size_t>(count));
  std::iota(indices.begin(), indices.end(), begin);
  return indices;
}

torch::Tensor epoch_order(const Options& o, int64_t epoch) {
  std::ostringstream name;
  name << "epoch-" << std::setw(4) << std::setfill('0') << epoch << ".i64.bin";
  auto order = rep::read_binary(o.orders, name.str(), "<i8", std::to_string(o.train_windows));
  std::vector<uint8_t> visited(static_cast<size_t>(o.train_windows), 0);
  const auto* indices = order.data_ptr<int64_t>();
  for (int64_t i = 0; i < o.train_windows; ++i) {
    const auto index = indices[i];
    TORCH_CHECK(index >= 0 && index < o.train_windows && !visited[index],
                "Epoch order must be a permutation of the selected training windows");
    visited[index] = 1;
  }
  return order;
}

double validation_loss(const rep::ElectricityData& data, const torch::Tensor& windows,
                        int64_t count, TFT& model, torch::Device device) {
  torch::NoGradGuard guard;
  model->eval();
  double total = 0.;
  for (int64_t start = 0; start < count; start += batch_size) {
    const auto size = std::min(batch_size, count - start);
    auto batch = data.batch(windows, sequential(start, size), device);
    const auto loss = replication::reference_quantile_loss(model->forward(batch.input).predictions, batch.targets,
                                    model->config.quantiles).item<double>();
    TORCH_CHECK(std::isfinite(loss), "Nonfinite validation loss");
    total += size * loss;
    if ((start / batch_size + 1) % 100 == 0)
      std::cout << "validation windows=" << start + size << '/' << count << '\n' << std::flush;
  }
  return total / count;
}

void save_best(const fs::path& path, const TFT& model, const rep::ReferenceAdam& optimizer,
                int64_t epoch, int64_t seed, double validation) {
  torch::NoGradGuard guard;
  torch::serialize::OutputArchive archive;
  archive.write("schema", c10::IValue("tft.electricity.reference-training.v1"));
  archive.write("epoch", c10::IValue(epoch));
  archive.write("seed", c10::IValue(seed));
  archive.write("validation_loss", c10::IValue(validation));
  torch::serialize::OutputArchive weights(archive.compilation_unit());
  model->save(weights); archive.write("model", weights);
  torch::serialize::OutputArchive moments(archive.compilation_unit());
  optimizer.save(moments); archive.write("optimizer", moments);
  archive.save_to(path.string());
}

void restore_best(const fs::path& path, TFT& model, torch::Device device) {
  torch::serialize::InputArchive archive, weights;
  archive.load_from(path.string(), device);
  archive.read("model", weights);
  model->load(weights);
  model->eval();
}

struct Risks {
  std::array<double, 24> denominator{}, p10{}, p50{}, p90{};
  int64_t windows = 0;
  void add(int64_t h, double target, double lower, double median, double upper) {
    TORCH_CHECK(std::isfinite(target) && std::isfinite(lower) && std::isfinite(median) && std::isfinite(upper),
                "Nonfinite inverse-scaled forecast or target");
    denominator[h] += std::abs(target);
    const auto a = target - median, b = target - upper, c = target - lower;
    p10[h] += std::max(.1 * c, -.9 * c);
    p50[h] += std::max(.5 * a, -.5 * a);
    p90[h] += std::max(.9 * b, -.1 * b);
  }
  void quantile_json(std::ostream& out, const std::array<double, 24>& losses) const {
    double numerator_sum = 0., denominator_sum = 0., horizon_sum = 0.;
    for (size_t h = 0; h < denominator.size(); ++h) {
      TORCH_CHECK(denominator[h] > 0. && std::isfinite(denominator[h]) && std::isfinite(losses[h]),
                  "Normalized risk has an invalid denominator or sum");
      numerator_sum += losses[h]; denominator_sum += denominator[h];
      horizon_sum += 2. * losses[h] / denominator[h];
    }
    out << "{\"paper_pooled\":" << 2. * numerator_sum / denominator_sum
        << ",\"reference_horizon_mean\":" << horizon_sum / denominator.size() << ",\"by_horizon\":[";
    for (size_t h = 0; h < denominator.size(); ++h) {
      if (h) out << ',';
      out << 2. * losses[h] / denominator[h];
    }
    out << "]}";
  }
  void json(std::ostream& out) const {
    out << "{\"windows\":" << windows << ",\"quantiles\":{\"p50\":";
    quantile_json(out, p50); out << ",\"p90\":"; quantile_json(out, p90);
    out << ",\"p10\":"; quantile_json(out, p10); out << "}}";
  }
};

struct Evaluation { Risks model, persistence, seasonal; };

void accumulate(const rep::ElectricityData& data, const rep::ElectricityBatch& batch,
                 const torch::Tensor& predictions, Evaluation& results) {
  TORCH_CHECK(predictions.device().is_cpu() && predictions.scalar_type() == torch::kFloat32 &&
                  predictions.is_contiguous() && predictions.dim() == 3 &&
                  predictions.size(0) == static_cast<int64_t>(batch.entities.size()) &&
                  predictions.size(1) == 24 && predictions.size(2) == 3,
              "Expected contiguous CPU predictions [batch,24,3]");
  const auto* scales = data.target_stats.data_ptr<double>();
  const auto* raw_targets = data.raw_targets.data_ptr<double>();
  const auto* predicted = predictions.data_ptr<float>();
  const auto size = predictions.size(0);
  for (int64_t b = 0; b < size; ++b) {
    const auto entity = batch.entities[b], origin = batch.starts[b];
    const auto mean = scales[2 * entity], scale = scales[2 * entity + 1];
    const double persistent = raw_targets[origin + 167];
    for (int64_t h = 0; h < 24; ++h) {
      const double target = raw_targets[origin + 168 + h];
      const auto offset = (b * 24 + h) * 3;
      const double lower = predicted[offset] * scale + mean;
      const double median = predicted[offset + 1] * scale + mean;
      const double upper = predicted[offset + 2] * scale + mean;
      const double seasonal = raw_targets[origin + 144 + h];
      results.model.add(h, target, lower, median, upper);
      results.persistence.add(h, target, persistent, persistent, persistent);
      results.seasonal.add(h, target, seasonal, seasonal, seasonal);
    }
  }
  results.model.windows += size; results.persistence.windows += size; results.seasonal.windows += size;
}

Evaluation evaluate(const rep::ElectricityData& data, const torch::Tensor& windows,
                     int64_t count, TFT& model, torch::Device device) {
  torch::NoGradGuard guard;
  model->eval();
  Evaluation results;
  for (int64_t start = 0; start < count; start += batch_size) {
    const auto size = std::min(batch_size, count - start);
    auto batch = data.batch(windows, sequential(start, size), device);
    auto predictions = model->forward(batch.input).predictions.to(torch::kCPU).contiguous();
    accumulate(data, batch, predictions, results);
    if ((start / batch_size + 1) % 100 == 0)
      std::cout << "evaluation windows=" << start + size << '/' << count << '\n' << std::flush;
  }
  return results;
}

int verify_data(const Options& o) {
  torch::set_num_threads(1);
  rep::ElectricityData data(o.data);
  const auto windows = data.windows("train_windows_" + std::to_string(o.seed));
  TORCH_CHECK(windows.size(0) >= 2, "Data verification needs two training windows");
  const auto batch = data.batch(windows, {0, 1}, torch::Device(torch::kCPU));
  const auto predictions = (batch.targets.unsqueeze(-1).expand({2, 24, 3}) * 0.9).contiguous();
  Evaluation metrics;
  accumulate(data, batch, predictions, metrics);
  if (!o.verify_data_output.parent_path().empty()) fs::create_directories(o.verify_data_output.parent_path());
  TORCH_CHECK(fs::create_directory(o.verify_data_output), "Cannot reserve verification output");
  std::ofstream manifest(o.verify_data_output / "tensors.tsv");
  manifest.exceptions(std::ios::badbit | std::ios::failbit);
  const auto save = [&](const std::string& name, const torch::Tensor& value) {
    const auto tensor = value.contiguous();
    const auto filename = name + ".bin";
    const auto dtype = tensor.scalar_type() == torch::kInt64 ? "<i8" : "<f4";
    std::ofstream output(o.verify_data_output / filename, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(static_cast<const char*>(tensor.const_data_ptr()),
                 static_cast<std::streamsize>(tensor.numel() * tensor.element_size()));
    output.close();
    manifest << name << '\t' << dtype << '\t';
    for (int64_t i = 0; i < tensor.dim(); ++i) {
      if (i) manifest << ',';
      manifest << tensor.size(i);
    }
    manifest << '\t' << filename << '\n';
  };
  save("static_continuous", batch.input.statics.continuous);
  save("static_categorical", batch.input.statics.categorical);
  save("past_continuous", batch.input.past.continuous);
  save("past_categorical", batch.input.past.categorical);
  save("future_continuous", batch.input.future.continuous);
  save("future_categorical", batch.input.future.categorical);
  save("targets", batch.targets);
  save("predictions", predictions);
  save("entity_indices", torch::tensor(batch.entities, torch::kInt64));
  save("start_rows", torch::tensor(batch.starts, torch::kInt64));
  manifest.close();
  std::ofstream report(o.verify_data_output / "metrics.json");
  report.exceptions(std::ios::badbit | std::ios::failbit);
  report << std::setprecision(17) << "{\"seed\":" << o.seed
         << ",\"forecast\":\"float32 normalized targets multiplied by 0.9 at every quantile\",\"tft\":";
  metrics.model.json(report);
  report << ",\"persistence\":"; metrics.persistence.json(report);
  report << ",\"seasonal_naive_24h\":"; metrics.seasonal.json(report); report << "}\n";
  report.close();
  std::cout << "Exported first two training windows and actual trainer metrics: " << o.verify_data_output.string() << '\n';
  return 0;
}

int run(const Options& o, bool& owned_output) {
  const auto started = Clock::now();
  torch::set_num_threads(static_cast<int>(o.threads));
  torch::set_num_interop_threads(1);
  at::globalContext().setAllowTF32CuBLAS(false);
  at::globalContext().setAllowTF32CuDNN(false);
  TORCH_CHECK(o.device != "cuda" || torch::cuda::is_available(), "CUDA requested but unavailable");
  const torch::Device device(o.device);
  rep::ElectricityData data(o.data);
  const auto training = data.windows("train_windows_" + std::to_string(o.seed));
  const auto validation = data.windows("valid_windows_" + std::to_string(o.seed));
  TORCH_CHECK(training.size(0) >= o.train_windows && validation.size(0) >= o.valid_windows,
              "Prepared data has insufficient selected windows");
  TORCH_CHECK(o.mode != "full" || (training.size(0) == 450000 && validation.size(0) == 50000),
              "Full protocol requires exact prepared training/validation sample counts");
  const auto shuffle = rep::read_settings(o.orders / "shuffle.tsv");
  TORCH_CHECK(rep::integer(shuffle.at("seed"), "shuffle seed") == o.seed &&
                  rep::integer(shuffle.at("window_count"), "shuffle window_count") == o.train_windows,
              "Shared epoch order seed/count differs from training selection");
  const auto dataset_metadata = read_text(o.data / "metadata.json");
  const auto initialization_metadata = read_text(o.initialization / "metadata.json");
  std::smatch training_hash;
  const auto training_descriptor = std::regex("\"train_windows_" + std::to_string(o.seed) +
      "\"\\s*:\\s*\\{[^}]*\"sha256\"\\s*:\\s*\"([a-f0-9]{64})\"");
  TORCH_CHECK(std::regex_search(dataset_metadata, training_hash, training_descriptor) &&
                  shuffle.at("training_manifest_sha256") == training_hash[1].str(),
              "Epoch orders identify a different training-window manifest");
  TORCH_CHECK(fs::is_regular_file(o.orders / "shuffle.json"), "Missing shuffle provenance");
  auto first_order = epoch_order(o, 0);
  TFT model(configuration());
  model->to(device);
  initialize(o, model, device);
  rep::ReferenceAdam optimizer(.001, .01, .9, .999, 1e-7);
  const auto parameters = model->named_parameters();

  if (!o.output.parent_path().empty()) fs::create_directories(o.output.parent_path());
  TORCH_CHECK(fs::create_directory(o.output), "Could not reserve exclusive output directory");
  owned_output = true;
  fs::copy_file(o.data / "metadata.json", o.output / "dataset-metadata.json");
  fs::copy_file(o.data / "tensors.tsv", o.output / "dataset-tensors.tsv");
  fs::copy_file(o.initialization / "metadata.json", o.output / "initialization-metadata.json");
  fs::copy_file(o.orders / "shuffle.json", o.output / "shuffle.json");
  std::ofstream history(o.output / "epochs.jsonl");
  history.exceptions(std::ios::badbit | std::ios::failbit);
  history << std::setprecision(12);
  // Framework dropout streams differ. Per-update seeds make this C++ run
  // reproducible while the shared fixture and permutations fix initialization/data.
  double checkpoint_best = std::numeric_limits<double>::infinity();
  double stopping_best = checkpoint_best;
  int64_t waiting = 0, best_epoch = -1, completed_epochs = 0, training_updates = 0;
  fs::path best_path;
  std::string stop_reason = o.mode == "profile" ? "profile_batch_limit" : "maximum_epochs";
  double training_seconds = 0., validation_seconds = 0.;
  for (int64_t epoch = 0; epoch < o.epochs; ++epoch) {
    auto order = epoch == 0 ? first_order : epoch_order(o, epoch);
    const auto* indices = order.data_ptr<int64_t>();
    double train_loss = 0.;
    int64_t seen = 0;
    const auto epoch_started = Clock::now();
    model->train();
    for (int64_t start = 0; start < o.train_windows; start += batch_size) {
      if (o.mode == "profile" && start / batch_size >= o.profile_batches) break;
      const auto size = std::min(batch_size, o.train_windows - start);
      std::vector<int64_t> selection(indices + start, indices + start + size);
      auto batch = data.batch(training, selection, device);
      torch::manual_seed(static_cast<uint64_t>(o.seed + training_updates));
      model->zero_grad();
      std::map<std::string, torch::Tensor> diagnostics;
      const auto output = model->forward(batch.input, &diagnostics);
      rep::retain_embedding_gradients(diagnostics);
      const auto loss = replication::reference_quantile_loss(output.predictions, batch.targets, model->config.quantiles);
      const auto value = loss.item<double>();
      TORCH_CHECK(std::isfinite(value), "Nonfinite training loss at epoch ", epoch, " batch ", start / batch_size);
      loss.backward();
      const auto norms = rep::sparse_embedding_norms(model->config, diagnostics);
      optimizer.step(parameters, norms);
      train_loss += value * size; seen += size; ++training_updates;
      // All graph-owning tensors and diagnostics leave scope at this batch end.
      if (start == 0 || (start / batch_size + 1) % 100 == 0)
        std::cout << "epoch=" << epoch << " trained=" << seen << '/' << o.train_windows
                  << " loss=" << value << " elapsed_s=" << seconds(epoch_started) << '\n' << std::flush;
    }
    synchronize(device);
    const double epoch_train_seconds = seconds(epoch_started);
    training_seconds += epoch_train_seconds;
    const auto validation_started = Clock::now();
    const double valid_loss = validation_loss(data, validation, o.valid_windows, model, device);
    synchronize(device);
    const double epoch_validation_seconds = seconds(validation_started);
    validation_seconds += epoch_validation_seconds;
    const bool improved = valid_loss < checkpoint_best;
    if (improved) {
      checkpoint_best = valid_loss; best_epoch = epoch;
      std::ostringstream filename;
      filename << "best-epoch-" << std::setw(4) << std::setfill('0') << epoch << ".pt";
      best_path = o.output / filename.str();
      save_best(best_path, model, optimizer, epoch, o.seed, valid_loss);
    }
    if (valid_loss < stopping_best - minimum_delta) { stopping_best = valid_loss; waiting = 0; }
    else ++waiting;
    ++completed_epochs;
    std::ostringstream entry;
    entry << std::setprecision(12) << "{\"epoch\":" << epoch << ",\"updates\":" << training_updates
          << ",\"train_windows\":" << seen << ",\"training_loss\":" << train_loss / seen
          << ",\"validation_loss\":" << valid_loss << ",\"checkpoint_improved\":" << (improved ? "true" : "false")
          << ",\"early_stopping_wait\":" << waiting << ",\"training_seconds\":" << epoch_train_seconds
          << ",\"validation_seconds\":" << epoch_validation_seconds << ",\"peak_host_memory_bytes\":" << peak_host_bytes() << '}';
    history << entry.str() << '\n'; history.flush();
    std::cout << entry.str() << '\n' << std::flush;
    if (waiting >= patience) { stop_reason = "early_stopping"; break; }
  }
  TORCH_CHECK(!best_path.empty(), "No valid checkpoint was selected");
  restore_best(best_path, model, device);
  const auto evaluation_started = Clock::now();
  const bool full = o.mode == "full";
  const bool score = o.mode != "profile";
  Evaluation metrics;
  int64_t count = 0;
  if (score) {
    const auto evaluated_windows = full ? data.windows("test_windows") : validation;
    count = full ? evaluated_windows.size(0) : o.valid_windows;
    metrics = evaluate(data, evaluated_windows, count, model, device);
  }
  synchronize(device);
  const auto evaluation_seconds = seconds(evaluation_started);
  std::ofstream report(o.output / "report.json");
  report.exceptions(std::ios::badbit | std::ios::failbit);
  report << std::setprecision(12) << "{\n\"schema_version\":1,\"implementation\":\"C++ LibTorch TFT\","
         << "\"protocol\":\"tft.original.electricity.v1\",\"mode\":" << quote(o.mode)
         << ",\"benchmark_protocol_complete\":" << (full ? "true" : "false")
         << ",\"seed\":" << o.seed << ",\"device\":" << quote(o.device) << ",\"threads\":" << o.threads
         << ",\"libtorch\":" << quote(TORCH_VERSION) << ",\"training_windows\":" << o.train_windows
         << ",\"tf32_cublas\":false,\"tf32_cudnn\":false"
         << ",\"validation_windows\":" << o.valid_windows << ",\"batch_size\":64,\"epochs\":" << completed_epochs
         << ",\"updates\":" << training_updates << ",\"best_epoch\":" << best_epoch
         << ",\"best_validation_loss\":" << checkpoint_best << ",\"stop_reason\":" << quote(stop_reason)
         << ",\"best_checkpoint\":" << quote(best_path.filename().string())
         << ",\"target_scale\":\"inverse transformed entity units\","
         << "\"dropout_seed_protocol\":\"seed plus absolute update index; framework RNG streams differ\","
         << "\"dataset_directory\":" << quote(fs::absolute(o.data).string())
         << ",\"initialization_directory\":" << quote(fs::absolute(o.initialization).string())
         << ",\"initialization\":{\"kind\":\"keras\",\"preset\":\"electricity\",\"seed\":" << o.seed
         << ",\"reference_revision\":" << quote(json_string(initialization_metadata, "reference_commit"))
         << ",\"weights_sha256\":" << quote(json_string(initialization_metadata, "original_tf_weights_sha256"))
         << ",\"hash_source\":\"copied initialization metadata; verify in run manifest\"}"
         << ",\"order_directory\":" << quote(fs::absolute(o.orders).string())
         << ",\"training_seconds\":" << training_seconds << ",\"validation_seconds\":" << validation_seconds
         << ",\"evaluation_seconds\":" << evaluation_seconds << ",\"total_seconds\":" << seconds(started)
         << ",\"peak_host_memory_bytes\":" << peak_host_bytes() << ",\"peak_gpu_memory_bytes\":null,\n\"evaluation\":";
  if (score) {
    report << "{\"split\":" << quote(full ? "test" : "validation") << ",\"windows\":" << count
           << ",\"elapsed_seconds\":" << evaluation_seconds
           << ",\"metrics\":{\"tft\":";
    metrics.model.json(report);
    report << ",\"persistence\":"; metrics.persistence.json(report);
    report << ",\"seasonal_naive_24h\":"; metrics.seasonal.json(report);
    report << "}}";
  } else report << "null";
  report << "\n}\n"; report.close();
  std::cout << "Completed " << o.mode << "; report=" << (o.output / "report.json").string() << '\n';
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  Options options;
  bool owned_output = false;
  try {
    options = parse(argc, argv);
    if (options.help) {
      std::cout << "train_electricity --data DIR --initialization FIXTURE --orders DIR --output NEW_DIR --seed N\n"
                   "  --data DIR --seed N --verify-data-output NEW_DIR   export real loader/metric fixture without training\n"
                   "  --mode full|smoke|profile   full is the fixed original Electricity protocol\n"
                   "  --device cpu|cuda --threads 1..32\n"
                   "  Smoke/profile only: --epochs N --train-windows N --valid-windows N\n"
                   "  Profile only: --profile-batches N (default 4)\n"
                   "Requires prepared data and shared shuffle.tsv + epoch-0000.i64.bin orders.\n"
                   "Smoke scores validation; profile reports timing without scores; full scores test after best-validation restore.\n";
      return 0;
    }
    if (!options.verify_data_output.empty()) return verify_data(options);
    return run(options, owned_output);
  } catch (const std::exception& error) {
    std::cerr << "Electricity training failed: " << error.what() << '\n';
    if (owned_output) {
      try {
        std::ofstream failure(options.output / "failure.json");
        failure << "{\"seed\":" << options.seed << ",\"mode\":" << quote(options.mode)
                << ",\"error\":" << quote(error.what()) << "}\n";
      } catch (...) { /* Preserve the original diagnostic and partial artifacts. */ }
    }
    return 1;
  }
}
