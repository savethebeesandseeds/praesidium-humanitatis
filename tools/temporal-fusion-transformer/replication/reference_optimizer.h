#pragma once

// TensorFlow 1.15 tf.keras Adam semantics for the pinned TFT reference:
// individual-variable clipnorm, uncorrected second moment epsilon, and sparse
// clipping before duplicate category contributions are aggregated. This helper
// intentionally lives in the replication harness, not the default library API.
#include "praesidium-humanitatis/tft.h"
#include <torch/torch.h>
#include <cmath>
#include <map>
#include <string>
#include <utility>

namespace praesidium_humanitatis::replication {

using NormOverrides = std::map<std::string, torch::Tensor>;

inline void retain_embedding_gradients(std::map<std::string, torch::Tensor>& diagnostics) {
  for (const auto* name : {"static_embedded", "past_embedded", "future_embedded"})
    diagnostics.at(name).retain_grad();
}

inline NormOverrides sparse_embedding_norms(
    const Config& config, const std::map<std::string, torch::Tensor>& diagnostics) {
  NormOverrides squared;
  const auto add = [&](const std::string& parameter, const std::string& diagnostic, int64_t channel) {
    const auto contribution = diagnostics.at(diagnostic).grad().select(-2, channel).square().sum();
    if (squared.count(parameter)) squared.at(parameter) = squared.at(parameter) + contribution;
    else squared.emplace(parameter, contribution);
  };
  for (size_t i = 0; i < config.static_categorical_cardinalities.size(); ++i)
    add("network.static_embedding.categorical_" + std::to_string(i) + ".weight",
        "static_embedded", config.static_continuous + static_cast<int64_t>(i));
  for (size_t i = 0; i < config.past_categorical_cardinalities.size(); ++i)
    add("network.past_embedding.categorical_" + std::to_string(i) + ".weight",
        "past_embedded", config.past_continuous + static_cast<int64_t>(i));
  for (size_t i = 0; i < config.future_categorical_cardinalities.size(); ++i) {
    const auto shared = config.future_categorical_past_indices.empty()
        ? -1 : config.future_categorical_past_indices.at(i);
    const auto name = shared >= 0
        ? "network.past_embedding.categorical_" + std::to_string(shared) + ".weight"
        : "network.future_embedding.categorical_" + std::to_string(i) + ".weight";
    add(name, "future_embedded", config.future_continuous + static_cast<int64_t>(i));
  }
  for (auto& item : squared) item.second = item.second.sqrt();
  return squared;
}

class ReferenceAdam {
 public:
  struct Moments { torch::Tensor first, second; };
  explicit ReferenceAdam(double learning_rate = 0.001, double clipnorm = 0.01,
                         double beta1 = 0.9, double beta2 = 0.999, double epsilon = 1e-7)
      : learning_rate_(learning_rate), clipnorm_(clipnorm), beta1_(beta1), beta2_(beta2), epsilon_(epsilon) {}

  void step(const torch::OrderedDict<std::string, torch::Tensor>& parameters,
            const NormOverrides& norms = {}) {
    torch::NoGradGuard guard;
    ++iterations_;
    for (const auto& item : parameters) {
      auto parameter = item.value();
      if (!parameter.requires_grad()) continue;
      TORCH_CHECK(parameter.grad().defined(), "Missing reference Adam gradient: ", item.key());
      const auto gradient = parameter.grad();
      const auto magnitude = norms.count(item.key()) ? norms.at(item.key()) : gradient.norm();
      // tf.clip_by_norm uses g * clip_norm / max(norm, clip_norm). Preserve its
      // operation ordering instead of the algebraically equivalent g * ratio.
      const auto clipped = gradient * clipnorm_ / torch::maximum(magnitude, torch::full({}, clipnorm_, gradient.options()));
      auto found = moments_.find(item.key());
      if (found == moments_.end())
        found = moments_.emplace(item.key(), Moments{torch::zeros_like(parameter), torch::zeros_like(parameter)}).first;
      auto& state = found->second;
      const auto b1 = torch::full({}, beta1_, parameter.options());
      const auto b2 = torch::full({}, beta2_, parameter.options());
      // Keras casts hyperparameters to the parameter dtype before subtraction.
      state.first.mul_(b1).add_(clipped * (1 - b1));
      state.second.mul_(b2).add_(clipped.square() * (1 - b2));
      const auto rate = torch::full({}, learning_rate_, parameter.options()) *
          torch::sqrt(1 - torch::pow(b2, iterations_)) / (1 - torch::pow(b1, iterations_));
      parameter.sub_(rate * state.first / (state.second.sqrt() + epsilon_));
    }
  }

  int64_t iterations() const { return iterations_; }
  const std::map<std::string, Moments>& moments() const { return moments_; }

  void save(torch::serialize::OutputArchive& archive) const {
    archive.write("iterations", torch::tensor(iterations_, torch::kInt64));
    archive.write("options", torch::tensor({learning_rate_, clipnorm_, beta1_, beta2_, epsilon_}, torch::kFloat64));
    torch::serialize::OutputArchive states;
    for (const auto& item : moments_) {
      torch::serialize::OutputArchive entry;
      entry.write("first", item.second.first);
      entry.write("second", item.second.second);
      states.write(archive_key(item.first), entry);
    }
    archive.write("moments", states);
  }

  void load(torch::serialize::InputArchive& archive,
            const torch::OrderedDict<std::string, torch::Tensor>& parameters) {
    torch::Tensor iterations, options;
    archive.read("iterations", iterations);
    archive.read("options", options);
    iterations_ = iterations.item<int64_t>();
    options = options.to(torch::kCPU);
    learning_rate_ = options[0].item<double>(); clipnorm_ = options[1].item<double>();
    beta1_ = options[2].item<double>(); beta2_ = options[3].item<double>(); epsilon_ = options[4].item<double>();
    torch::serialize::InputArchive states;
    archive.read("moments", states);
    moments_.clear();
    for (const auto& item : parameters) {
      if (!item.value().requires_grad()) continue;
      torch::serialize::InputArchive entry;
      states.read(archive_key(item.key()), entry);
      Moments state;
      entry.read("first", state.first); entry.read("second", state.second);
      state.first = state.first.to(item.value().options());
      state.second = state.second.to(item.value().options());
      moments_.emplace(item.key(), std::move(state));
    }
  }

 private:
  static std::string archive_key(const std::string& name) {
    // Archive attributes use identifiers; encode every byte to avoid dots and
    // preserve names without collision-prone punctuation replacement.
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded = "p_";
    for (unsigned char character : name) {
      encoded += digits[character >> 4]; encoded += digits[character & 15];
    }
    return encoded;
  }
  double learning_rate_, clipnorm_, beta1_, beta2_, epsilon_;
  int64_t iterations_ = 0;
  std::map<std::string, Moments> moments_;
};

}  // namespace praesidium_humanitatis::replication
