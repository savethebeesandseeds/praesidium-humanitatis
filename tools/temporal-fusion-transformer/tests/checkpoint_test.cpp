#include <praesidium-humanitatis/checkpoint.h>

#include <torch/cuda.h>
#include <torch/torch.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

template <typename Function>
void require_error_containing(Function&& function, const std::string& diagnostic) {
  try {
    function();
  } catch (const std::exception& error) {
    require(std::string(error.what()).find(diagnostic) != std::string::npos,
            "expected diagnostic '" + diagnostic + "', received: " + error.what());
    return;
  }
  throw std::runtime_error("expected error: " + diagnostic);
}

void require_close(const torch::Tensor& actual, const torch::Tensor& expected,
                   const std::string& message, double tolerance = 0.0) {
  require(actual.defined() && expected.defined(), message + " (undefined)");
  require(actual.sizes() == expected.sizes(), message + " (shape)");
  require(actual.scalar_type() == expected.scalar_type(), message + " (dtype)");
  require(torch::allclose(actual.cpu(), expected.cpu(), tolerance, tolerance),
          message);
}

struct Fixture {
  std::filesystem::path directory;

  Fixture() {
    const auto timestamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      auto candidate = std::filesystem::temp_directory_path() /
                       ("praesidium-humanitatis-checkpoint-test-" + std::to_string(timestamp) +
                        "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate)) {
        directory = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error("could not create unique checkpoint fixture");
  }

  ~Fixture() {
    // Only remove the unique directory successfully created by this fixture.
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  std::string file(const std::string& name = "training.pt") const {
    return (directory / name).string();
  }
};

praesidium_humanitatis::Config make_config() {
  praesidium_humanitatis::Config config;
  config.hidden_size = 12;
  config.attention_heads = 3;
  config.dropout = 0.2;
  config.static_continuous = 2;
  config.past_continuous = 3;
  config.future_continuous = 2;
  config.static_categorical_cardinalities = {3, 5};
  config.past_categorical_cardinalities = {4, 2};
  config.future_categorical_cardinalities = {6};
  config.quantiles = {0.05, 0.25, 0.5, 0.8, 0.95};
  return config;
}

void require_config(const praesidium_humanitatis::Config& actual, const praesidium_humanitatis::Config& expected) {
  require(actual.hidden_size == expected.hidden_size, "hidden size persisted");
  require(actual.attention_heads == expected.attention_heads, "head count persisted");
  require(actual.dropout == expected.dropout, "dropout persisted");
  require(actual.static_continuous == expected.static_continuous,
          "static continuous count persisted");
  require(actual.past_continuous == expected.past_continuous,
          "past continuous count persisted");
  require(actual.future_continuous == expected.future_continuous,
          "future continuous count persisted");
  require(actual.static_categorical_cardinalities ==
              expected.static_categorical_cardinalities,
          "static vocabularies persisted");
  require(actual.past_categorical_cardinalities ==
              expected.past_categorical_cardinalities,
          "past vocabularies persisted");
  require(actual.future_categorical_cardinalities ==
              expected.future_categorical_cardinalities,
          "future vocabularies persisted");
  require(actual.quantiles == expected.quantiles, "quantiles persisted");
  require(actual.ordered_quantiles == expected.ordered_quantiles,
          "quantile output parameterization persisted");
}

torch::Tensor categories(const std::vector<int64_t>& cardinalities,
                         const std::vector<int64_t>& shape) {
  std::vector<torch::Tensor> columns;
  for (const auto cardinality : cardinalities) {
    columns.push_back(torch::randint(cardinality, shape, torch::kInt64));
  }
  return torch::stack(columns, -1);
}

praesidium_humanitatis::Batch make_batch(const praesidium_humanitatis::Config& config, const torch::Device& device,
                       torch::ScalarType dtype = torch::kFloat32) {
  praesidium_humanitatis::Batch batch;
  batch.statics.continuous = torch::randn({2, config.static_continuous}, dtype);
  batch.past.continuous = torch::randn({2, 3, config.past_continuous}, dtype);
  batch.future.continuous = torch::randn({2, 2, config.future_continuous}, dtype);
  batch.statics.categorical = categories(config.static_categorical_cardinalities, {2});
  batch.past.categorical = categories(config.past_categorical_cardinalities, {2, 3});
  batch.future.categorical = categories(config.future_categorical_cardinalities, {2, 2});
  for (auto* features : {&batch.statics, &batch.past, &batch.future}) {
    features->continuous = features->continuous.to(device);
    features->categorical = features->categorical.to(device);
  }
  return batch;
}

praesidium_humanitatis::Batch on_device(praesidium_humanitatis::Batch batch, const torch::Device& device) {
  for (auto* features : {&batch.statics, &batch.past, &batch.future}) {
    features->continuous = features->continuous.to(device);
    features->categorical = features->categorical.to(device);
  }
  return batch;
}

praesidium_humanitatis::TrainingSession make_session(const torch::Device& device,
                                   torch::ScalarType dtype = torch::kFloat32,
                                   bool ordered_quantiles = false) {
  praesidium_humanitatis::TrainingSession session;
  auto config = make_config();
  config.ordered_quantiles = ordered_quantiles;
  session.model = praesidium_humanitatis::TFT(config);
  session.model->to(device, dtype);
  session.optimizer = std::make_unique<torch::optim::Adam>(
      session.model->parameters(),
      torch::optim::AdamOptions(0.007).betas(std::make_tuple(0.7, 0.91))
          .eps(1e-7).weight_decay(0.003).amsgrad(true));
  session.progress.seed = 3017;
  session.progress.gradient_clip_norm = 0.37;
  session.progress.data_id = "checkpoint-test:v1:fixed-batch";
  return session;
}

void train_step(praesidium_humanitatis::TrainingSession& session, const praesidium_humanitatis::Batch& batch) {
  // The caller's absolute-step seed contract makes stochastic continuation
  // reproducible without pretending the archive contains application RNG state.
  torch::manual_seed(session.progress.seed + session.progress.completed_steps);
  session.model->train();
  session.optimizer->zero_grad();
  const auto targets = batch.future.continuous.select(-1, 0) * 0.4 +
                       batch.past.continuous.select(1, 2).select(-1, 0)
                           .unsqueeze(-1) * 0.6;
  const auto loss = praesidium_humanitatis::quantile_loss(session.model->forward(batch).predictions,
                                       targets, session.model->config.quantiles);
  require(torch::isfinite(loss).item<bool>(), "training loss must be finite");
  loss.backward();
  torch::nn::utils::clip_grad_norm_(session.model->parameters(),
                                   session.progress.gradient_clip_norm);
  session.optimizer->step();
  ++session.progress.completed_steps;
}

void require_options(const torch::optim::AdamOptions& actual,
                     const torch::optim::AdamOptions& expected) {
  require(actual.lr() == expected.lr(), "Adam learning rate persisted");
  require(actual.betas() == expected.betas(), "Adam betas persisted");
  require(actual.eps() == expected.eps(), "Adam epsilon persisted");
  require(actual.weight_decay() == expected.weight_decay(),
          "Adam weight decay persisted");
  require(actual.amsgrad() == expected.amsgrad(), "Adam AMSGrad setting persisted");
}

void require_optimizer(const praesidium_humanitatis::TrainingSession& actual,
                       const praesidium_humanitatis::TrainingSession& expected,
                       const torch::Device& device, double tolerance = 0.0) {
  const auto& actual_groups = actual.optimizer->param_groups();
  const auto& expected_groups = expected.optimizer->param_groups();
  require(actual_groups.size() == expected_groups.size(), "Adam group count persisted");
  require(actual.optimizer->state().size() == expected.optimizer->state().size(),
          "Adam state count persisted");
  require_options(static_cast<const torch::optim::AdamOptions&>(actual.optimizer->defaults()),
                  static_cast<const torch::optim::AdamOptions&>(expected.optimizer->defaults()));
  for (size_t group = 0; group < actual_groups.size(); ++group) {
    require_options(static_cast<const torch::optim::AdamOptions&>(
                        actual_groups[group].options()),
                    static_cast<const torch::optim::AdamOptions&>(
                        expected_groups[group].options()));
    const auto& actual_parameters = actual_groups[group].params();
    const auto& expected_parameters = expected_groups[group].params();
    require(actual_parameters.size() == expected_parameters.size(),
            "Adam group parameter count persisted");
    for (size_t index = 0; index < actual_parameters.size(); ++index) {
      const auto actual_state = actual.optimizer->state().find(
          actual_parameters[index].unsafeGetTensorImpl());
      const auto expected_state = expected.optimizer->state().find(
          expected_parameters[index].unsafeGetTensorImpl());
      require(actual_state != actual.optimizer->state().end(),
              "restored Adam state belongs to restored parameters");
      require(expected_state != expected.optimizer->state().end(),
              "fixture must have Adam state for each parameter");
      const auto& a = static_cast<const torch::optim::AdamParamState&>(*actual_state->second);
      const auto& e = static_cast<const torch::optim::AdamParamState&>(*expected_state->second);
      require(a.step() == e.step(), "Adam step persisted");
      require(a.exp_avg().device() == device, "Adam first moment on requested device");
      require(a.exp_avg_sq().device() == device, "Adam second moment on requested device");
      require_close(a.exp_avg(), e.exp_avg(), "Adam first moment persisted", tolerance);
      require_close(a.exp_avg_sq(), e.exp_avg_sq(), "Adam second moment persisted", tolerance);
      require(a.max_exp_avg_sq().defined() == e.max_exp_avg_sq().defined(),
              "Adam AMSGrad moment presence persisted");
      if (e.max_exp_avg_sq().defined()) {
        require(a.max_exp_avg_sq().device() == device, "Adam AMSGrad moment on requested device");
        require_close(a.max_exp_avg_sq(), e.max_exp_avg_sq(),
                      "Adam AMSGrad moment persisted", tolerance);
      }
    }
  }
}

void require_model(const praesidium_humanitatis::TFT& actual, const praesidium_humanitatis::TFT& expected,
                   double tolerance = 0.0) {
  const auto actual_parameters = actual->named_parameters();
  const auto expected_parameters = expected->named_parameters();
  require(actual_parameters.size() == expected_parameters.size(), "model parameter count persisted");
  for (const auto& parameter : expected_parameters) {
    require_close(actual_parameters[parameter.key()], parameter.value(),
                  "model parameter persisted: " + parameter.key(), tolerance);
  }
}

void test_roundtrip(const torch::Device& device, torch::ScalarType dtype,
                    bool ordered_quantiles = false) {
  Fixture fixture;
  auto session = make_session(device, dtype, ordered_quantiles);
  const auto batch = make_batch(session.model->config, device, dtype);
  train_step(session, batch);
  train_step(session, batch);
  session.model->eval();
  praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, session.progress);
  torch::serialize::InputArchive metadata;
  metadata.load_from(fixture.file(), torch::kCPU);
  c10::IValue version;
  metadata.read("format_version", version);
  require(version.isInt() && version.toInt() == 3, "new saves use checkpoint format version 3");
  auto restored = praesidium_humanitatis::load_checkpoint(fixture.file(), device);
  require_config(restored.model->config, session.model->config);
  require(!restored.model->is_training(), "evaluation mode persisted");
  require(restored.progress.completed_steps == 2, "completed steps persisted");
  require(restored.progress.seed == 3017, "training seed persisted");
  require(restored.progress.gradient_clip_norm == 0.37, "gradient clip norm persisted");
  require(restored.progress.data_id == session.progress.data_id, "data identity persisted");
  require_model(restored.model, session.model);
  require_optimizer(restored, session, device);
  torch::NoGradGuard no_grad;
  const auto expected = session.model->forward(batch);
  const auto actual = restored.model->forward(batch);
  const double tolerance = device.is_cuda() ? 1e-5 : 0.0;
  require_close(actual.predictions, expected.predictions, "forecast round trip", tolerance);
  require_close(actual.attention_weights, expected.attention_weights, "attention round trip", tolerance);
  require_close(actual.static_weights, expected.static_weights, "static selection round trip", tolerance);
  require_close(actual.past_weights, expected.past_weights, "past selection round trip", tolerance);
  require_close(actual.future_weights, expected.future_weights, "future selection round trip", tolerance);
}

void test_optimizer_groups(const torch::Device& device) {
  Fixture fixture;
  auto session = make_session(device);
  auto parameters = session.model->parameters();
  std::vector<torch::Tensor> first;
  std::vector<torch::Tensor> second;
  // Deliberately use reverse module order and unequal group options. A loader
  // that attaches Adam state by model order silently trains the wrong weights.
  for (size_t i = parameters.size(); i > 0; --i) {
    (i % 2 == 0 ? first : second).push_back(parameters[i - 1]);
  }
  std::vector<torch::optim::OptimizerParamGroup> groups;
  groups.emplace_back(first, std::make_unique<torch::optim::AdamOptions>(
      torch::optim::AdamOptions(0.002).betas(std::make_tuple(0.6, 0.93))
          .eps(1e-6).weight_decay(0.001).amsgrad(true)));
  groups.emplace_back(second, std::make_unique<torch::optim::AdamOptions>(
      torch::optim::AdamOptions(0.009).betas(std::make_tuple(0.8, 0.95))
          .eps(1e-8).weight_decay(0.004).amsgrad(false)));
  session.optimizer = std::make_unique<torch::optim::Adam>(
      groups, torch::optim::AdamOptions(0.007).amsgrad(true));
  const auto batch = make_batch(session.model->config, device);
  train_step(session, batch);
  praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, session.progress);
  auto restored = praesidium_humanitatis::load_checkpoint(fixture.file(), device);
  require_optimizer(restored, session, device);
  train_step(session, batch);
  train_step(restored, batch);
  const double tolerance = device.is_cuda() ? 2e-5 : 0.0;
  require_model(restored.model, session.model, tolerance);
  require_optimizer(restored, session, device, tolerance);
}

void test_continuation(const torch::Device& device, bool ordered_quantiles = false) {
  Fixture fixture;
  auto uninterrupted = make_session(device, torch::kFloat32, ordered_quantiles);
  const auto batch = make_batch(uninterrupted.model->config, device);
  train_step(uninterrupted, batch);
  train_step(uninterrupted, batch);
  praesidium_humanitatis::save_checkpoint(fixture.file(), uninterrupted.model,
                         *uninterrupted.optimizer, uninterrupted.progress);
  for (int step = 0; step < 2; ++step) {
    train_step(uninterrupted, batch);
  }
  // Consume RNG and allocate another model before loading; model initialization
  // must not accidentally be relied upon to reproduce the resumed dropout masks.
  auto distraction = make_session(device);
  (void)distraction;
  auto resumed = praesidium_humanitatis::load_checkpoint(fixture.file(), device);
  require_config(resumed.model->config, uninterrupted.model->config);
  require(resumed.model->is_training(), "training mode persisted");
  for (int step = 0; step < 2; ++step) {
    train_step(resumed, batch);
  }
  require(resumed.progress.completed_steps == uninterrupted.progress.completed_steps,
          "resumed progress matches uninterrupted training");
  const double tolerance = device.is_cuda() ? 2e-5 : 0.0;
  require_model(resumed.model, uninterrupted.model, tolerance);
  require_optimizer(resumed, uninterrupted, device, tolerance);
}

// Construct the complete historical v1 schema independently of the production
// writer. No checked-in binary or new-format writer is needed to test migration.
void save_legacy_fixture(const std::string& path,
                         const praesidium_humanitatis::TrainingSession& session) {
  using torch::serialize::OutputArchive;
  const auto& config = session.model->config;
  require(!config.ordered_quantiles, "v1 fixture must use the raw output head");
  const auto parameters = session.model->parameters();
  OutputArchive archive;
  archive.write("format_version", c10::IValue(int64_t{1}));
  archive.write("model_dtype", c10::IValue(parameters.front().scalar_type() == torch::kFloat32
                                             ? "float32" : "float64"));
  archive.write("model_training", c10::IValue(session.model->is_training()));
  archive.write("model_parameter_count", c10::IValue(static_cast<int64_t>(parameters.size())));
  archive.write("completed_steps", c10::IValue(session.progress.completed_steps));
  archive.write("seed", c10::IValue(session.progress.seed));
  archive.write("gradient_clip_norm", c10::IValue(session.progress.gradient_clip_norm));
  archive.write("data_id", c10::IValue(session.progress.data_id));
  OutputArchive configuration(archive.compilation_unit());
  configuration.write("hidden_size", c10::IValue(config.hidden_size));
  configuration.write("attention_heads", c10::IValue(config.attention_heads));
  configuration.write("dropout", c10::IValue(config.dropout));
  configuration.write("static_continuous", c10::IValue(config.static_continuous));
  configuration.write("past_continuous", c10::IValue(config.past_continuous));
  configuration.write("future_continuous", c10::IValue(config.future_continuous));
  configuration.write("static_categorical_cardinalities",
                      torch::tensor(config.static_categorical_cardinalities, torch::kInt64), true);
  configuration.write("past_categorical_cardinalities",
                      torch::tensor(config.past_categorical_cardinalities, torch::kInt64), true);
  configuration.write("future_categorical_cardinalities",
                      torch::tensor(config.future_categorical_cardinalities, torch::kInt64), true);
  configuration.write("quantiles", torch::tensor(config.quantiles, torch::kFloat64), true);
  archive.write("config", configuration);

  require(session.optimizer->param_groups().size() == 1,
          "legacy fixture uses one Adam group in model parameter order");
  OutputArchive adam(archive.compilation_unit());
  adam.write("type", c10::IValue("Adam"));
  OutputArchive defaults(archive.compilation_unit());
  session.optimizer->defaults().serialize(defaults);
  adam.write("defaults", defaults);
  adam.write("group_count", c10::IValue(int64_t{1}));
  OutputArchive group(archive.compilation_unit());
  OutputArchive options(archive.compilation_unit());
  session.optimizer->param_groups().front().options().serialize(options);
  group.write("options", options);
  group.write("parameter_indices", torch::arange(static_cast<int64_t>(parameters.size()),
                                                torch::kInt64), true);
  for (size_t index = 0; index < parameters.size(); ++index) {
    const auto state = session.optimizer->state().find(parameters[index].unsafeGetTensorImpl());
    if (state != session.optimizer->state().end()) {
      OutputArchive moment(archive.compilation_unit());
      state->second->serialize(moment);
      group.write("state_" + std::to_string(index), moment);
    }
  }
  adam.write("group_0", group);
  archive.write("optimizer", adam);
  OutputArchive model(archive.compilation_unit());
  session.model->save(model);
  archive.write("model", model);
  archive.save_to(path);
}

void test_legacy_compatibility(const torch::Device& device) {
  Fixture fixture;
  auto original = make_session(device);
  const auto batch = make_batch(original.model->config, device);
  {
    // Deliberately crossed forecasts make accidental reinterpretation or sorting
    // detectable even if the number and shape of parameters remain identical.
    torch::NoGradGuard no_grad;
    const auto parameters = original.model->named_parameters();
    parameters["network.projection.weight"].zero_();
    parameters["network.projection.bias"].copy_(
        torch::tensor({5.0, 3.0, 0.0, -3.0, -5.0}).to(device));
  }
  train_step(original, batch);
  train_step(original, batch);
  original.model->eval();
  save_legacy_fixture(fixture.file("legacy.pt"), original);
  auto restored = praesidium_humanitatis::load_checkpoint(fixture.file("legacy.pt"), device);
  require(!restored.model->config.ordered_quantiles, "legacy checkpoints retain the raw output head");
  require_config(restored.model->config, original.model->config);
  require_model(restored.model, original.model);
  require_optimizer(restored, original, device);
  {
    torch::NoGradGuard no_grad;
    const auto actual = restored.model->forward(batch).predictions;
    require_close(actual, original.model->forward(batch).predictions,
                  "legacy raw forecast equivalence", device.is_cuda() ? 1e-5 : 0.0);
    require((actual.select(2, 0) > actual.select(2, 4)).all().item<bool>(),
            "v1 crossed predictions must remain crossed");
  }
  praesidium_humanitatis::save_checkpoint(fixture.file("migrated.pt"), restored.model,
                                         *restored.optimizer, restored.progress);
  auto migrated = praesidium_humanitatis::load_checkpoint(fixture.file("migrated.pt"), device);
  require_config(migrated.model->config, original.model->config);
  train_step(original, batch);
  train_step(migrated, batch);
  const double tolerance = device.is_cuda() ? 2e-5 : 0.0;
  require_model(migrated.model, original.model, tolerance);
  require_optimizer(migrated, original, device, tolerance);
  require(migrated.progress.completed_steps == original.progress.completed_steps,
          "v1 to v2 migration preserves training progress");
}

std::string read_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "fixture checkpoint readable");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_validation_and_preservation(const torch::Device& device) {
  Fixture fixture;
  auto session = make_session(device);
  const auto batch = make_batch(session.model->config, device);
  train_step(session, batch);
  praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, session.progress);
  const auto original_bytes = read_bytes(fixture.file());
  const std::vector<std::function<void(praesidium_humanitatis::TrainingProgress&)>> invalid{
      [](auto& progress) { progress.completed_steps = -1; },
      [](auto& progress) { progress.seed = -1; },
      [](auto& progress) { progress.seed = std::numeric_limits<int64_t>::max(); },
      [](auto& progress) { progress.gradient_clip_norm = 0.0; },
      [](auto& progress) { progress.gradient_clip_norm = -1.0; },
      [](auto& progress) { progress.gradient_clip_norm = std::numeric_limits<double>::infinity(); },
      [](auto& progress) { progress.gradient_clip_norm = std::numeric_limits<double>::quiet_NaN(); },
  };
  for (const auto& mutate : invalid) {
    auto progress = session.progress;
    mutate(progress);
    require_throws([&] {
      praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, progress);
    }, "invalid training progress must be rejected");
    require(read_bytes(fixture.file()) == original_bytes,
            "failed overwrite must preserve the existing checkpoint byte-for-byte");
  }
  auto other_model = praesidium_humanitatis::TFT(session.model->config);
  other_model->to(device);
  torch::optim::Adam wrong_optimizer(other_model->parameters(), torch::optim::AdamOptions(0.01));
  require_throws([&] {
    praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, wrong_optimizer, session.progress);
  }, "optimizer for another model must be rejected");
  require(read_bytes(fixture.file()) == original_bytes,
          "optimizer rejection must preserve existing checkpoint");
  auto restored = praesidium_humanitatis::load_checkpoint(fixture.file(), device);
  require_model(restored.model, session.model);
  // A successful overwrite must replace both progress and model state.
  train_step(session, batch);
  praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, session.progress);
  auto overwritten = praesidium_humanitatis::load_checkpoint(fixture.file(), device);
  require(overwritten.progress.completed_steps == 2, "successful overwrite updates progress");
  require_model(overwritten.model, session.model);
}

void test_invalid_archives(const torch::Device& device) {
  Fixture fixture;
  auto session = make_session(device);
  session.progress.data_id.clear();
  praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, session.progress);
  auto untrained = praesidium_humanitatis::load_checkpoint(fixture.file(), device);
  require(untrained.progress.completed_steps == 0, "untrained checkpoint has zero steps");
  require(untrained.progress.data_id.empty(), "empty data identity round trip");
  require(untrained.optimizer->state().empty(), "untrained Adam has no moments");
  require_model(untrained.model, session.model);
  auto bytes = read_bytes(fixture.file());
  {
    std::ofstream truncated(fixture.file("truncated.pt"), std::ios::binary);
    truncated.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
  }
  require_throws([&] { praesidium_humanitatis::load_checkpoint(fixture.file("truncated.pt"), device); },
                 "truncated checkpoint must fail");
  require_throws([&] { praesidium_humanitatis::load_checkpoint(fixture.file("missing.pt"), device); },
                 "missing checkpoint must fail");
  torch::serialize::OutputArchive future_version;
  future_version.write("format_version", c10::IValue(int64_t{999}));
  future_version.save_to(fixture.file("future.pt"));
  require_error_containing([&] { praesidium_humanitatis::load_checkpoint(fixture.file("future.pt"), device); },
                           "Unsupported checkpoint format_version");
  torch::serialize::OutputArchive wrong_type;
  wrong_type.write("format_version", c10::IValue("1"));
  wrong_type.save_to(fixture.file("wrong-type.pt"));
  require_error_containing([&] { praesidium_humanitatis::load_checkpoint(fixture.file("wrong-type.pt"), device); },
                           "'format_version' must be an integer");

  // Malformed metadata must be rejected before model/optimizer construction.
  // Match the precise diagnostic so missing later archives cannot make a
  // malformed prefix fixture pass for an unrelated reason.
  for (const std::string malformed : {"static_categorical_cardinalities", "quantiles",
                                      "missing_ordered_quantiles", "integer_ordered_quantiles",
                                      "string_ordered_quantiles", "double_ordered_quantiles"}) {
    torch::serialize::InputArchive source;
    source.load_from(fixture.file(), torch::kCPU);
    torch::serialize::OutputArchive corrupt;
    for (const std::string key : {"format_version", "model_dtype", "model_training",
                                 "model_parameter_count", "completed_steps", "seed",
                                 "gradient_clip_norm", "data_id"}) {
      c10::IValue value;
      source.read(key, value);
      corrupt.write(key, value);
    }
    torch::serialize::InputArchive source_config;
    source.read("config", source_config);
    torch::serialize::OutputArchive corrupt_config(corrupt.compilation_unit());
    for (const std::string key : {"hidden_size", "attention_heads", "dropout",
                                 "static_continuous", "past_continuous", "future_continuous",
                                 "ordered_quantiles"}) {
      if (key == "ordered_quantiles" && malformed == "missing_ordered_quantiles") continue;
      c10::IValue value;
      source_config.read(key, value);
      if (key == "ordered_quantiles") {
        if (malformed == "integer_ordered_quantiles") value = c10::IValue(int64_t{1});
        if (malformed == "string_ordered_quantiles") value = c10::IValue("false");
        if (malformed == "double_ordered_quantiles") value = c10::IValue(0.0);
      }
      corrupt_config.write(key, value);
    }
    for (const std::string key : {"static_categorical_cardinalities",
                                 "past_categorical_cardinalities",
                                 "future_categorical_cardinalities", "quantiles"}) {
      torch::Tensor value;
      source_config.read(key, value, true);
      if (key == malformed) {
        value = key == "quantiles" ? value.to(torch::kFloat32) : value.unsqueeze(0);
      }
      corrupt_config.write(key, value, true);
    }
    corrupt.write("config", corrupt_config);
    corrupt.save_to(fixture.file("bad-config.pt"));
    const auto diagnostic = malformed == "missing_ordered_quantiles"
        ? "'ordered_quantiles' is required"
        : malformed.find("_ordered_quantiles") != std::string::npos
            ? "'ordered_quantiles' must be a boolean"
            : "'" + malformed + "' has an invalid vector type or shape";
    require_error_containing([&] { praesidium_humanitatis::load_checkpoint(fixture.file("bad-config.pt"), device); },
                             diagnostic);
  }
}

void test_cross_device() {
  Fixture fixture;
  const torch::Device cpu(torch::kCPU);
  const torch::Device cuda(torch::kCUDA, 0);
  for (const auto& source : {cpu, cuda}) {
    const auto destination = source.is_cuda() ? cpu : cuda;
    auto session = make_session(source);
    const auto source_batch = make_batch(session.model->config, source);
    train_step(session, source_batch);
    session.model->eval();
    praesidium_humanitatis::save_checkpoint(fixture.file(), session.model, *session.optimizer, session.progress);
    auto restored = praesidium_humanitatis::load_checkpoint(fixture.file(), destination);
    require(restored.model->parameters().front().device() == destination,
            "model loaded onto requested device");
    require_optimizer(restored, session, destination);
    const auto destination_batch = on_device(source_batch, destination);
    {
      torch::NoGradGuard no_grad;
      require_close(restored.model->forward(destination_batch).predictions,
                    session.model->forward(source_batch).predictions,
                    "cross-device forecast equivalence", 2e-4);
    }
    train_step(restored, destination_batch);
    require(restored.progress.completed_steps == 2,
            "restored optimizer can train on destination device");
  }
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
      std::cerr << "Usage: checkpoint_test [--device cpu|cuda]\n";
      return 2;
    }
  } else if (argc != 1) {
    std::cerr << "Usage: checkpoint_test [--device cpu|cuda]\n";
    return 2;
  }
  torch::set_num_threads(1);
  std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"architecture, predictions, diagnostics and Adam round trip",
       [&] { test_roundtrip(device, torch::kFloat32); }},
      {"double precision checkpoint", [&] { test_roundtrip(device, torch::kFloat64); }},
      {"dropout training continuation", [&] { test_continuation(device); }},
      {"ordered quantile checkpoint", [&] { test_roundtrip(device, torch::kFloat32, true); }},
      {"ordered double precision checkpoint", [&] { test_roundtrip(device, torch::kFloat64, true); }},
      {"ordered dropout training continuation", [&] { test_continuation(device, true); }},
      {"legacy v1 raw-head compatibility and migration", [&] { test_legacy_compatibility(device); }},
      {"multiple reordered Adam parameter groups", [&] { test_optimizer_groups(device); }},
      {"validation and overwrite preservation", [&] { test_validation_and_preservation(device); }},
      {"truncated and invalid archives", [&] { test_invalid_archives(device); }},
  };
  if (device.is_cuda()) {
    tests.emplace_back("CPU/CUDA checkpoint portability", test_cross_device);
  }
  int failures = 0;
  for (const auto& test : tests) {
    torch::manual_seed(20260916);
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
