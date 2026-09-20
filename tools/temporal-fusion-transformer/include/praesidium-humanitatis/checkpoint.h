#pragma once

#include "praesidium-humanitatis/tft.h"

#include <cstdint>
#include <memory>
#include <string>

namespace praesidium_humanitatis {

struct TrainingProgress {
  int64_t completed_steps = 0;
  int64_t seed = 20260916;
  double gradient_clip_norm = 1.0;
  // Caller-defined dataset/generator version; preprocessing remains caller-owned.
  std::string data_id;
};

struct TrainingSession {
  TFT model{nullptr};
  std::unique_ptr<torch::optim::Adam> optimizer;
  TrainingProgress progress;
};

// Saves format version 3, the complete Config, float32/float64 model (including
// train/eval mode), Adam groups/options/moments, and progress. Adam must reference
// every model parameter exactly once. Call between completed optimizer updates,
// without concurrent model/optimizer mutation. Existing files are replaced by an
// atomic rename on Linux; failed saves preserve the previous checkpoint.
//
// RNG state and data preprocessing are not captured. For reproducible dropout,
// callers can seed each update with progress.seed + its absolute update index.
void save_checkpoint(const std::string& path, const TFT& model,
                     const torch::optim::Adam& optimizer,
                     const TrainingProgress& progress);

// Reconstructs the model and optimizer on the requested CPU/CUDA device. Loading
// supports versions 1, 2 and 3; version 1 retains its independent quantile head.
// Versions 1/2 retain independent embedding groups and LayerNorm epsilon 1e-5.
// Loading constructs a model and consumes RNG state: reseed before an update.
// Checkpoints are LibTorch archives and should only be loaded from trusted input.
TrainingSession load_checkpoint(const std::string& path,
                                torch::Device device = torch::kCPU);

}  // namespace praesidium_humanitatis
