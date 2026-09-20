#pragma once

// Indexed Electricity export reader. Windows are gathered on demand; the
// 450,000 training windows are never materialized as one dense input tensor.
#include "praesidium-humanitatis/tft.h"
#include <torch/torch.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace praesidium_humanitatis::replication {
namespace fs = std::filesystem;

inline std::vector<std::string> fields(const std::string& text, char delimiter) {
  std::vector<std::string> values;
  size_t start = 0;
  do {
    const auto end = text.find(delimiter, start);
    values.push_back(text.substr(start, end == std::string::npos ? end : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  } while (true);
  return values;
}

inline int64_t integer(const std::string& value, const std::string& label) {
  size_t consumed = 0;
  const auto result = std::stoll(value, &consumed);
  TORCH_CHECK(consumed == value.size(), "Invalid integer for ", label);
  return result;
}

inline std::map<std::string, std::string> read_settings(const fs::path& path) {
  std::ifstream input(path);
  TORCH_CHECK(input.good(), "Cannot read settings: ", path.string());
  std::map<std::string, std::string> settings;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto cells = fields(line, '\t');
    TORCH_CHECK(cells.size() == 2 && settings.emplace(cells[0], cells[1]).second,
                "Invalid or duplicate settings record in ", path.string());
  }
  return settings;
}

inline torch::Tensor read_binary(const fs::path& root, const std::string& filename,
                                 const std::string& dtype, const std::string& dimensions) {
  const uint16_t endian = 1;
  TORCH_CHECK(*reinterpret_cast<const uint8_t*>(&endian) == 1,
              "Electricity exports require a little-endian host");
  const fs::path relative(filename);
  TORCH_CHECK(!relative.empty() && relative == relative.filename() &&
                  relative != "." && relative != "..",
              "Tensor filename must be a single relative filename");
  const auto type = dtype == "<f4" || dtype == "f32" ? torch::kFloat32
                    : dtype == "<f8" ? torch::kFloat64 : torch::kInt64;
  TORCH_CHECK(dtype == "<f4" || dtype == "f32" || dtype == "<f8" ||
                  dtype == "<i8" || dtype == "i64", "Unsupported tensor dtype: ", dtype);
  std::vector<int64_t> shape;
  int64_t elements = 1;
  if (!dimensions.empty()) {
    for (const auto& size : fields(dimensions, ',')) {
      const auto n = integer(size, "tensor dimension");
      TORCH_CHECK(n >= 0 && (n == 0 || elements <= std::numeric_limits<int64_t>::max() / n),
                  "Invalid or overflowing tensor shape");
      elements *= n;
      shape.push_back(n);
    }
  }
  const auto width = type == torch::kFloat32 ? 4 : 8;
  TORCH_CHECK(elements <= std::numeric_limits<int64_t>::max() / width,
              "Tensor byte count overflow");
  const auto bytes = static_cast<uintmax_t>(elements) * width;
  const auto path = root / relative;
  TORCH_CHECK(fs::is_regular_file(path) && fs::file_size(path) == bytes,
              "Tensor file size mismatch: ", path.string());
  auto result = torch::empty(shape, type);
  std::ifstream input(path, std::ios::binary);
  input.read(static_cast<char*>(result.data_ptr()), static_cast<std::streamsize>(bytes));
  TORCH_CHECK(input.good(), "Cannot read tensor: ", path.string());
  return result;
}

class TensorManifest {
 public:
  explicit TensorManifest(fs::path root, const std::string& manifest = "tensors.tsv") : root_(std::move(root)) {
    std::ifstream input(root_ / manifest);
    TORCH_CHECK(input.good(), "Cannot read tensor manifest: ", (root_ / manifest).string());
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      const auto cells = fields(line, '\t');
      TORCH_CHECK(cells.size() == 4 && descriptions_.emplace(cells[0], cells).second,
                  "Invalid or duplicate tensor manifest record");
    }
  }

  torch::Tensor get(const std::string& name) const {
    const auto& item = descriptions_.at(name);
    return read_binary(root_, item[3], item[1], item[2]);
  }
  bool contains(const std::string& name) const { return descriptions_.count(name) != 0; }

 private:
  fs::path root_;
  std::map<std::string, std::vector<std::string>> descriptions_;
};

struct ElectricityBatch {
  Batch input;
  torch::Tensor targets;
  std::vector<int64_t> entities, starts;
};

class ElectricityData {
 public:
  explicit ElectricityData(const fs::path& root) : manifest(root) {
    features = manifest.get("features");
    spans = manifest.get("entity_spans");
    categories = manifest.get("entity_categories");
    target_stats = manifest.get("target_mean_scale");
    raw_targets = manifest.get("raw_targets");
    TORCH_CHECK(features.scalar_type() == torch::kFloat32 && features.dim() == 2 &&
                    features.size(0) > 0 && features.size(1) == 4,
                "Expected float32 features [rows,4]");
    TORCH_CHECK(spans.scalar_type() == torch::kInt64 && spans.dim() == 2 &&
                    spans.size(0) > 0 && spans.size(1) == 2,
                "Expected int64 entity spans [entities,2]");
    TORCH_CHECK(categories.scalar_type() == torch::kInt64 && categories.dim() == 1 &&
                    categories.size(0) == spans.size(0), "Invalid static entity categories");
    TORCH_CHECK(target_stats.scalar_type() == torch::kFloat64 && target_stats.dim() == 2 &&
                    target_stats.size(0) == spans.size(0) && target_stats.size(1) == 2,
                "Invalid target scaling statistics");
    TORCH_CHECK(raw_targets.scalar_type() == torch::kFloat64 && raw_targets.dim() == 1 &&
                    raw_targets.size(0) == features.size(0) && torch::isfinite(raw_targets).all().item<bool>(),
                "Expected finite float64 raw targets [rows]");
    TORCH_CHECK(torch::isfinite(features).all().item<bool>() &&
                    torch::isfinite(target_stats).all().item<bool>() &&
                    (target_stats.select(1, 1) > 0).all().item<bool>(),
                "Features/scalers must be finite, with positive scale");
    TORCH_CHECK((categories >= 0).all().item<bool>() && (categories < 370).all().item<bool>(),
                "Category IDs must use the fixed reference vocabulary of size 370");
    int64_t expected_start = 0;
    const auto* positions = spans.data_ptr<int64_t>();
    for (int64_t i = 0; i < spans.size(0); ++i) {
      const auto start = positions[2 * i], count = positions[2 * i + 1];
      TORCH_CHECK(start == expected_start && count > 0 && count <= features.size(0) - start,
                  "Entity spans must partition the stored feature rows");
      expected_start += count;
    }
    TORCH_CHECK(expected_start == features.size(0), "Entity spans do not cover all rows");
  }

  torch::Tensor windows(const std::string& name) const {
    auto result = manifest.get(name);
    TORCH_CHECK(result.scalar_type() == torch::kInt64 && result.dim() == 2 &&
                    result.size(0) > 0 && result.size(1) == 2,
                "Expected nonempty int64 windows [count,2]: ", name);
    const auto* indices = result.data_ptr<int64_t>();
    const auto* entities = spans.data_ptr<int64_t>();
    for (int64_t i = 0; i < result.size(0); ++i) {
      const auto entity = indices[2 * i], start = indices[2 * i + 1];
      TORCH_CHECK(entity >= 0 && entity < spans.size(0), "Invalid window entity");
      const auto offset = entities[2 * entity], length = entities[2 * entity + 1];
      TORCH_CHECK(length >= total && start >= offset && start <= offset + length - total,
                  "Window crosses its entity boundary");
    }
    return result;
  }

  ElectricityBatch batch(const torch::Tensor& windows, const std::vector<int64_t>& indices,
                         torch::Device device) const {
    TORCH_CHECK(!indices.empty(), "Cannot build an empty Electricity batch");
    const auto n = static_cast<int64_t>(indices.size());
    auto past = torch::empty({n, history, 4}, torch::kFloat32);
    auto future = torch::empty({n, horizon, 3}, torch::kFloat32);
    auto target = torch::empty({n, horizon}, torch::kFloat32);
    auto ids = torch::empty({n, 1}, torch::kInt64);
    auto* p = past.data_ptr<float>(); auto* f = future.data_ptr<float>();
    auto* y = target.data_ptr<float>(); auto* c = ids.data_ptr<int64_t>();
    const auto* x = features.data_ptr<float>();
    const auto* w = windows.data_ptr<int64_t>();
    const auto* category = categories.data_ptr<int64_t>();
    ElectricityBatch result;
    for (int64_t b = 0; b < n; ++b) {
      const auto index = indices[b];
      TORCH_CHECK(index >= 0 && index < windows.size(0), "Window index out of bounds");
      const auto entity = w[2 * index], start = w[2 * index + 1];
      result.entities.push_back(entity); result.starts.push_back(start);
      c[b] = category[entity];
      for (int64_t t = 0; t < history; ++t) {
        for (int64_t v = 0; v < 4; ++v)
          p[(b * history + t) * 4 + v] = x[(start + t) * 4 + (v + 1) % 4];
      }
      for (int64_t t = 0; t < horizon; ++t) {
        for (int64_t v = 0; v < 3; ++v)
          f[(b * horizon + t) * 3 + v] = x[(start + history + t) * 4 + v + 1];
        y[b * horizon + t] = x[(start + history + t) * 4];
      }
    }
    auto floats = torch::TensorOptions().dtype(torch::kFloat32).device(device);
    auto ints = floats.dtype(torch::kInt64);
    result.input = {{torch::empty({n, 0}, floats), ids.to(device)},
                    {past.to(device), torch::empty({n, history, 0}, ints)},
                    {future.to(device), torch::empty({n, horizon, 0}, ints)}};
    result.targets = target.to(device);
    return result;
  }

  static constexpr int64_t history = 168, horizon = 24, total = history + horizon;
  TensorManifest manifest;
  torch::Tensor features, spans, categories, target_stats, raw_targets;
};
}  // namespace praesidium_humanitatis::replication
