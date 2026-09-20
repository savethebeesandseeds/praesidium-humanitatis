# Third-party dependencies and reference material

Repository material defaults to MIT, with the price optimization engine as an
explicit exception under its own worker-protection license. See
[license boundaries](licensing.md). Dependencies have separate licenses;
neither project license replaces them.

- nlohmann/json 3.12.0: vendored under MIT in the optimizer's
  `third_party/nlohmann/` directory, expressly excluded from its custom license.
  The unmodified header was verified against the upstream release's published
  SHA-256. See its [provenance and license](../tools/price-optimization/third_party/nlohmann/PROVENANCE.md).

- Emscripten 4.0.15: builds the exchange simulator's WebAssembly and JavaScript
  runtime. Its MIT runtime notice and the bundled LLVM C/C++/compiler-rt,
  libunwind and musl notices are embedded in the standalone HTML and its source
  archive. The compiler SDK remains ignored local build data. See the
  [pinned build provenance](../projects/post-profit-exchange/environment/README.md).

- AMPL interpreter, C++ API/SDK, and AMPL solver drivers: separately supplied
  vendor components governed by the applicable [AMPL agreement](https://ampl.com/terms-eula/).
  The engine calls AMPL through its C++ API; it does not implement or redistribute
  AMPL. Operational store use needs an appropriate entitlement. A Community
  Edition or nonprofit label is not sufficient evidence of that permission.
- HiGHS: an optional mixed-integer solver, whose upstream source uses
  [MIT](https://github.com/ERGO-Code/HiGHS/blob/master/LICENSE.txt). AMPL runtime,
  driver packaging, and any alternative solver retain their applicable terms.
  No solver binary is included in the source release. The local evaluation
  runtime and its supplied notices remain under ignored `.build/deps/`.

- LibTorch / PyTorch: BSD-style license and bundled third-party notices. Preserve
  the complete `LICENSE`, `NOTICE`, and other license files shipped with the
  staged LibTorch distribution when redistributing it. See the
  [upstream license](https://github.com/pytorch/pytorch/blob/v2.6.0/LICENSE).
- NVIDIA CUDA and cuDNN: distributed under NVIDIA's applicable license terms.
  The dependency installer uses NVIDIA's official Debian repository. Check those
  terms before distributing a binary environment.
- Debian packages: individual package copyright/license files under
  `/usr/share/doc/<package>/copyright` in the container.
- Model design reference: Lim et al., *Temporal Fusion Transformers for
  Interpretable Multi-horizon Time Series Forecasting*,
  [paper](https://arxiv.org/abs/1912.09363). The Google reference implementation is
  Apache-2.0 licensed. This project independently implements the architecture in
  C++. The replication harness downloads a pinned copy of the reference Python
  source into ignored `.build/reference/tft`, retaining its copyright/license
  headers. It is not included in the project source release.
- Original-reference runtime: TensorFlow 1.15.5 and its pinned Python dependencies
  retain their upstream licenses and package notices. The isolated Miniconda
  runtime and downloaded wheels remain build artifacts; preserve their notices
  when redistributing an environment.
- Electricity benchmark data: Trindade, A. (2015), *ElectricityLoadDiagrams20112014*,
  UCI Machine Learning Repository, [DOI](https://doi.org/10.24432/C58C86), CC BY 4.0.
  Raw data and derived tensors stay outside version control; source URLs and
  checksums are recorded with each prepared dataset.
- Environment procedure adapted from the user's local `cuwacunu_embedding`
  project; see `environment.md` for its inspected revision and dependency pins.

Vendor dependency binaries and working build artifacts stay in ignored `.build/`
and `.temp/` directories. The explicit exception is the exchange project's
standalone HTML in its `dist/` folder: it embeds the compiled simulation and
pricing code, runtime notices and corresponding source, without AMPL binaries.
