#include "praesidium-humanitatis/checkpoint.h"
#include "praesidium-humanitatis/evaluation.h"

#include <torch/cuda.h>
#include <torch/version.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace {
constexpr const char* data_id = "praesidium-humanitatis.synthetic.sine.v1";
// The project rename did not change the generator or existing run identities.
constexpr const char* legacy_data_id = "adht.synthetic.sine.v1";
constexpr int64_t history = 16, horizon = 4, batch_size = 32, validation_size = 128;

struct Options {
  std::string device = "cpu", checkpoint, resume, report;
  int64_t steps = 80, seed = 20260916, save_every = 20;
  bool evaluate_only = false, help = false, ordered_quantiles = false;
};

int64_t integer(const std::string& value, int64_t minimum, int64_t maximum) {
  size_t consumed = 0;
  const auto parsed = std::stoll(value, &consumed);
  if (consumed != value.size() || parsed < minimum || parsed > maximum)
    throw std::invalid_argument("Integer argument is outside its supported range.");
  return parsed;
}

bool same_path(const std::string& left, const std::string& right) {
  if (left.empty() || right.empty()) return false;
  const auto a = std::filesystem::weakly_canonical(std::filesystem::absolute(left));
  const auto b = std::filesystem::weakly_canonical(std::filesystem::absolute(right));
  return a == b || (std::filesystem::exists(a) && std::filesystem::exists(b) &&
                    std::filesystem::equivalent(a, b));
}

Options parse(int argc, char** argv) {
  Options options;
  std::set<std::string> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!seen.insert(arg).second) throw std::invalid_argument("Duplicate option: " + arg);
    if (arg == "--help") { options.help = true; continue; }
    if (arg == "--evaluate-only") { options.evaluate_only = true; continue; }
    if (arg == "--ordered-quantiles") { options.ordered_quantiles = true; continue; }
    if (arg != "--device" && arg != "--steps" && arg != "--seed" &&
        arg != "--checkpoint" && arg != "--resume" && arg != "--report" &&
        arg != "--save-every") throw std::invalid_argument("Unknown option: " + arg);
    if (i + 1 == argc) throw std::invalid_argument("Missing value for " + arg);
    const std::string value = argv[++i];
    if (value.empty()) throw std::invalid_argument("Empty value for " + arg);
    if (arg == "--device") options.device = value;
    if (arg == "--steps") options.steps = integer(value, 1, 100000);
    if (arg == "--seed") options.seed = integer(value, 0, std::numeric_limits<int64_t>::max());
    if (arg == "--save-every") options.save_every = integer(value, 0, 100000);
    if (arg == "--checkpoint") options.checkpoint = value;
    if (arg == "--resume") options.resume = value;
    if (arg == "--report") options.report = value;
  }
  if (options.help) return options;
  if (options.device != "cpu" && options.device != "cuda")
    throw std::invalid_argument("--device must be cpu or cuda.");
  if (!options.resume.empty() && seen.count("--seed"))
    throw std::invalid_argument("Resume restores its seed; --seed cannot override it.");
  if (!options.resume.empty() && options.ordered_quantiles)
    throw std::invalid_argument("Resume restores its output head; --ordered-quantiles cannot override it.");
  if (options.evaluate_only && (options.resume.empty() || seen.count("--steps")))
    throw std::invalid_argument("--evaluate-only requires --resume and does not accept --steps.");
  if (seen.count("--save-every") && (options.checkpoint.empty() || options.evaluate_only))
    throw std::invalid_argument("--save-every requires training with --checkpoint.");
  if (same_path(options.report, options.checkpoint) || same_path(options.report, options.resume))
    throw std::invalid_argument("A report must have a different path from a checkpoint.");
  return options;
}

struct Example { praesidium_humanitatis::Batch input; torch::Tensor target; };

// Known calendar-like covariates, static amplitude, and noisy target observations.
Example generate(int64_t count, const torch::TensorOptions& options) {
  auto amplitude = 0.5 + torch::rand({count, 1}, options);
  auto phase = 6.283185307179586 * torch::rand({count, 1}, options);
  auto time = torch::arange(history + horizon, options).unsqueeze(0) * 0.25 + phase;
  auto observed = amplitude * time.sin() + 0.03 * torch::randn({count, history + horizon}, options);
  auto past = torch::stack({observed.slice(1, 0, history),
                            time.slice(1, 0, history).sin(),
                            time.slice(1, 0, history).cos()}, -1);
  auto future = torch::stack({time.slice(1, history).sin(), time.slice(1, history).cos()}, -1);
  auto ids = options.dtype(torch::kInt64);
  return {{{amplitude, torch::empty({count, 0}, ids)},
           {past, torch::empty({count, history, 0}, ids)},
           {future, torch::empty({count, horizon, 0}, ids)}},
          observed.slice(1, history)};
}

void require_demo_schema(const praesidium_humanitatis::TrainingSession& session) {
  const auto& c = session.model->config;
  TORCH_CHECK(session.progress.data_id == data_id || session.progress.data_id == legacy_data_id,
              "Checkpoint belongs to a different dataset/generator.");
  TORCH_CHECK(c.static_continuous == 1 && c.past_continuous == 3 && c.future_continuous == 2 &&
                  c.static_categorical_cardinalities.empty() &&
                  c.past_categorical_cardinalities.empty() && c.future_categorical_cardinalities.empty(),
              "Checkpoint features do not match this synthetic generator.");
}

void prepare_output(const std::string& name) {
  if (name.empty()) return;
  const std::filesystem::path path(name);
  if (path.filename().empty() || (std::filesystem::exists(path) && !std::filesystem::is_regular_file(path)))
    throw std::invalid_argument("Output path must name a regular file.");
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
}

void print_metrics(const std::string& label, const praesidium_humanitatis::ForecastMetrics& m) {
  std::cout << label << " quantile_loss=" << m.quantile_loss << " median_mae=" << m.median_mae
            << " interval_coverage=" << m.interval_coverage << " interval_width=" << m.interval_width
            << " crossing_rate=" << m.crossing_rate << '\n';
}

void vector_json(std::ostream& out, const torch::Tensor& values) {
  const auto cpu = values.to(torch::kCPU).to(torch::kFloat64);
  out << '[';
  for (int64_t i = 0; i < cpu.numel(); ++i) {
    if (i) out << ',';
    out << cpu[i].item<double>();
  }
  out << ']';
}

void metrics_json(std::ostream& out, const praesidium_humanitatis::ForecastMetrics& m) {
  out << "{\"quantile_loss\":" << m.quantile_loss << ",\"median_mae\":" << m.median_mae
      << ",\"interval_coverage\":" << m.interval_coverage << ",\"interval_width\":" << m.interval_width
      << ",\"crossing_rate\":" << m.crossing_rate << ",\"quantile_loss_by_horizon\":";
  vector_json(out, m.quantile_loss_by_horizon);
  out << ",\"median_mae_by_horizon\":";
  vector_json(out, m.median_mae_by_horizon);
  out << ",\"interval_coverage_by_horizon\":";
  vector_json(out, m.interval_coverage_by_horizon);
  out << '}';
}

void write_report(const std::string& path, const Options& options, const praesidium_humanitatis::TrainingSession& session,
                  int64_t initial_steps, const praesidium_humanitatis::ForecastMetrics& initial,
                  const praesidium_humanitatis::ForecastMetrics& final, const praesidium_humanitatis::ForecastMetrics& baseline) {
  if (path.empty()) return;
  const std::filesystem::path destination(path);
  const auto parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
  std::filesystem::create_directories(parent);
  std::filesystem::path scratch;
  for (int attempt = 0; attempt < 100; ++attempt) {
    auto candidate = parent / (".praesidium-humanitatis-report-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
    if (std::filesystem::create_directory(candidate)) { scratch = candidate; break; }
  }
  if (scratch.empty()) throw std::runtime_error("Could not reserve report temporary directory.");
  const auto temporary = scratch / "report.json";
  try {
    std::ofstream out(temporary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << std::setprecision(12) << "{\n\"format_version\":1,\"data_id\":\"" << session.progress.data_id
        << "\",\"libtorch\":\"" << TORCH_VERSION << "\",\"device\":\"" << options.device
        << "\",\"dtype\":\"" << (session.model->parameters().front().scalar_type() == torch::kFloat64 ? "float64" : "float32")
        << "\",\"seed\":" << session.progress.seed << ",\"initial_steps\":" << initial_steps
        << ",\"completed_steps\":" << session.progress.completed_steps
        << ",\"validation_size\":" << validation_size << ",\"batch_size\":" << batch_size
        << ",\"history\":" << history << ",\"horizon\":" << horizon
        << ",\"hidden_size\":" << session.model->config.hidden_size
        << ",\"attention_heads\":" << session.model->config.attention_heads
        << ",\"dropout\":" << session.model->config.dropout
        << ",\"ordered_quantiles\":" << (session.model->config.ordered_quantiles ? "true" : "false")
        << ",\"gradient_clip_norm\":" << session.progress.gradient_clip_norm << ",\"quantiles\":[";
    for (size_t i = 0; i < session.model->config.quantiles.size(); ++i) {
      if (i) out << ',';
      out << session.model->config.quantiles[i];
    }
    out << "],\"optimizer_groups\":[";
    for (size_t i = 0; i < session.optimizer->param_groups().size(); ++i) {
      if (i) out << ',';
      const auto& group = session.optimizer->param_groups()[i];
      const auto& adam = dynamic_cast<const torch::optim::AdamOptions&>(group.options());
      out << "{\"lr\":" << adam.lr() << ",\"beta1\":" << std::get<0>(adam.betas())
          << ",\"beta2\":" << std::get<1>(adam.betas()) << ",\"eps\":" << adam.eps()
          << ",\"weight_decay\":" << adam.weight_decay() << ",\"amsgrad\":"
          << (adam.amsgrad() ? "true" : "false") << ",\"parameter_count\":" << group.params().size() << '}';
    }
    out << "],\n\"initial\":"; metrics_json(out, initial);
    out << ",\n\"final\":"; metrics_json(out, final);
    out << ",\n\"persistence\":"; metrics_json(out, baseline);
    out << "\n}\n";
    out.close();
    std::filesystem::rename(temporary, destination);
    std::filesystem::remove(scratch);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::remove(scratch, ignored);
    throw;
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse(argc, argv);
    if (options.help) {
      std::cout << "Synthetic TFT training and evaluation\n"
                   "  --device cpu|cuda    execution device (default cpu)\n"
                   "  --steps N            additional updates, 1..100000 (default 80)\n"
                   "  --seed N             fresh-run seed (default 20260916)\n"
                   "  --ordered-quantiles  train noncrossing outputs (fresh runs only)\n"
                   "  --checkpoint PATH    save resumable checkpoint\n"
                   "  --save-every N       save every N updates; 0=final only (default 20)\n"
                   "  --resume PATH        restore model, Adam, progress and seed\n"
                   "  --evaluate-only      evaluate --resume without updates\n"
                   "  --report PATH        write JSON metrics and persistence baseline\n"
                   "Synthetic results are not a real-world benchmark.\n";
      return 0;
    }
    if (options.device == "cuda" && !torch::cuda::is_available())
      throw std::runtime_error("CUDA requested but unavailable.");
    const torch::Device device(options.device);
    torch::set_num_threads(1);
    praesidium_humanitatis::TrainingSession session;
    if (options.resume.empty()) {
      torch::manual_seed(options.seed);
      praesidium_humanitatis::Config config;
      config.past_continuous = 3;
      config.future_continuous = 2;
      config.ordered_quantiles = options.ordered_quantiles;
      session.model = praesidium_humanitatis::TFT(config);
      session.model->to(device);
      session.optimizer = std::make_unique<torch::optim::Adam>(
          session.model->parameters(), torch::optim::AdamOptions(0.003));
      session.progress.seed = options.seed;
      session.progress.data_id = data_id;
    } else {
      session = praesidium_humanitatis::load_checkpoint(options.resume, device);
      require_demo_schema(session);
    }
    const int64_t initial_steps = session.progress.completed_steps;
    const int64_t updates = options.evaluate_only ? 0 : options.steps;
    TORCH_CHECK(initial_steps <= std::numeric_limits<int64_t>::max() - session.progress.seed &&
                    updates <= std::numeric_limits<int64_t>::max() - session.progress.seed - initial_steps,
                "Training progress would overflow the reproducible seed sequence.");
    prepare_output(options.checkpoint);
    prepare_output(options.report);
    // A report's parent creation must not turn the checkpoint path into a directory.
    prepare_output(options.checkpoint);
    const auto tensor_options = session.model->parameters().front().options().requires_grad(false);
    // Validation has a separate fixed stream; every training update reseeds.
    torch::manual_seed(static_cast<uint64_t>(session.progress.seed) ^ 0x5DEECE66DULL);
    const auto validation = generate(validation_size, tensor_options);
    const auto evaluate = [&]() {
      torch::NoGradGuard no_grad;
      session.model->eval();
      return praesidium_humanitatis::evaluate_forecasts(session.model->forward(validation.input).predictions,
                                      validation.target, session.model->config.quantiles);
    };
    const auto initial = evaluate();
    const auto baseline = praesidium_humanitatis::evaluate_forecasts(praesidium_humanitatis::persistence_forecast(
        validation.input.past.continuous.select(1, history - 1).select(1, 0),
        horizon, static_cast<int64_t>(session.model->config.quantiles.size())),
        validation.target, session.model->config.quantiles);
    std::cout << "device=" << options.device << " seed=" << session.progress.seed
              << " completed_steps=" << initial_steps << '\n';
    print_metrics("initial", initial);
    print_metrics("persistence", baseline);
    for (int64_t step = 0; step < updates; ++step) {
      torch::manual_seed(session.progress.seed + session.progress.completed_steps);
      session.model->train();
      const auto batch = generate(batch_size, tensor_options);
      session.optimizer->zero_grad();
      const auto prediction = session.model->forward(batch.input).predictions;
      auto loss = praesidium_humanitatis::quantile_loss(prediction, batch.target, session.model->config.quantiles);
      loss.backward();
      const double norm = torch::nn::utils::clip_grad_norm_(
          session.model->parameters(), session.progress.gradient_clip_norm);
      TORCH_CHECK(std::isfinite(norm), "Gradient norm became nonfinite.");
      session.optimizer->step();
      ++session.progress.completed_steps;
      if (step == 0 || session.progress.completed_steps % 20 == 0 || step + 1 == updates)
        std::cout << "step=" << session.progress.completed_steps << " training_loss=" << loss.item<double>() << '\n';
      if (!options.checkpoint.empty() && options.save_every > 0 &&
          session.progress.completed_steps % options.save_every == 0 && step + 1 != updates)
        praesidium_humanitatis::save_checkpoint(options.checkpoint, session.model, *session.optimizer, session.progress);
    }
    const auto final = evaluate();
    print_metrics("final", final);
    if (!options.checkpoint.empty()) {
      praesidium_humanitatis::save_checkpoint(options.checkpoint, session.model, *session.optimizer, session.progress);
      std::cout << "checkpoint=" << options.checkpoint << '\n';
    }
    // Recheck after publication: newly created paths may alias on a
    // case-insensitive mount even when neither existed during option parsing.
    if (same_path(options.report, options.checkpoint) || same_path(options.report, options.resume))
      throw std::invalid_argument("Report path aliases a checkpoint; the checkpoint was preserved.");
    write_report(options.report, options, session, initial_steps, initial, final, baseline);
    if (!options.report.empty()) std::cout << "report=" << options.report << '\n';
    std::cout << "Synthetic validation only. Persistence repeats the last observation at all quantiles; "
                 "it has zero-width intervals. Evaluate real temporal holdouts before drawing conclusions.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "tft_demo: " << error.what() << '\n';
    return 1;
  }
}
