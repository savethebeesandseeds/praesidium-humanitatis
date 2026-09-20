#include "praesidium-humanitatis/tft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using praesidium_humanitatis::Batch;
using praesidium_humanitatis::Config;
using praesidium_humanitatis::FeatureBatch;
using praesidium_humanitatis::Output;
using praesidium_humanitatis::TFT;
using torch::Tensor;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// This oracle consumes only the public state dictionary and input contract. It
// intentionally does not instantiate production submodules, invoke LSTM,
// LayerNorm, embedding, or attention operators, or process whole sequences in
// one operation. Each sample, recurrent step, query and head is evaluated
// separately. This catches wiring/kernel differences; it is not an independent
// reproduction of the paper or proof that the architecture is a good forecaster.
class Reference {
 public:
  explicit Reference(const TFT& model) : config_(model->config) {
    for (const auto& item : model->named_parameters()) {
      parameters_.emplace(item.key(), item.value().detach());
    }
  }

  Output forward(const Batch& batch) {
    std::vector<Tensor> predictions, statics, past, future, attention;
    for (int64_t sample = 0; sample < batch.statics.continuous.size(0); ++sample) {
      const auto result = sample_forward(batch, sample);
      predictions.push_back(result.predictions);
      statics.push_back(result.static_weights);
      past.push_back(result.past_weights);
      future.push_back(result.future_weights);
      attention.push_back(result.attention_weights);
    }
    for (const auto& parameter : parameters_) {
      require(used_.count(parameter.first) == 1,
              "reference did not consume registered parameter " + parameter.first);
    }
    return {torch::stack(predictions), torch::stack(statics), torch::stack(past),
            torch::stack(future), torch::stack(attention)};
  }

 private:
  const Tensor& parameter(const std::string& name) {
    const auto full = "network." + name;
    used_.insert(full);
    return parameters_.at(full);
  }

  bool has(const std::string& name) const {
    return parameters_.count("network." + name) != 0;
  }

  Tensor affine(const std::string& name, const Tensor& vector) {
    auto result = torch::mv(parameter(name + ".weight"), vector);
    if (has(name + ".bias")) result = result + parameter(name + ".bias");
    return result;
  }

  Tensor normalize(const std::string& name, const Tensor& vector) {
    const auto centered = vector - vector.mean();
    // LayerNorm uses population variance and epsilon 1e-5.
    const auto standardized = centered / torch::sqrt(centered.square().mean() + 1e-5);
    return standardized * parameter(name + ".weight") + parameter(name + ".bias");
  }

  static Tensor probabilities(const Tensor& logits) {
    const auto unnormalized = torch::exp(logits - logits.max());
    return unnormalized / unnormalized.sum();
  }

  Tensor gated(const std::string& name, const Tensor& vector) {
    // All comparisons are in eval mode: configured dropout is inactive.
    return affine(name + ".value", vector) * torch::sigmoid(affine(name + ".gate", vector));
  }

  Tensor residual_network(const std::string& name, const Tensor& vector,
                          const Tensor& context = {}) {
    auto hidden = affine(name + ".input_projection", vector);
    if (context.defined()) hidden = hidden + affine(name + ".context_projection", context);
    const auto activated = torch::where(hidden >= 0, hidden, torch::exp(hidden) - 1);
    const auto transformed = affine(name + ".hidden_projection", activated);
    const auto skip = has(name + ".residual_projection.weight")
                          ? affine(name + ".residual_projection", vector) : vector;
    return normalize(name + ".norm", skip + gated(name + ".glu", transformed));
  }

  // Input vectors contain one sample at one time. Keeping each embedded
  // variable separate makes the flattening order and weighted sum explicit.
  std::pair<Tensor, Tensor> select(const std::string& group, const Tensor& continuous,
                                 const Tensor& categorical, const Tensor& context = {}) {
    std::vector<Tensor> variables;
    for (int64_t index = 0; index < continuous.numel(); ++index) {
      variables.push_back(affine(group + "_embedding.continuous_" + std::to_string(index),
                                 continuous.narrow(0, index, 1)));
    }
    for (int64_t index = 0; index < categorical.numel(); ++index) {
      const auto category = categorical[index].item<int64_t>();
      variables.push_back(parameter(group + "_embedding.categorical_" +
                                    std::to_string(index) + ".weight").select(0, category));
    }
    const auto name = group + "_selection";
    const auto weights = probabilities(residual_network(name + ".weights_network",
                                                         torch::cat(variables), context));
    auto value = torch::zeros_like(variables.front());
    for (size_t index = 0; index < variables.size(); ++index) {
      value = value + weights[static_cast<int64_t>(index)] *
          residual_network(name + ".variable_" + std::to_string(index), variables[index]);
    }
    return {value, weights};
  }

  void recurrent_step(const std::string& name, const Tensor& input, Tensor& hidden, Tensor& cell) {
    // PyTorch's single-layer LSTM stores gates in i, f, g, o order, with
    // separate input/recurrent biases. No production LSTM operator is used.
    const auto gates = torch::mv(parameter(name + ".weight_ih_l0"), input) +
                       parameter(name + ".bias_ih_l0") +
                       torch::mv(parameter(name + ".weight_hh_l0"), hidden) +
                       parameter(name + ".bias_hh_l0");
    const int64_t width = config_.hidden_size;
    const auto input_gate = torch::sigmoid(gates.narrow(0, 0, width));
    const auto forget_gate = torch::sigmoid(gates.narrow(0, width, width));
    const auto candidate = torch::tanh(gates.narrow(0, 2 * width, width));
    const auto output_gate = torch::sigmoid(gates.narrow(0, 3 * width, width));
    cell = forget_gate * cell + input_gate * candidate;
    hidden = output_gate * torch::tanh(cell);
  }

  Tensor quantile_head(const Tensor& raw) const {
    if (!config_.ordered_quantiles || config_.quantiles.size() == 1) return raw;
    const auto center = std::distance(config_.quantiles.begin(), std::min_element(
        config_.quantiles.begin(), config_.quantiles.end(), [](double a, double b) {
          return std::abs(a - 0.5) < std::abs(b - 0.5);
        }));
    std::vector<Tensor> result;
    for (int64_t index = 0; index < raw.numel(); ++index) {
      // Calculate every offset directly from the raw vector rather than reuse
      // another output. Stable primitive arithmetic independently implements
      // softplus(x) = max(x, 0) + log(1 + exp(-abs(x))).
      auto value = raw[center];
      for (int64_t gap = std::min<int64_t>(index, center);
           gap <= std::max<int64_t>(index, center); ++gap) {
        if (gap == center) continue;
        const auto x = raw[gap];
        const auto positive = torch::maximum(x, torch::zeros_like(x)) +
                              torch::log1p(torch::exp(-torch::abs(x)));
        value = index < center ? value - positive : value + positive;
      }
      result.push_back(value);
    }
    return torch::stack(result);
  }

  Output sample_forward(const Batch& batch, int64_t sample) {
    const int64_t history = batch.past.continuous.size(1);
    const int64_t horizon = batch.future.continuous.size(1);
    const int64_t total = history + horizon;
    const auto statics = select("static", batch.statics.continuous[sample],
                                batch.statics.categorical[sample]);
    const auto selection_context = residual_network("selection_context", statics.first);
    const auto enrichment_context = residual_network("enrichment_context", statics.first);
    auto hidden = residual_network("hidden_context", statics.first);
    auto cell = residual_network("cell_context", statics.first);
    std::vector<Tensor> temporal, enriched, past_weights, future_weights;
    for (int64_t time = 0; time < total; ++time) {
      const bool historical = time < history;
      const auto& features = historical ? batch.past : batch.future;
      const auto step = historical ? time : time - history;
      const auto selected = select(historical ? "past" : "future",
                                   features.continuous[sample][step],
                                   features.categorical[sample][step], selection_context);
      (historical ? past_weights : future_weights).push_back(selected.second);
      // Carry both encoder states directly into the decoder's first step.
      recurrent_step(historical ? "encoder" : "decoder", selected.first, hidden, cell);
      temporal.push_back(normalize("recurrent_norm", selected.first + gated("recurrent_glu", hidden)));
      enriched.push_back(residual_network("enrichment", temporal.back(), enrichment_context));
    }

    std::vector<Tensor> predictions;
    std::vector<std::vector<Tensor>> head_rows(static_cast<size_t>(config_.attention_heads));
    const int64_t head_width = config_.hidden_size / config_.attention_heads;
    for (int64_t time = history; time < total; ++time) {
      auto mean_head = torch::zeros({head_width}, enriched.front().options());
      for (int64_t head = 0; head < config_.attention_heads; ++head) {
        const auto suffix = std::to_string(head);
        const auto query = affine("attention.query_" + suffix, enriched[time]);
        std::vector<Tensor> scores;
        // Only construct permitted keys. This independently checks the mask's
        // orientation, diagonal inclusion and history/decoder boundary.
        for (int64_t key = 0; key <= time; ++key) {
          scores.push_back(torch::dot(query, affine("attention.key_" + suffix, enriched[key])) /
                           std::sqrt(static_cast<double>(head_width)));
        }
        const auto weights = probabilities(torch::stack(scores));
        auto attended = torch::zeros_like(mean_head);
        for (int64_t key = 0; key <= time; ++key) {
          attended = attended + weights[key] * affine("attention.value", enriched[key]);
        }
        mean_head = mean_head + attended / static_cast<double>(config_.attention_heads);
        head_rows[static_cast<size_t>(head)].push_back(torch::cat({
            weights, torch::zeros({total - time - 1}, weights.options())}));
      }
      const auto attended = affine("attention.output", mean_head);
      const auto attention_skip = normalize("attention_norm", enriched[time] + gated("attention_glu", attended));
      const auto processed = residual_network("positionwise", attention_skip);
      const auto final = normalize("final_norm", temporal[time] + gated("final_glu", processed));
      predictions.push_back(quantile_head(affine("projection", final)));
    }
    std::vector<Tensor> attention;
    for (const auto& rows : head_rows) attention.push_back(torch::stack(rows));
    return {torch::stack(predictions), statics.second, torch::stack(past_weights),
            torch::stack(future_weights), torch::stack(attention)};
  }

  Config config_;
  std::map<std::string, Tensor> parameters_;
  std::set<std::string> used_;
};

// A stable name hash gives different parameters without depending on
// constructor RNG consumption, standard-library hash implementations or device RNG.
uint32_t stable_hash(const std::string& name) {
  uint32_t hash = 2166136261u;
  for (const unsigned char byte : name) hash = (hash ^ byte) * 16777619u;
  return hash;
}

void fixed_parameters(TFT& model, int seed) {
  for (auto& item : model->named_parameters()) {
    uint32_t state = stable_hash(item.key()) ^ static_cast<uint32_t>(seed);
    if (state == 0) state = 1;
    std::vector<double> values(static_cast<size_t>(item.value().numel()));
    for (auto& value : values) {
      // Xorshift32 supplies fixed, non-symmetric, full-rank test matrices.
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      value = 0.8 * (static_cast<double>(state) / 4294967295.0) - 0.4;
      if (item.key().find("norm.weight") != std::string::npos) value = 1.0 + value * 0.3;
    }
    item.value().copy_(torch::tensor(values, torch::kFloat64)
                          .reshape(item.value().sizes()).to(item.value().options()));
  }
}

FeatureBatch features(const std::vector<int64_t>& prefix, int64_t count,
                      const std::vector<int64_t>& cardinalities,
                      const torch::TensorOptions& options, int seed) {
  int64_t positions = 1;
  for (const auto dimension : prefix) positions *= dimension;
  auto shape = prefix;
  shape.push_back(count);
  const auto index = torch::arange(positions * count, torch::kFloat64);
  const auto continuous = (torch::sin(index * 0.43 + seed) +
                           0.4 * torch::cos(index * 0.79 - seed * 0.3))
                              .reshape(shape).to(options);
  shape.back() = static_cast<int64_t>(cardinalities.size());
  std::vector<Tensor> categories;
  for (size_t variable = 0; variable < cardinalities.size(); ++variable) {
    categories.push_back(torch::remainder(torch::arange(positions, torch::kInt64) +
        seed + static_cast<int64_t>(variable), cardinalities[variable]).reshape(prefix));
  }
  const auto categorical = categories.empty()
      ? torch::empty(shape, options.dtype(torch::kInt64))
      : torch::stack(categories, -1).to(options.dtype(torch::kInt64));
  return {continuous, categorical};
}

double compare(const Tensor& actual, const Tensor& expected,
               const std::string& label, double relative, double absolute) {
  require(actual.sizes() == expected.sizes(), label + " shape differs");
  require(torch::isfinite(actual).all().item<bool>() && torch::isfinite(expected).all().item<bool>(),
          label + " contains nonfinite values");
  const auto maximum = (actual - expected).abs().max().item<double>();
  require(torch::allclose(actual, expected, relative, absolute),
          label + " differs from functional reference; max absolute error = " + std::to_string(maximum));
  return maximum;
}

struct Case {
  std::string name;
  Config config;
  int64_t samples;
  int64_t history;
  int64_t horizon;
  int seed;
};

std::vector<Case> cases() {
  Config mixed;
  mixed.hidden_size = 8;
  mixed.attention_heads = 2;
  mixed.dropout = 0.35;  // Eval must disable dropout throughout the graph.
  mixed.static_continuous = 2;
  mixed.past_continuous = 2;
  mixed.future_continuous = 1;
  mixed.static_categorical_cardinalities = {3};
  mixed.past_categorical_cardinalities = {4, 3};
  mixed.future_categorical_cardinalities = {5};
  Config alternate = mixed;
  alternate.hidden_size = 6;
  alternate.attention_heads = 3;
  alternate.quantiles = {0.2, 0.8};
  Config continuous = mixed;
  continuous.hidden_size = 4;
  continuous.attention_heads = 1;
  continuous.future_continuous = 2;
  continuous.static_categorical_cardinalities.clear();
  continuous.past_categorical_cardinalities.clear();
  continuous.future_categorical_cardinalities.clear();
  Config categorical = mixed;
  categorical.hidden_size = 6;
  categorical.static_continuous = categorical.past_continuous = categorical.future_continuous = 0;
  categorical.static_categorical_cardinalities = {3, 4};
  categorical.future_categorical_cardinalities = {5, 2};
  Config single;
  single.hidden_size = 2;
  single.attention_heads = 2;
  single.static_continuous = single.past_continuous = single.future_continuous = 1;
  single.quantiles = {0.5};
  Config ordered = mixed;
  ordered.ordered_quantiles = true;
  ordered.quantiles = {0.05, 0.2, 0.5, 0.8, 0.95};
  Config ordered_nonmedian = alternate;
  ordered_nonmedian.ordered_quantiles = true;
  ordered_nonmedian.quantiles = {0.1, 0.3, 0.8, 0.95};
  Config ordered_single = single;
  ordered_single.ordered_quantiles = true;
  ordered_single.quantiles = {0.8};
  return {{"mixed", mixed, 2, 3, 3, 17},
          {"mixed, short history and three heads", alternate, 3, 1, 4, 41},
          {"continuous only", continuous, 2, 4, 2, 71},
          {"categorical only", categorical, 2, 2, 3, 103},
          {"one variable, one step, head width one", single, 1, 1, 1, 131},
          {"ordered mixed, five quantiles", ordered, 2, 3, 3, 173},
          {"ordered nonmedian center", ordered_nonmedian, 3, 1, 4, 211},
          {"ordered singleton identity", ordered_single, 1, 1, 1, 251}};
}

void run_case(const Case& test, const torch::Device& device) {
  torch::NoGradGuard no_grad;
  auto model = TFT(test.config);
  const auto dtype = device.is_cuda() ? torch::kFloat32 : torch::kFloat64;
  model->to(device, dtype);
  model->eval();
  fixed_parameters(model, test.seed);
  const auto options = torch::TensorOptions().dtype(dtype).device(device);
  const Batch batch{
      features({test.samples}, test.config.static_continuous,
               test.config.static_categorical_cardinalities, options, test.seed),
      features({test.samples, test.history}, test.config.past_continuous,
               test.config.past_categorical_cardinalities, options, test.seed + 1),
      features({test.samples, test.horizon}, test.config.future_continuous,
               test.config.future_categorical_cardinalities, options, test.seed + 2)};
  const auto expected = Reference(model).forward(batch);
  const auto actual = model->forward(batch);
  const double tolerance = device.is_cuda() ? 3e-5 : 2e-10;
  double maximum = 0;
  for (const auto& field : std::vector<std::pair<std::string, std::pair<Tensor, Tensor>>>{
           {"predictions", {actual.predictions, expected.predictions}},
           {"static weights", {actual.static_weights, expected.static_weights}},
           {"past weights", {actual.past_weights, expected.past_weights}},
           {"future weights", {actual.future_weights, expected.future_weights}},
           {"attention weights", {actual.attention_weights, expected.attention_weights}}}) {
    maximum = std::max(maximum, compare(field.second.first, field.second.second,
                                       test.name + ": " + field.first, tolerance, tolerance));
  }
  std::cout << "PASS: " << test.name << "; all five outputs agree; max absolute error " << maximum << '\n';
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
      std::cerr << "Usage: tft_reference_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: tft_reference_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  const auto tests = cases();
  int failures = 0;
  for (const auto& test : tests) {
    try {
      run_case(test, device);
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size()
            << " functional reference cases passed on " << device << '\n';
  return failures == 0 ? 0 : 1;
}
