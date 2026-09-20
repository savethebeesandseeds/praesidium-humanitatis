#include "praesidium-humanitatis/checkpoint.h"

#include <torch/serialize.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace praesidium_humanitatis {
namespace {

using InputArchive = torch::serialize::InputArchive;
using OutputArchive = torch::serialize::OutputArchive;
namespace fs = std::filesystem;
constexpr int64_t kFormatVersion = 3;

int64_t read_int(InputArchive& archive, const std::string& name) {
  c10::IValue value;
  archive.read(name, value);
  TORCH_CHECK(value.isInt(), "Checkpoint field '", name, "' must be an integer");
  return value.toInt();
}

double read_double(InputArchive& archive, const std::string& name) {
  c10::IValue value;
  archive.read(name, value);
  TORCH_CHECK(value.isDouble(), "Checkpoint field '", name, "' must be a double");
  return value.toDouble();
}

std::string read_string(InputArchive& archive, const std::string& name) {
  c10::IValue value;
  archive.read(name, value);
  TORCH_CHECK(value.isString(), "Checkpoint field '", name, "' must be a string");
  return value.toStringRef();
}

bool read_bool(InputArchive& archive, const std::string& name) {
  c10::IValue value;
  TORCH_CHECK(archive.try_read(name, value), "Checkpoint field '", name, "' is required");
  TORCH_CHECK(value.isBool(), "Checkpoint field '", name, "' must be a boolean");
  return value.toBool();
}

template <typename T>
std::vector<T> read_vector(InputArchive& archive, const std::string& name,
                           torch::ScalarType dtype) {
  torch::Tensor value;
  archive.read(name, value, true);
  TORCH_CHECK(value.defined() && value.dim() == 1 && value.scalar_type() == dtype,
              "Checkpoint field '", name, "' has an invalid vector type or shape");
  value = value.cpu().contiguous();
  std::vector<T> result(static_cast<size_t>(value.numel()));
  if (!result.empty()) {
    std::copy_n(value.data_ptr<T>(), result.size(), result.begin());
  }
  return result;
}

void validate_progress(const TrainingProgress& progress) {
  TORCH_CHECK(progress.completed_steps >= 0, "completed_steps must be nonnegative");
  TORCH_CHECK(progress.seed >= 0, "seed must be nonnegative");
  TORCH_CHECK(progress.seed <= std::numeric_limits<int64_t>::max() -
                                   progress.completed_steps,
              "seed + completed_steps overflows int64");
  TORCH_CHECK(std::isfinite(progress.gradient_clip_norm) &&
                  progress.gradient_clip_norm > 0.0,
              "gradient_clip_norm must be finite and positive");
}

void write_config(OutputArchive& archive, const Config& config) {
  archive.write("hidden_size", c10::IValue(config.hidden_size));
  archive.write("attention_heads", c10::IValue(config.attention_heads));
  archive.write("dropout", c10::IValue(config.dropout));
  archive.write("static_continuous", c10::IValue(config.static_continuous));
  archive.write("past_continuous", c10::IValue(config.past_continuous));
  archive.write("future_continuous", c10::IValue(config.future_continuous));
  archive.write("static_categorical_cardinalities",
                torch::tensor(config.static_categorical_cardinalities, torch::kInt64), true);
  archive.write("past_categorical_cardinalities",
                torch::tensor(config.past_categorical_cardinalities, torch::kInt64), true);
  archive.write("future_categorical_cardinalities",
                torch::tensor(config.future_categorical_cardinalities, torch::kInt64), true);
  archive.write("quantiles", torch::tensor(config.quantiles, torch::kFloat64), true);
  archive.write("ordered_quantiles", c10::IValue(config.ordered_quantiles));
  archive.write("layer_norm_epsilon", c10::IValue(config.layer_norm_epsilon));
  archive.write("future_continuous_past_indices",
                torch::tensor(config.future_continuous_past_indices, torch::kInt64), true);
  archive.write("future_categorical_past_indices",
                torch::tensor(config.future_categorical_past_indices, torch::kInt64), true);
}

Config read_config(InputArchive& archive, int64_t format_version) {
  Config config;
  config.hidden_size = read_int(archive, "hidden_size");
  config.attention_heads = read_int(archive, "attention_heads");
  config.dropout = read_double(archive, "dropout");
  config.static_continuous = read_int(archive, "static_continuous");
  config.past_continuous = read_int(archive, "past_continuous");
  config.future_continuous = read_int(archive, "future_continuous");
  config.static_categorical_cardinalities = read_vector<int64_t>(
      archive, "static_categorical_cardinalities", torch::kInt64);
  config.past_categorical_cardinalities = read_vector<int64_t>(
      archive, "past_categorical_cardinalities", torch::kInt64);
  config.future_categorical_cardinalities = read_vector<int64_t>(
      archive, "future_categorical_cardinalities", torch::kInt64);
  config.quantiles = read_vector<double>(archive, "quantiles", torch::kFloat64);
  // Version 1 used the raw projection output. Never reinterpret its learned
  // weights through a new head, even if Config defaults change in the future.
  config.ordered_quantiles = format_version == 1 ? false : read_bool(archive, "ordered_quantiles");
  // Earlier formats used independent groups and LibTorch's default epsilon.
  // Preserve those semantics explicitly rather than relying on future defaults.
  config.layer_norm_epsilon = format_version < 3 ? 1e-5 : read_double(archive, "layer_norm_epsilon");
  if (format_version >= 3) {
    config.future_continuous_past_indices = read_vector<int64_t>(
        archive, "future_continuous_past_indices", torch::kInt64);
    config.future_categorical_past_indices = read_vector<int64_t>(
        archive, "future_categorical_past_indices", torch::kInt64);
  } else {
    config.future_continuous_past_indices.clear();
    config.future_categorical_past_indices.clear();
  }
  config.validate();
  return config;
}

const torch::optim::AdamOptions& adam_options(const torch::optim::OptimizerOptions& options) {
  auto* adam = dynamic_cast<const torch::optim::AdamOptions*>(&options);
  TORCH_CHECK(adam != nullptr, "Checkpoint optimizer contains non-Adam options");
  const auto beta1 = std::get<0>(adam->betas());
  const auto beta2 = std::get<1>(adam->betas());
  TORCH_CHECK(std::isfinite(adam->lr()) && adam->lr() >= 0.0 &&
                  std::isfinite(adam->eps()) && adam->eps() >= 0.0 &&
                  std::isfinite(adam->weight_decay()) && adam->weight_decay() >= 0.0 &&
                  std::isfinite(beta1) && beta1 >= 0.0 && beta1 < 1.0 &&
                  std::isfinite(beta2) && beta2 >= 0.0 && beta2 < 1.0,
              "Checkpoint contains invalid Adam options");
  return *adam;
}

void validate_moment(const torch::Tensor& tensor, const torch::Tensor& parameter,
                     const char* name) {
  TORCH_CHECK(tensor.defined() && tensor.layout() == torch::kStrided &&
                  tensor.sizes() == parameter.sizes() &&
                  tensor.scalar_type() == parameter.scalar_type() &&
                  tensor.device() == parameter.device(),
              "Checkpoint Adam ", name, " does not match its model parameter");
  TORCH_CHECK(torch::isfinite(tensor).all().item<bool>(),
              "Checkpoint Adam ", name, " contains non-finite values");
}

void validate_state(const torch::optim::AdamParamState& state,
                    const torch::Tensor& parameter, bool amsgrad) {
  TORCH_CHECK(state.step() >= 0, "Checkpoint Adam step must be nonnegative");
  validate_moment(state.exp_avg(), parameter, "exp_avg");
  validate_moment(state.exp_avg_sq(), parameter, "exp_avg_sq");
  TORCH_CHECK((state.exp_avg_sq() >= 0).all().item<bool>(),
              "Checkpoint Adam exp_avg_sq must be nonnegative");
  if (amsgrad || state.max_exp_avg_sq().defined()) {
    validate_moment(state.max_exp_avg_sq(), parameter, "max_exp_avg_sq");
    TORCH_CHECK((state.max_exp_avg_sq() >= 0).all().item<bool>(),
                "Checkpoint Adam max_exp_avg_sq must be nonnegative");
  }
}

torch::ScalarType validate_model(const TFT& model) {
  TORCH_CHECK(model, "Cannot checkpoint a null model");
  model->config.validate();
  const auto parameters = model->parameters();
  TORCH_CHECK(!parameters.empty(), "Cannot checkpoint a model without parameters");
  const auto dtype = parameters.front().scalar_type();
  const auto device = parameters.front().device();
  TORCH_CHECK(dtype == torch::kFloat32 || dtype == torch::kFloat64,
              "Checkpoint supports only float32 or float64 models");
  TORCH_CHECK(device.is_cpu() || device.is_cuda(), "Checkpoint supports only CPU/CUDA models");
  for (const auto& parameter : parameters) {
    TORCH_CHECK(parameter.scalar_type() == dtype && parameter.device() == device,
                "Checkpoint model parameters must share dtype and device");
    TORCH_CHECK(torch::isfinite(parameter).all().item<bool>(),
                "Checkpoint model contains non-finite parameters");
  }
  for (const auto& buffer : model->buffers()) {
    TORCH_CHECK(buffer.device() == device &&
                    (!buffer.is_floating_point() || buffer.scalar_type() == dtype),
                "Checkpoint model buffers do not match model dtype/device");
    TORCH_CHECK(torch::isfinite(buffer).all().item<bool>(),
                "Checkpoint model contains non-finite buffers");
  }
  return dtype;
}

// Atomically reserve a private sibling directory before creating a temporary
// file. Unlike check-then-open filenames, reservation cannot clobber another
// writer's temporary file. The final rename stays on the same filesystem.
class PendingFile {
 public:
  explicit PendingFile(const fs::path& destination) {
    static std::atomic<uint64_t> sequence{0};
    const auto parent = destination.parent_path().empty() ? fs::path(".") : destination.parent_path();
    TORCH_CHECK(fs::is_directory(parent), "Checkpoint parent directory does not exist: ", parent.string());
    TORCH_CHECK(!destination.filename().empty(), "Checkpoint path must name a file");
    TORCH_CHECK(!fs::exists(destination) || fs::is_regular_file(destination),
                "Checkpoint destination must be a regular file: ", destination.string());
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
      const auto suffix = std::to_string(clock) + "." + std::to_string(sequence.fetch_add(1));
      directory_ = parent / (destination.filename().string() + ".tmp." + suffix);
      std::error_code error;
      if (fs::create_directory(directory_, error)) {
        file_ = directory_ / "checkpoint.pt";
        return;
      }
      TORCH_CHECK(!error || error == std::errc::file_exists,
                  "Cannot create checkpoint temporary directory: ", error.message());
    }
    TORCH_CHECK(false, "Cannot reserve a checkpoint temporary file");
  }

  ~PendingFile() {
    std::error_code ignored;
    fs::remove(file_, ignored);
    fs::remove(directory_, ignored);
  }

  const fs::path& path() const { return file_; }

 private:
  fs::path directory_;
  fs::path file_;
};

struct SavedGroup {
  std::vector<int64_t> indices;
  torch::optim::AdamOptions options;
};

}  // namespace

void save_checkpoint(const std::string& path, const TFT& model,
                     const torch::optim::Adam& optimizer,
                     const TrainingProgress& progress) {
  torch::NoGradGuard no_grad;
  TORCH_CHECK(!path.empty(), "Checkpoint path must not be empty");
  validate_progress(progress);
  const auto dtype = validate_model(model);
  const auto parameters = model->parameters();
  std::unordered_map<void*, int64_t> parameter_indices;
  for (size_t i = 0; i < parameters.size(); ++i) {
    parameter_indices.emplace(parameters[i].unsafeGetTensorImpl(), static_cast<int64_t>(i));
  }
  TORCH_CHECK(!optimizer.param_groups().empty(), "Checkpoint Adam has no parameter groups");
  const auto& defaults = adam_options(optimizer.defaults());
  std::unordered_set<void*> seen;

  OutputArchive archive;
  archive.write("format_version", c10::IValue(kFormatVersion));
  archive.write("model_dtype", c10::IValue(dtype == torch::kFloat32 ? "float32" : "float64"));
  archive.write("model_training", c10::IValue(model->is_training()));
  archive.write("model_parameter_count", c10::IValue(static_cast<int64_t>(parameters.size())));
  archive.write("completed_steps", c10::IValue(progress.completed_steps));
  archive.write("seed", c10::IValue(progress.seed));
  archive.write("gradient_clip_norm", c10::IValue(progress.gradient_clip_norm));
  archive.write("data_id", c10::IValue(progress.data_id));
  OutputArchive configuration(archive.compilation_unit());
  write_config(configuration, model->config);
  archive.write("config", configuration);

  OutputArchive adam(archive.compilation_unit());
  adam.write("type", c10::IValue("Adam"));
  OutputArchive default_options(archive.compilation_unit());
  defaults.serialize(default_options);
  adam.write("defaults", default_options);
  adam.write("group_count", c10::IValue(static_cast<int64_t>(optimizer.param_groups().size())));
  for (size_t group_index = 0; group_index < optimizer.param_groups().size(); ++group_index) {
    const auto& group = optimizer.param_groups()[group_index];
    const auto& options = adam_options(group.options());
    TORCH_CHECK(!group.params().empty(), "Checkpoint Adam parameter groups must not be empty");
    OutputArchive group_archive(archive.compilation_unit());
    OutputArchive option_archive(archive.compilation_unit());
    options.serialize(option_archive);
    group_archive.write("options", option_archive);
    std::vector<int64_t> indices;
    for (const auto& parameter : group.params()) {
      const auto key = parameter.unsafeGetTensorImpl();
      const auto found = parameter_indices.find(key);
      TORCH_CHECK(found != parameter_indices.end(),
                  "Checkpoint optimizer references a parameter outside this model");
      TORCH_CHECK(seen.insert(key).second, "Checkpoint optimizer contains duplicate parameters");
      indices.push_back(found->second);
      auto state_entry = optimizer.state().find(key);
      if (state_entry != optimizer.state().end()) {
        const auto* state = dynamic_cast<const torch::optim::AdamParamState*>(state_entry->second.get());
        TORCH_CHECK(state != nullptr, "Checkpoint optimizer contains non-Adam state");
        validate_state(*state, parameter, options.amsgrad());
        OutputArchive state_archive(archive.compilation_unit());
        state->serialize(state_archive);
        group_archive.write("state_" + std::to_string(found->second), state_archive);
      }
    }
    group_archive.write("parameter_indices", torch::tensor(indices, torch::kInt64), true);
    adam.write("group_" + std::to_string(group_index), group_archive);
  }
  TORCH_CHECK(seen.size() == parameters.size(),
              "Checkpoint optimizer must reference every model parameter exactly once");
  for (const auto& state : optimizer.state()) {
    TORCH_CHECK(seen.count(state.first) == 1, "Checkpoint Adam contains orphaned parameter state");
  }
  archive.write("optimizer", adam);
  OutputArchive model_archive(archive.compilation_unit());
  model->save(model_archive);
  archive.write("model", model_archive);

  const fs::path destination(path);
  PendingFile pending(destination);
  {
    std::ofstream output(pending.path(), std::ios::binary | std::ios::trunc);
    TORCH_CHECK(output.is_open(), "Cannot open checkpoint temporary file");
    archive.save_to(output);
    output.flush();
    TORCH_CHECK(output.good(), "Cannot flush checkpoint temporary file");
    output.close();
    TORCH_CHECK(!output.fail(), "Cannot close checkpoint temporary file");
  }
  // Never remove the destination first. On platforms that cannot replace an
  // existing file atomically, rename fails and the previous file survives.
  fs::rename(pending.path(), destination);
}

TrainingSession load_checkpoint(const std::string& path, torch::Device device) {
  torch::NoGradGuard no_grad;
  TORCH_CHECK(device.is_cpu() || device.is_cuda(), "Checkpoint destination must be CPU or CUDA");
  TORCH_CHECK(!path.empty() && fs::is_regular_file(fs::path(path)),
              "Checkpoint path must name an existing regular file");
  InputArchive archive;
  // Validate on CPU, including when reading a checkpoint saved from CUDA.
  archive.load_from(path, torch::kCPU);
  const auto format_version = read_int(archive, "format_version");
  TORCH_CHECK(format_version >= 1 && format_version <= kFormatVersion,
              "Unsupported checkpoint format_version; expected 1, 2 or ", kFormatVersion);
  const auto dtype_name = read_string(archive, "model_dtype");
  TORCH_CHECK(dtype_name == "float32" || dtype_name == "float64",
              "Unsupported checkpoint model_dtype");
  const auto dtype = dtype_name == "float32" ? torch::kFloat32 : torch::kFloat64;
  const auto training = read_bool(archive, "model_training");
  const auto parameter_count = read_int(archive, "model_parameter_count");
  TORCH_CHECK(parameter_count > 0, "Checkpoint model_parameter_count must be positive");
  TrainingSession session;
  session.progress.completed_steps = read_int(archive, "completed_steps");
  session.progress.seed = read_int(archive, "seed");
  session.progress.gradient_clip_norm = read_double(archive, "gradient_clip_norm");
  session.progress.data_id = read_string(archive, "data_id");
  validate_progress(session.progress);
  InputArchive configuration;
  archive.read("config", configuration);
  const auto config = read_config(configuration, format_version);

  InputArchive adam;
  archive.read("optimizer", adam);
  TORCH_CHECK(read_string(adam, "type") == "Adam", "Checkpoint optimizer must be Adam");
  InputArchive defaults_archive;
  adam.read("defaults", defaults_archive);
  torch::optim::AdamOptions defaults;
  defaults.serialize(defaults_archive);
  adam_options(defaults);
  const auto group_count = read_int(adam, "group_count");
  TORCH_CHECK(group_count > 0 && group_count <= parameter_count,
              "Checkpoint Adam group_count is invalid");
  std::vector<SavedGroup> saved_groups;
  std::vector<InputArchive> group_archives;
  std::unordered_set<int64_t> seen;
  for (int64_t i = 0; i < group_count; ++i) {
    InputArchive group_archive;
    adam.read("group_" + std::to_string(i), group_archive);
    SavedGroup group;
    group.indices = read_vector<int64_t>(group_archive, "parameter_indices", torch::kInt64);
    TORCH_CHECK(!group.indices.empty(), "Checkpoint Adam parameter group is empty");
    for (const auto index : group.indices) {
      TORCH_CHECK(index >= 0 && index < parameter_count && seen.insert(index).second,
                  "Checkpoint Adam parameter indices are invalid or duplicated");
    }
    InputArchive options;
    group_archive.read("options", options);
    group.options.serialize(options);
    adam_options(group.options);
    // A state may be absent before a parameter receives its first gradient.
    // Reject unexpected state keys so corrupt/orphaned entries cannot disappear.
    std::unordered_set<std::string> allowed{"options", "parameter_indices"};
    for (const auto index : group.indices) {
      allowed.insert("state_" + std::to_string(index));
    }
    for (const auto& key : group_archive.keys()) {
      TORCH_CHECK(allowed.count(key) != 0, "Checkpoint Adam has an unexpected state field: ", key);
    }
    saved_groups.push_back(std::move(group));
    group_archives.push_back(std::move(group_archive));
  }
  TORCH_CHECK(seen.size() == static_cast<size_t>(parameter_count),
              "Checkpoint Adam does not cover all model parameters");

  // Configuration, dtype, progress, and optimizer topology/options have all
  // been checked before allocating the network.
  session.model = TFT(config);
  session.model->to(dtype);
  const auto expected_parameters = session.model->parameters();
  const auto expected_buffers = session.model->buffers();
  TORCH_CHECK(expected_parameters.size() == static_cast<size_t>(parameter_count),
              "Checkpoint parameter count does not match its model configuration");
  std::vector<std::vector<int64_t>> parameter_shapes, buffer_shapes;
  for (const auto& parameter : expected_parameters) {
    parameter_shapes.push_back(parameter.sizes().vec());
  }
  for (const auto& buffer : expected_buffers) {
    buffer_shapes.push_back(buffer.sizes().vec());
  }
  InputArchive model_archive;
  archive.read("model", model_archive);
  session.model->load(model_archive);
  auto parameters = session.model->parameters();
  const auto buffers = session.model->buffers();
  TORCH_CHECK(parameters.size() == parameter_shapes.size() && buffers.size() == buffer_shapes.size(),
              "Checkpoint model tensor counts do not match the configuration");
  for (size_t i = 0; i < parameters.size(); ++i) {
    TORCH_CHECK(parameters[i].sizes().vec() == parameter_shapes[i] && parameters[i].scalar_type() == dtype,
                "Checkpoint parameter shape or dtype does not match the configuration");
  }
  for (size_t i = 0; i < buffers.size(); ++i) {
    TORCH_CHECK(buffers[i].sizes().vec() == buffer_shapes[i],
                "Checkpoint buffer shape does not match the configuration");
  }
  validate_model(session.model);
  session.model->to(device);
  session.model->train(training);
  parameters = session.model->parameters();

  std::vector<torch::optim::OptimizerParamGroup> groups;
  for (const auto& saved : saved_groups) {
    std::vector<torch::Tensor> group_parameters;
    for (const auto index : saved.indices) {
      group_parameters.push_back(parameters[static_cast<size_t>(index)]);
    }
    groups.emplace_back(std::move(group_parameters),
                        std::make_unique<torch::optim::AdamOptions>(saved.options));
  }
  session.optimizer = std::make_unique<torch::optim::Adam>(groups, defaults);
  for (size_t i = 0; i < saved_groups.size(); ++i) {
    for (const auto index : saved_groups[i].indices) {
      InputArchive state_archive;
      if (!group_archives[i].try_read("state_" + std::to_string(index), state_archive)) {
        continue;
      }
      auto state = std::make_unique<torch::optim::AdamParamState>();
      state->serialize(state_archive);
      const auto& parameter = parameters[static_cast<size_t>(index)];
      if (state->exp_avg().defined()) state->exp_avg(state->exp_avg().to(device));
      if (state->exp_avg_sq().defined()) state->exp_avg_sq(state->exp_avg_sq().to(device));
      if (state->max_exp_avg_sq().defined()) {
        state->max_exp_avg_sq(state->max_exp_avg_sq().to(device));
      }
      validate_state(*state, parameter, saved_groups[i].options.amsgrad());
      session.optimizer->state()[parameter.unsafeGetTensorImpl()] = std::move(state);
    }
  }
  return session;
}

}  // namespace praesidium_humanitatis
