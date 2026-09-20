#include "praesidium-humanitatis/tft.h"

#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace praesidium_humanitatis {
namespace {

void validate_quantiles(const std::vector<double>& quantiles) {
  TORCH_CHECK(!quantiles.empty(), "At least one quantile is required");
  double previous = 0.0;
  for (const auto q : quantiles) {
    TORCH_CHECK(std::isfinite(q) && q > previous && q < 1.0,
                "Quantiles must be finite, strictly increasing, and in (0, 1)");
    previous = q;
  }
}

int64_t variable_count(int64_t continuous,
                       const std::vector<int64_t>& cardinalities) {
  TORCH_CHECK(continuous >= 0, "Continuous variable counts cannot be negative");
  TORCH_CHECK(cardinalities.size() <=
                  static_cast<size_t>(std::numeric_limits<int64_t>::max() - continuous),
              "Variable count exceeds supported range");
  return continuous + static_cast<int64_t>(cardinalities.size());
}

void validate_group_config(const char* name, int64_t continuous,
                           const std::vector<int64_t>& cardinalities,
                           int64_t hidden_size) {
  const auto count = variable_count(continuous, cardinalities);
  TORCH_CHECK(count > 0, name, " requires at least one variable");
  TORCH_CHECK(count <= std::numeric_limits<int64_t>::max() / hidden_size,
              name, " embedding dimensions exceed supported range");
  for (const auto cardinality : cardinalities) {
    TORCH_CHECK(cardinality > 0, name, " category cardinalities must be positive");
  }
}

void validate_embedding_map(const char* name, const std::vector<int64_t>& indices,
                            int64_t future_count, int64_t past_count) {
  TORCH_CHECK(indices.empty() || indices.size() == static_cast<size_t>(future_count),
              name, " must be empty or contain one entry per future variable");
  for (const auto index : indices) {
    TORCH_CHECK(index >= -1 && index < past_count,
                name, " entries must be -1 or valid past variable indices");
  }
}

bool supported_floating_type(c10::ScalarType dtype) {
  return dtype == torch::kFloat32 || dtype == torch::kFloat64;
}

void validate_features(const FeatureBatch& features, const char* name,
                       int64_t rank, int64_t continuous_count,
                       const std::vector<int64_t>& cardinalities,
                       const torch::Tensor& parameter) {
  TORCH_CHECK(features.continuous.defined() && features.categorical.defined(),
              name, " requires both tensors; use zero-width tensors for absent channels");
  TORCH_CHECK(features.continuous.layout() == torch::kStrided &&
                  features.categorical.layout() == torch::kStrided,
              name, " tensors must use dense strided layout");
  TORCH_CHECK(features.continuous.dim() == rank && features.categorical.dim() == rank,
              name, " tensors must have rank ", rank);
  TORCH_CHECK(features.continuous.size(-1) == continuous_count,
              name, " continuous variable count does not match configuration");
  TORCH_CHECK(features.categorical.size(-1) == static_cast<int64_t>(cardinalities.size()),
              name, " categorical variable count does not match configuration");
  TORCH_CHECK(features.continuous.scalar_type() == parameter.scalar_type(),
              name, " continuous dtype must match the model's float32 or float64 dtype");
  TORCH_CHECK(features.categorical.scalar_type() == torch::kInt64,
              name, " categorical dtype must be int64");
  TORCH_CHECK(features.continuous.device() == parameter.device() &&
                  features.categorical.device() == parameter.device(),
              name, " tensors must be on the model's device, including empty tensors");
  for (int64_t axis = 0; axis < rank - 1; ++axis) {
    TORCH_CHECK(features.continuous.size(axis) > 0,
                name, " batch and temporal dimensions must be positive");
    TORCH_CHECK(features.continuous.size(axis) == features.categorical.size(axis),
                name, " continuous and categorical batch/time dimensions differ");
  }
  TORCH_CHECK(torch::isfinite(features.continuous).all().item<bool>(),
              name, " continuous inputs must be finite; missing values are unsupported");
  for (size_t i = 0; i < cardinalities.size(); ++i) {
    const auto values = features.categorical.select(-1, static_cast<int64_t>(i));
    TORCH_CHECK(values.ge(0).logical_and(values.lt(cardinalities[i])).all().item<bool>(),
                name, " category ", i, " contains an ID outside [0, cardinality)");
  }
}

}  // namespace

void Config::validate() const {
  TORCH_CHECK(hidden_size >= 2, "hidden_size must be at least 2");
  TORCH_CHECK(attention_heads > 0 && hidden_size % attention_heads == 0,
              "attention_heads must be positive and divide hidden_size");
  TORCH_CHECK(std::isfinite(dropout) && dropout >= 0.0 && dropout < 1.0,
              "dropout must be finite and in [0, 1)");
  TORCH_CHECK(std::isfinite(layer_norm_epsilon) && layer_norm_epsilon > 0.0,
              "layer_norm_epsilon must be finite and positive");
  validate_group_config("Static inputs", static_continuous,
                        static_categorical_cardinalities, hidden_size);
  validate_group_config("Past inputs", past_continuous,
                        past_categorical_cardinalities, hidden_size);
  validate_group_config("Future inputs", future_continuous,
                        future_categorical_cardinalities, hidden_size);
  validate_embedding_map("future_continuous_past_indices", future_continuous_past_indices,
                         future_continuous, past_continuous);
  validate_embedding_map("future_categorical_past_indices", future_categorical_past_indices,
                         static_cast<int64_t>(future_categorical_cardinalities.size()),
                         static_cast<int64_t>(past_categorical_cardinalities.size()));
  for (size_t i = 0; i < future_categorical_past_indices.size(); ++i) {
    const auto index = future_categorical_past_indices[i];
    TORCH_CHECK(index == -1 || future_categorical_cardinalities[i] ==
                                  past_categorical_cardinalities[static_cast<size_t>(index)],
                "Shared categorical embeddings require equal cardinalities");
  }
  validate_quantiles(quantiles);
}

namespace detail {

struct GLUImpl : torch::nn::Module {
  GLUImpl(int64_t input, int64_t output, double dropout_probability)
      : value(register_module("value", torch::nn::Linear(input, output))),
        gate(register_module("gate", torch::nn::Linear(input, output))),
        dropout(register_module("dropout", torch::nn::Dropout(dropout_probability))) {}

  torch::Tensor forward(torch::Tensor input) {
    input = dropout(input);
    return value(input) * torch::sigmoid(gate(input));
  }

  torch::nn::Linear value, gate;
  torch::nn::Dropout dropout;
};

struct GRNImpl : torch::nn::Module {
  GRNImpl(int64_t input, int64_t hidden, int64_t output,
          int64_t context_size, double dropout_probability, double norm_epsilon)
      : input_projection(register_module("input_projection", torch::nn::Linear(input, hidden))),
        hidden_projection(register_module("hidden_projection", torch::nn::Linear(hidden, hidden))),
        glu(register_module("glu", std::make_shared<GLUImpl>(hidden, output, dropout_probability))),
        norm(register_module("norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({output}).eps(norm_epsilon)))) {
    if (input != output) {
      residual_projection = register_module("residual_projection", torch::nn::Linear(input, output));
    }
    if (context_size > 0) {
      context_projection = register_module("context_projection",
          torch::nn::Linear(torch::nn::LinearOptions(context_size, hidden).bias(false)));
    }
  }

  torch::Tensor forward(const torch::Tensor& input,
                         const torch::Tensor& context = {}) {
    auto hidden = input_projection(input);
    if (context_projection) {
      TORCH_CHECK(context.defined(), "GRN context is required");
      hidden = hidden + context_projection(context);
    }
    hidden = hidden_projection(torch::elu(hidden));
    auto residual = residual_projection ? residual_projection(input) : input;
    return norm(residual + glu->forward(hidden));
  }

  torch::nn::Linear input_projection, hidden_projection;
  torch::nn::Linear context_projection{nullptr}, residual_projection{nullptr};
  std::shared_ptr<GLUImpl> glu;
  torch::nn::LayerNorm norm;
};

struct EmbeddingGroupImpl : torch::nn::Module {
  EmbeddingGroupImpl(int64_t continuous_count,
                     const std::vector<int64_t>& cardinalities, int64_t hidden,
                     const EmbeddingGroupImpl* past = nullptr,
                     const std::vector<int64_t>& continuous_past_indices = {},
                     const std::vector<int64_t>& categorical_past_indices = {}) {
    for (int64_t i = 0; i < continuous_count; ++i) {
      const auto source = continuous_past_indices.empty() ? -1 : continuous_past_indices[i];
      if (source >= 0) {
        // Retain the same module, without a second registration path. The past
        // group owns device/dtype migration, serialization and parameter listing.
        continuous.push_back(past->continuous[static_cast<size_t>(source)]);
      } else {
        continuous.push_back(register_module("continuous_" + std::to_string(i),
                                               torch::nn::Linear(1, hidden)));
      }
    }
    for (size_t i = 0; i < cardinalities.size(); ++i) {
      const auto source = categorical_past_indices.empty() ? -1 : categorical_past_indices[i];
      if (source >= 0) {
        categorical.push_back(past->categorical[static_cast<size_t>(source)]);
      } else {
        categorical.push_back(register_module("categorical_" + std::to_string(i),
            torch::nn::Embedding(cardinalities[i], hidden)));
      }
    }
  }

  torch::Tensor forward(const FeatureBatch& features) {
    std::vector<torch::Tensor> embedded;
    embedded.reserve(continuous.size() + categorical.size());
    for (size_t i = 0; i < continuous.size(); ++i) {
      embedded.push_back(continuous[i](features.continuous.narrow(-1, static_cast<int64_t>(i), 1)));
    }
    for (size_t i = 0; i < categorical.size(); ++i) {
      embedded.push_back(categorical[i](features.categorical.select(-1, static_cast<int64_t>(i))));
    }
    return torch::stack(embedded, -2);
  }

  std::vector<torch::nn::Linear> continuous;
  std::vector<torch::nn::Embedding> categorical;
};

struct Selection {
  torch::Tensor values;
  torch::Tensor weights;
};

struct VariableSelectionImpl : torch::nn::Module {
  VariableSelectionImpl(int64_t variables, int64_t hidden, bool contextual,
                         double dropout_probability, double norm_epsilon)
      : weights_network(register_module("weights_network", std::make_shared<GRNImpl>(
            variables * hidden, hidden, variables, contextual ? hidden : 0,
            dropout_probability, norm_epsilon))) {
    for (int64_t i = 0; i < variables; ++i) {
      variable_networks.push_back(register_module("variable_" + std::to_string(i),
          std::make_shared<GRNImpl>(hidden, hidden, hidden, 0, dropout_probability, norm_epsilon)));
    }
  }

  Selection forward(const torch::Tensor& embedding, const torch::Tensor& context = {}) {
    auto weights = torch::softmax(weights_network->forward(embedding.flatten(-2, -1), context), -1);
    std::vector<torch::Tensor> transformed;
    transformed.reserve(variable_networks.size());
    for (size_t i = 0; i < variable_networks.size(); ++i) {
      transformed.push_back(variable_networks[i]->forward(embedding.select(-2, static_cast<int64_t>(i))));
    }
    auto values = (torch::stack(transformed, -2) * weights.unsqueeze(-1)).sum(-2);
    return {values, weights};
  }

  std::shared_ptr<GRNImpl> weights_network;
  std::vector<std::shared_ptr<GRNImpl>> variable_networks;
};

struct AttentionResult {
  torch::Tensor values;
  torch::Tensor weights;
};

struct InterpretableAttentionImpl : torch::nn::Module {
  InterpretableAttentionImpl(int64_t hidden, int64_t heads, double dropout_probability)
      : head_size(hidden / heads),
        value(register_module("value", torch::nn::Linear(torch::nn::LinearOptions(hidden, head_size).bias(false)))),
        output(register_module("output", torch::nn::Linear(torch::nn::LinearOptions(head_size, hidden).bias(false)))),
        dropout(register_module("dropout", torch::nn::Dropout(dropout_probability))) {
    for (int64_t i = 0; i < heads; ++i) {
      queries.push_back(register_module("query_" + std::to_string(i),
          torch::nn::Linear(torch::nn::LinearOptions(hidden, head_size).bias(false))));
      keys.push_back(register_module("key_" + std::to_string(i),
          torch::nn::Linear(torch::nn::LinearOptions(hidden, head_size).bias(false))));
    }
  }

  AttentionResult forward(const torch::Tensor& enriched, int64_t history) {
    const auto total = enriched.size(1);
    const auto horizon = total - history;
    auto decoder = enriched.narrow(1, history, horizon);
    auto positions = torch::arange(total, enriched.options().dtype(torch::kInt64));
    auto query_positions = positions.narrow(0, history, horizon);
    auto forbidden = positions.unsqueeze(0).gt(query_positions.unsqueeze(1)).unsqueeze(0);
    auto shared_values = value(enriched);
    std::vector<torch::Tensor> head_outputs;
    std::vector<torch::Tensor> head_weights;
    for (size_t i = 0; i < queries.size(); ++i) {
      auto scores = torch::matmul(queries[i](decoder), keys[i](enriched).transpose(-2, -1)) /
                    std::sqrt(static_cast<double>(head_size));
      scores = scores.masked_fill(forbidden, -std::numeric_limits<double>::infinity());
      auto weights = torch::softmax(scores, -1);
      head_outputs.push_back(dropout(torch::matmul(weights, shared_values)));
      head_weights.push_back(weights);
    }
    auto averaged = torch::stack(head_outputs, 1).mean(1);
    return {dropout(output(averaged)), torch::stack(head_weights, 1)};
  }

  int64_t head_size;
  torch::nn::Linear value, output;
  torch::nn::Dropout dropout;
  std::vector<torch::nn::Linear> queries, keys;
};

struct TFTNetworkImpl : torch::nn::Module {
  explicit TFTNetworkImpl(const Config& cfg) {
    const auto hidden = cfg.hidden_size;
    const auto dropout = cfg.dropout;
    const auto epsilon = cfg.layer_norm_epsilon;
    static_embedding = register_module("static_embedding", std::make_shared<EmbeddingGroupImpl>(
        cfg.static_continuous, cfg.static_categorical_cardinalities, hidden));
    past_embedding = register_module("past_embedding", std::make_shared<EmbeddingGroupImpl>(
        cfg.past_continuous, cfg.past_categorical_cardinalities, hidden));
    future_embedding = register_module("future_embedding", std::make_shared<EmbeddingGroupImpl>(
        cfg.future_continuous, cfg.future_categorical_cardinalities, hidden, past_embedding.get(),
        cfg.future_continuous_past_indices, cfg.future_categorical_past_indices));
    static_selection = register_module("static_selection", std::make_shared<VariableSelectionImpl>(
        variable_count(cfg.static_continuous, cfg.static_categorical_cardinalities), hidden, false, dropout, epsilon));
    past_selection = register_module("past_selection", std::make_shared<VariableSelectionImpl>(
        variable_count(cfg.past_continuous, cfg.past_categorical_cardinalities), hidden, true, dropout, epsilon));
    future_selection = register_module("future_selection", std::make_shared<VariableSelectionImpl>(
        variable_count(cfg.future_continuous, cfg.future_categorical_cardinalities), hidden, true, dropout, epsilon));
    selection_context = register_module("selection_context", std::make_shared<GRNImpl>(hidden, hidden, hidden, 0, dropout, epsilon));
    enrichment_context = register_module("enrichment_context", std::make_shared<GRNImpl>(hidden, hidden, hidden, 0, dropout, epsilon));
    hidden_context = register_module("hidden_context", std::make_shared<GRNImpl>(hidden, hidden, hidden, 0, dropout, epsilon));
    cell_context = register_module("cell_context", std::make_shared<GRNImpl>(hidden, hidden, hidden, 0, dropout, epsilon));
    encoder = register_module("encoder", torch::nn::LSTM(torch::nn::LSTMOptions(hidden, hidden).batch_first(true)));
    decoder = register_module("decoder", torch::nn::LSTM(torch::nn::LSTMOptions(hidden, hidden).batch_first(true)));
    recurrent_glu = register_module("recurrent_glu", std::make_shared<GLUImpl>(hidden, hidden, dropout));
    recurrent_norm = register_module("recurrent_norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({hidden}).eps(epsilon)));
    enrichment = register_module("enrichment", std::make_shared<GRNImpl>(hidden, hidden, hidden, hidden, dropout, epsilon));
    attention = register_module("attention", std::make_shared<InterpretableAttentionImpl>(hidden, cfg.attention_heads, dropout));
    attention_glu = register_module("attention_glu", std::make_shared<GLUImpl>(hidden, hidden, dropout));
    attention_norm = register_module("attention_norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({hidden}).eps(epsilon)));
    positionwise = register_module("positionwise", std::make_shared<GRNImpl>(hidden, hidden, hidden, 0, dropout, epsilon));
    final_glu = register_module("final_glu", std::make_shared<GLUImpl>(hidden, hidden, 0.0));
    final_norm = register_module("final_norm", torch::nn::LayerNorm(torch::nn::LayerNormOptions({hidden}).eps(epsilon)));
    projection = register_module("projection", torch::nn::Linear(hidden, static_cast<int64_t>(cfg.quantiles.size())));
  }

  Output forward(const Batch& batch, std::map<std::string, torch::Tensor>* diagnostics) {
    auto static_embedded = static_embedding->forward(batch.statics);
    auto statics = static_selection->forward(static_embedded);
    auto selection_state = selection_context->forward(statics.values);
    auto selection = selection_state.unsqueeze(1);
    auto past_embedded = past_embedding->forward(batch.past);
    auto past = past_selection->forward(past_embedded, selection);
    auto future_embedded = future_embedding->forward(batch.future);
    auto future = future_selection->forward(future_embedded, selection);
    auto hidden_state = hidden_context->forward(statics.values);
    auto initial_hidden = hidden_state.unsqueeze(0);
    auto cell_state = cell_context->forward(statics.values);
    auto initial_cell = cell_state.unsqueeze(0);
    auto encoded = encoder->forward(past.values, std::make_tuple(initial_hidden, initial_cell));
    auto decoded = decoder->forward(future.values, std::get<1>(encoded));
    auto recurrent = torch::cat({std::get<0>(encoded), std::get<0>(decoded)}, 1);
    auto selected = torch::cat({past.values, future.values}, 1);
    auto temporal = recurrent_norm(selected + recurrent_glu->forward(recurrent));
    auto enrichment_state = enrichment_context->forward(statics.values);
    auto enriched = enrichment->forward(temporal, enrichment_state.unsqueeze(1));
    const auto history = past.values.size(1);
    const auto horizon = future.values.size(1);
    auto attended = attention->forward(enriched, history);
    auto attention_skip = attention_norm(enriched.narrow(1, history, horizon) + attention_glu->forward(attended.values));
    auto processed = positionwise->forward(attention_skip);
    auto final = final_norm(temporal.narrow(1, history, horizon) + final_glu->forward(processed));
    auto raw_projection = projection(final);
    if (diagnostics) {
      *diagnostics = {{"static_embedded", static_embedded},
                      {"past_embedded", past_embedded},
                      {"future_embedded", future_embedded},
                      {"static_selected", statics.values},
                      {"selection_context", selection_state},
                      {"enrichment_context", enrichment_state},
                      {"hidden_context", hidden_state},
                      {"cell_context", cell_state},
                      {"past_selected", past.values},
                      {"future_selected", future.values},
                      {"encoder", std::get<0>(encoded)},
                      {"decoder", std::get<0>(decoded)},
                      {"recurrent", recurrent},
                      {"temporal", temporal},
                      {"enriched", enriched},
                      {"attention_output", attended.values},
                      {"attention_skip", attention_skip},
                      {"processed", processed},
                      {"final", final},
                      {"raw_projection", raw_projection}};
    }
    return {raw_projection, statics.weights, past.weights, future.weights, attended.weights};
  }

  std::shared_ptr<EmbeddingGroupImpl> static_embedding, past_embedding, future_embedding;
  std::shared_ptr<VariableSelectionImpl> static_selection, past_selection, future_selection;
  std::shared_ptr<GRNImpl> selection_context, enrichment_context, hidden_context, cell_context;
  std::shared_ptr<GRNImpl> enrichment, positionwise;
  std::shared_ptr<GLUImpl> recurrent_glu, attention_glu, final_glu;
  std::shared_ptr<InterpretableAttentionImpl> attention;
  torch::nn::LSTM encoder{nullptr}, decoder{nullptr};
  torch::nn::LayerNorm recurrent_norm{nullptr}, attention_norm{nullptr}, final_norm{nullptr};
  torch::nn::Linear projection{nullptr};
};

}  // namespace detail

TFTImpl::TFTImpl(Config configuration) : config(std::move(configuration)) {
  config.validate();
  network_ = register_module("network", std::make_shared<detail::TFTNetworkImpl>(config));
}

Output TFTImpl::forward(const Batch& batch,
                        std::map<std::string, torch::Tensor>* diagnostics) {
  if (diagnostics) diagnostics->clear();
  const auto parameter = parameters().front();
  TORCH_CHECK(parameter.device().is_cpu() || parameter.device().is_cuda(),
              "TFT supports CPU and CUDA devices");
  TORCH_CHECK(supported_floating_type(parameter.scalar_type()),
              "TFT supports float32 and float64 model parameters");
  validate_features(batch.statics, "Static inputs", 2, config.static_continuous,
                     config.static_categorical_cardinalities, parameter);
  validate_features(batch.past, "Past inputs", 3, config.past_continuous,
                     config.past_categorical_cardinalities, parameter);
  validate_features(batch.future, "Future inputs", 3, config.future_continuous,
                     config.future_categorical_cardinalities, parameter);
  const auto batch_size = batch.statics.continuous.size(0);
  TORCH_CHECK(batch.past.continuous.size(0) == batch_size &&
                  batch.future.continuous.size(0) == batch_size,
              "Static, past, and future batch sizes must match");
  auto output = network_->forward(batch, diagnostics);
  if (config.ordered_quantiles && config.quantiles.size() > 1) {
    // Keep the quantile nearest 0.5 unconstrained; build outward with positive
    // gaps. The raw projection is never sorted or detached, so pinball loss
    // trains this parameterization directly. Parameter names/shapes are stable.
    int64_t center = 0;
    for (int64_t i = 1; i < static_cast<int64_t>(config.quantiles.size()); ++i) {
      if (std::abs(config.quantiles[i] - 0.5) < std::abs(config.quantiles[center] - 0.5)) center = i;
    }
    const auto raw = output.predictions;
    std::vector<torch::Tensor> ordered(config.quantiles.size());
    ordered[center] = raw.select(-1, center);
    for (int64_t i = center - 1; i >= 0; --i)
      ordered[i] = ordered[i + 1] - torch::softplus(raw.select(-1, i));
    for (int64_t i = center + 1; i < static_cast<int64_t>(ordered.size()); ++i)
      ordered[i] = ordered[i - 1] + torch::softplus(raw.select(-1, i));
    output.predictions = torch::stack(ordered, -1);
  }
  return output;
}

torch::Tensor quantile_loss(const torch::Tensor& predictions,
                            const torch::Tensor& targets,
                            const std::vector<double>& quantiles) {
  validate_quantiles(quantiles);
  TORCH_CHECK(predictions.defined() && targets.defined(), "Predictions and targets must be defined");
  TORCH_CHECK(predictions.layout() == torch::kStrided && targets.layout() == torch::kStrided,
              "Predictions and targets must use dense strided layout");
  TORCH_CHECK(predictions.dim() == 3 && targets.dim() == 2,
              "Predictions must have shape [batch, horizon, quantiles]; targets [batch, horizon]");
  TORCH_CHECK(predictions.size(0) > 0 && predictions.size(1) > 0 &&
                  predictions.size(2) == static_cast<int64_t>(quantiles.size()),
              "Prediction dimensions or quantile count are invalid");
  TORCH_CHECK(targets.size(0) == predictions.size(0) && targets.size(1) == predictions.size(1),
              "Target batch and horizon must match predictions");
  TORCH_CHECK(supported_floating_type(predictions.scalar_type()) &&
                  targets.scalar_type() == predictions.scalar_type(),
              "Predictions and targets must have the same float32 or float64 dtype");
  TORCH_CHECK((predictions.device().is_cpu() || predictions.device().is_cuda()) &&
                  targets.device() == predictions.device(),
              "Predictions and targets must share a CPU or CUDA device");
  TORCH_CHECK(torch::isfinite(predictions).all().item<bool>() &&
                  torch::isfinite(targets).all().item<bool>(),
              "Predictions and targets must be finite; missing targets are unsupported");
  auto q = torch::tensor(quantiles, torch::TensorOptions().dtype(torch::kFloat64)).to(predictions.options());
  auto error = targets.unsqueeze(-1) - predictions;
  return torch::maximum(q * error, (q - 1.0) * error).sum(-1).mean();
}

}  // namespace praesidium_humanitatis
