#pragma once

#include <torch/torch.h>
#include <vector>

namespace praesidium_humanitatis::replication {

// The pinned TensorFlow loss forms separate positive/negative parts, with
// (1-q) evaluated in Python double before casting to the prediction dtype.
// clamp_min's derivative is 1 at equality, matching TensorFlow maximum(e, 0)
// ties. torch::maximum of the two pinball branches instead splits tie gradients.
// Keep this exact reference convention local to the replication harness.
inline torch::Tensor reference_quantile_loss(const torch::Tensor& predictions,
                                            const torch::Tensor& targets,
                                            const std::vector<double>& quantiles) {
  TORCH_CHECK(predictions.dim() == 3 && targets.dim() == 2 &&
                  predictions.size(0) == targets.size(0) &&
                  predictions.size(1) == targets.size(1) &&
                  predictions.size(2) == static_cast<int64_t>(quantiles.size()),
              "Invalid complete-window reference loss dimensions");
  std::vector<double> complements;
  for (const auto q : quantiles) {
    TORCH_CHECK(q > 0 && q < 1, "Invalid reference quantile");
    complements.push_back(1. - q);
  }
  const auto positive = torch::tensor(quantiles, torch::kFloat64).to(predictions.options());
  const auto negative = torch::tensor(complements, torch::kFloat64).to(predictions.options());
  const auto error = targets.unsqueeze(-1) - predictions;
  return (positive * error.clamp_min(0) + negative * (-error).clamp_min(0)).sum(-1).mean();
}

}  // namespace praesidium_humanitatis::replication
