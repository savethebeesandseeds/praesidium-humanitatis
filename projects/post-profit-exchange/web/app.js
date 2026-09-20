/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Waajacu */
(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const clone = (value) => JSON.parse(JSON.stringify(value));
  const state = { config: null, result: null, runConfig: null, currentDay: 0,
    dirty: false, busy: false, worker: null, sequence: 0, pending: null };
  const colors = { optimized: "#155f93", fixed: "#9c4613" };
  const modes = ["optimized", "fixed"];
  const modeNames = { optimized: "Optimized", fixed: "Fixed price" };
  const numberFormat = new Intl.NumberFormat("en", { maximumFractionDigits: 4 });
  const format = (value) => value === null || value === undefined ? "—"
    : typeof value === "number" ? numberFormat.format(value)
    : typeof value === "boolean" ? (value ? "yes" : "no") : String(value);

  const globalFields = [
    ["periods", "Days to simulate", 1, 365], ["seed", "Random seed", 0, 4294967295],
    ["currency", "Currency code", "text"], ["initial_cash", "Initial total cash (cents)", 0, 100000000],
    ["initial_reserve", "Initial earmarked reserve (cents)", 0, 100000000],
    ["procurement_budget", "Daily procurement budget (cents)", 0, 100000000],
    ["worker_wages", "Daily worker wages due (cents)", 1, 100000000],
    ["operating_cost", "Daily operating cost due (cents)", 0, 100000000],
    ["reserve_contribution", "Daily reserve contribution (cents)", 0, 100000000],
    ["reserve_target", "Reserve target (cents)", 0, 100000000],
    ["demand_noise_bps", "Demand noise range (basis points)", 0, 10000]
  ];
  const shockFields = [
    ["start_day", "First shock day (0 disables)", 0, 365],
    ["end_day", "Last shock day (inclusive; 0 disables)", 0, 365],
    ["demand_factor_bps", "Demand factor (basis points)", 0, 30000],
    ["cost_factor_bps", "Unit cost factor (basis points)", 0, 30000]
  ];
  const productFields = [
    ["sku", "SKU", "text"], ["label", "Product label", "text"],
    ["unit_cost", "Base unit cost (cents)", 0, 1000000],
    ["reference_price", "Reference price (cents)", 1, 1000000],
    ["fixed_price", "Fixed comparison price (cents)", 1, 1000000],
    ["initial_stock", "Initial stock (units)", 0, 100000],
    ["target_stock", "Procurement stock target (units)", 0, 100000],
    ["base_demand", "Daily demand at reference price (units)", 0, 10000],
    ["elasticity_bps", "Synthetic price-response slope (basis points)", 0, 30000],
    ["spoilage_bps", "Daily spoilage (basis points)", 0, 10000],
    ["affordability_ceiling", "Price ceiling (cents)", 1, 1000000],
    ["max_change_bps", "Per-day price change cap (basis points)", 0, 10000]
  ];
  const scenarioFields = [["id", "ID", "text"], ["factor_bps", "Demand factor", 0, 30000],
    ["probability", "Probability", 0, 1, "any"]];

  function element(tag, text, className) {
    const node = document.createElement(tag);
    if (text !== undefined) node.textContent = text;
    if (className) node.className = className;
    return node;
  }
  function showError(message) {
    $("error").textContent = message;
    $("error").hidden = !message;
  }
  function status(message) { $("status").textContent = message; }

  function inputFor(spec, path, value, ariaLabel) {
    const [key, , minimum, maximum, step] = spec;
    const input = element("input");
    input.dataset.path = path;
    input.id = `field-${path.replaceAll(".", "-")}`;
    input.type = minimum === "text" ? "text" : "number";
    input.required = true;
    if (input.type === "number") {
      input.min = minimum;
      input.max = maximum;
      input.step = step || "1";
    } else {
      input.maxLength = key === "currency" ? 3 : 64;
      if (key === "currency") input.pattern = "[A-Z]{3}";
    }
    if (ariaLabel) input.setAttribute("aria-label", ariaLabel);
    input.value = value;
    return input;
  }

  function appendFields(container, specs, prefix, values) {
    for (const spec of specs) {
      const [key, title] = spec;
      const label = element("label");
      label.append(element("span", title, "field-name"), element("span", `${prefix}${key}`, "field-key"));
      label.append(inputFor(spec, `${prefix}${key}`, values[key]));
      container.append(label);
    }
  }

  function button(text, listener) {
    const result = element("button", text);
    result.type = "button";
    result.addEventListener("click", listener);
    return result;
  }

  function buildConfig(config) {
    state.config = clone(config);
    $("global-fields").replaceChildren();
    appendFields($("global-fields"), globalFields, "", config);
    $("shock-fields").replaceChildren();
    appendFields($("shock-fields"), shockFields, "shock.", config.shock);
    const scenarioBody = $("scenario-table").tBodies[0];
    scenarioBody.replaceChildren();
    config.scenarios.forEach((scenario, index) => {
      const row = element("tr");
      scenarioFields.forEach((spec) => {
        const cell = element("td");
        cell.append(inputFor(spec, `scenarios.${index}.${spec[0]}`, scenario[spec[0]],
          `Scenario ${index + 1}: ${spec[1]}`));
        row.append(cell);
      });
      scenarioBody.append(row);
    });
    $("product-fields").replaceChildren();
    config.products.forEach((product, index) => {
      const fieldset = element("fieldset");
      fieldset.append(element("legend", `Product ${index + 1}`));
      const fields = element("div", undefined, "fields");
      appendFields(fields, productFields, `products.${index}.`, product);
      fieldset.append(fields, element("p", "Candidate public prices (cents; 1–9 distinct values)", "note"));
      const candidates = element("div", undefined, "candidates");
      product.candidate_prices.forEach((price, candidate) => {
        const label = element("label", `Price ${candidate + 1}`);
        label.append(inputFor(["price", "Price", 1, 1000000],
          `products.${index}.candidate_prices.${candidate}`, price));
        candidates.append(label);
      });
      fieldset.append(candidates);
      const actions = element("div", undefined, "actions candidate-controls");
      const add = button("Add candidate price", () => mutateConfig((next) => {
        const prices = next.products[index].candidate_prices;
        let candidate = Math.min(1000000, Math.max(...prices) + 10);
        while (prices.includes(candidate) && candidate > 1) --candidate;
        prices.push(candidate);
      }));
      add.disabled = product.candidate_prices.length >= 9;
      add.dataset.boundDisabled = String(add.disabled);
      const remove = button("Remove last candidate", () => mutateConfig((next) => next.products[index].candidate_prices.pop()));
      remove.disabled = product.candidate_prices.length <= 1;
      remove.dataset.boundDisabled = String(remove.disabled);
      const removeProduct = button(`Remove product ${index + 1}`, () => mutateConfig((next) => next.products.splice(index, 1)));
      removeProduct.disabled = config.products.length <= 1;
      removeProduct.dataset.boundDisabled = String(removeProduct.disabled);
      actions.append(add, remove, removeProduct);
      fieldset.append(actions);
      $("product-fields").append(fieldset);
    });
    $("config-json").value = JSON.stringify(config, null, 2);
    updateButtons();
  }

  function readConfig(validate = true) {
    if (!state.config) throw new Error("Load the C++ defaults before running the simulation.");
    if (validate && !$("config-form").reportValidity()) return null;
    const config = clone(state.config);
    for (const input of $("config-form").querySelectorAll("input[data-path]")) {
      const path = input.dataset.path.split(".");
      let owner = config;
      for (let i = 0; i < path.length - 1; ++i) owner = owner[path[i]];
      const value = input.type === "number" ? input.valueAsNumber : input.value;
      if (input.type === "number" && !Number.isFinite(value)) throw new Error(`${input.dataset.path} needs a number.`);
      owner[path[path.length - 1]] = value;
    }
    return config;
  }

  function configChanged() {
    state.currentDay = 0;
    state.dirty = !!state.result;
    showError("");
    status("Configuration changed. The next Step starts at day 1.");
    if (state.result) $("result-note").textContent = "Previous completed result: configuration has changed. Run again to compare the new configuration.";
    updateButtons();
  }

  function mutateConfig(mutation) {
    try {
      const config = readConfig();
      if (!config) return;
      mutation(config);
      buildConfig(config);
      configChanged();
    } catch (error) { showError(error.message); }
  }

  function updateButtons() {
    const ready = !!state.config;
    for (const id of ["run", "step", "reset", "add-product", "refresh-json", "load-json"]) $(id).disabled = state.busy || !ready;
    $("defaults").disabled = state.busy;
    $("stop").disabled = !state.busy;
    $("export-json").disabled = !state.result;
    $("export-csv").disabled = !state.result;
    if (ready && !state.busy) {
      $("add-product").disabled = state.config.products.length >= 3;
      const periods = document.querySelector('[data-path="periods"]');
      $("step").disabled = !state.dirty && state.currentDay >= Number(periods.value);
    }
    for (const input of $("config-form").querySelectorAll("input, button")) {
      input.disabled = state.busy || input.dataset.boundDisabled === "true";
    }
    $("add-product").disabled = state.busy || !ready || state.config.products.length >= 3;
    $("config-json").disabled = state.busy;
    $("config-form").setAttribute("aria-busy", String(state.busy));
  }

  function disposeWorker() {
    if (state.worker) state.worker.terminate();
    state.worker = null;
    ++state.sequence;
    state.pending = null;
  }

  function createWorker() {
    if (state.worker) return state.worker;
    const runtime = $("wasm-runtime").textContent;
    if (!runtime.trim() || !runtime.includes("createExchangeModule")) throw new Error("The embedded WebAssembly runtime is missing. Open the packaged standalone HTML file.");
    const handler = `
      let exchangeModulePromise;
      self.onmessage = async (event) => {
        const {id, command} = event.data;
        try {
          if (!exchangeModulePromise) exchangeModulePromise = createExchangeModule({print:()=>{}, printErr:()=>{}});
          const module = await exchangeModulePromise;
          const result = JSON.parse(module.ccall('exchange_run', 'string', ['string'], [JSON.stringify(command)]));
          self.postMessage({id, result});
        } catch (error) { self.postMessage({id, error: error && error.message ? error.message : String(error)}); }
      };`;
    const url = URL.createObjectURL(new Blob([runtime, "\n", handler], { type: "text/javascript" }));
    let worker;
    try { worker = new Worker(url); } finally { URL.revokeObjectURL(url); }
    worker.addEventListener("message", (event) => {
      if (worker !== state.worker || !state.pending || event.data.id !== state.pending.id) return;
      const pending = state.pending;
      state.pending = null;
      state.busy = false;
      try {
        if (event.data.error) throw new Error(event.data.error);
        const result = event.data.result;
        if (!result || result.status !== "ok") throw new Error(result?.error?.message || "C++ returned an invalid result.");
        if (result.schema_version !== "exchange.sim.v1") throw new Error("Unsupported simulation response schema.");
        pending.complete(result);
      } catch (error) {
        showError(error.message);
        status("The request failed. No new result was applied.");
      }
      updateButtons();
    });
    worker.addEventListener("error", (event) => {
      if (worker !== state.worker) return;
      event.preventDefault();
      disposeWorker();
      state.busy = false;
      showError(event.message || "The WebAssembly worker could not start.");
      status("Worker failed. Restore defaults or run again to retry.");
      updateButtons();
    });
    state.worker = worker;
    return worker;
  }

  function command(commandValue, complete, progress) {
    if (state.busy) return;
    showError("");
    try {
      const worker = createWorker();
      const id = ++state.sequence;
      state.pending = { id, complete };
      state.busy = true;
      updateButtons();
      status(progress);
      worker.postMessage({ id, command: commandValue });
    } catch (error) {
      disposeWorker();
      state.busy = false;
      updateButtons();
      showError(error.message);
      status("Unable to start the C++ simulation.");
    }
  }

  function clearResults() {
    state.result = null;
    state.runConfig = null;
    state.currentDay = 0;
    state.dirty = false;
    $("summary").replaceChildren();
    $("charts").replaceChildren();
    $("ledger").replaceChildren(element("p", "No daily records yet."));
    $("product-detail").replaceChildren(element("p", "No day selected."));
    $("selected-day").replaceChildren(element("option", "No results"));
    $("selected-day").disabled = true;
    $("raw-result").textContent = "No result.";
    $("result-note").textContent = "No simulation has been run.";
    showError("");
    updateButtons();
  }

  function defaults() {
    command({ op: "defaults" }, (result) => {
      clearResults();
      buildConfig(result.config);
      status("C++ / WebAssembly ready. Defaults loaded; no simulation has been run.");
    }, "Loading the embedded C++ / WebAssembly runtime and its defaults…");
  }

  function run(step) {
    try {
      const config = readConfig();
      if (!config) return;
      const horizon = config.periods;
      const days = step ? Math.min(horizon, state.currentDay + 1) : horizon;
      const requested = clone(config);
      requested.periods = days;
      command({ op: "simulate", config: requested }, (result) => {
        if (!Array.isArray(result.optimized?.rows) || !Array.isArray(result.fixed?.rows)) throw new Error("Missing daily simulation records.");
        state.result = result;
        state.runConfig = clone(config);
        state.config = clone(config);
        state.currentDay = days;
        state.dirty = false;
        renderResults();
        status(`Completed ${days} of ${horizon} days for both policies. Seed ${config.seed}.`);
      }, `Simulating days 1–${days} for both policies in the C++ worker…`);
    } catch (error) { showError(error.message); }
  }

  function table(headers, rows, caption) {
    const output = element("table");
    if (caption) output.append(element("caption", caption));
    const head = element("thead");
    const header = element("tr");
    headers.forEach((name) => { const th = element("th", name); th.scope = "col"; header.append(th); });
    head.append(header);
    const body = element("tbody");
    rows.forEach((cells) => {
      const row = element("tr");
      cells.forEach((value) => {
        const cell = element("td");
        if (value instanceof Node) cell.append(value); else cell.textContent = format(value);
        row.append(cell);
      });
      body.append(row);
    });
    output.append(head, body);
    return output;
  }

  const summaryMetrics = [
    ["economic_result", "Realized FIFO economic result (cents)"], ["closing_cash", "Closing total cash (cents)"],
    ["available_cash", "Closing available cash (cents)"], ["reserve_balance", "Closing reserve (cents)"],
    ["initial_inventory_value", "Initial inventory endowment (cents)"],
    ["closing_inventory_value", "Closing inventory value (cents)"],
    ["worker_wages_due", "Worker wages due (cents)"], ["worker_wages_paid", "Worker wages paid (cents)"],
    ["wage_arrears", "Unpaid wage balance (cents)"], ["operating_arrears", "Unpaid operating balance (cents)"],
    ["revenue", "Sales revenue (cents)"], ["procurement", "Procurement paid (cents)"],
    ["sales_units", "Units sold"], ["unmet_demand", "Unmet demand (units)"],
    ["waste_units", "Waste (units)"], ["successful_periods", "Trading days"],
    ["failed_periods", "Closed / infeasible days"], ["accounting_ok", "Accounting reconciles"]
  ];
  const ledgerMetrics = [
    ["status", "Decision status"], ["shock_active", "Shock"],
    ["opening_cash", "Opening cash"], ["procurement", "Procurement paid"], ["revenue", "Revenue"],
    ["cost_of_goods_sold", "Cost of goods sold"], ["waste_cost", "Waste cost"],
    ["worker_wages_due", "Wages due"], ["worker_wages_paid", "Wages paid"],
    ["operating_cost_due", "Operating due"], ["operating_cost_paid", "Operating paid"],
    ["economic_result", "Realized FIFO economic result"], ["cumulative_economic_result", "Cumulative realized FIFO economic result"],
    ["closing_cash", "Closing cash"], ["available_cash", "Available cash"],
    ["reserve_balance", "Reserve"], ["wage_arrears", "Wage arrears"],
    ["operating_arrears", "Operating arrears"], ["sales_units", "Sold units"],
    ["unmet_demand", "Unmet units"], ["waste_units", "Waste units"]
  ];

  function renderResults() {
    const result = state.result;
    const days = result.optimized.rows.length;
    $("result-note").textContent = `${days} days · seed ${result.config.seed} · ${result.config.currency} cents. Optimized and fixed-price paths use paired external draws. Differences below are optimized minus fixed; a positive difference is not always a benefit.`;
    const summaryRows = summaryMetrics.map(([key, title]) => {
      const optimized = result.optimized.summary[key];
      const fixed = result.fixed.summary[key];
      return [title, optimized, fixed, typeof optimized === "number" && typeof fixed === "number" ? optimized - fixed : "—"];
    });
    $("summary").replaceChildren(table(["Metric", "Optimized", "Fixed price", "Difference"], summaryRows, "Comparison over simulated days"));
    const limitations = element("details");
    limitations.append(element("summary", "Model assumptions returned by C++"));
    const list = element("ul");
    for (const item of result.limitations || []) list.append(element("li", item));
    limitations.append(list);
    $("summary").append(limitations);
    renderCharts();
    renderLedger();
    $("selected-day").replaceChildren();
    result.optimized.rows.forEach((row) => {
      const option = element("option", String(row.day));
      option.value = row.day;
      $("selected-day").append(option);
    });
    $("selected-day").disabled = false;
    $("selected-day").value = String(days);
    renderDay();
    $("raw-result").textContent = JSON.stringify(result, null, 2);
  }

  function renderLedger() {
    const rows = [];
    for (let index = 0; index < state.result.optimized.rows.length; ++index) {
      for (const mode of modes) {
        const record = state.result[mode].rows[index];
        const inspect = button(String(record.day), () => selectDay(record.day, true));
        inspect.className = "day-button";
        inspect.setAttribute("aria-label", `Inspect day ${record.day}, ${modeNames[mode]}`);
        rows.push([inspect, modeNames[mode], ...ledgerMetrics.map(([key]) => record[key])]);
      }
    }
    const ledger = table(["Day", "Policy", ...ledgerMetrics.map(([, title]) => title)], rows, "Daily policy ledger — money in cents");
    [...ledger.tBodies[0].rows].forEach((row, index) => { row.dataset.day = String(Math.floor(index / 2) + 1); });
    $("ledger").replaceChildren(ledger);
  }

  const svgNS = "http://www.w3.org/2000/svg";
  function svgElement(tag, attributes, text) {
    const node = document.createElementNS(svgNS, tag);
    Object.entries(attributes || {}).forEach(([key, value]) => node.setAttribute(key, String(value)));
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function chart(title, unit, series) {
    const figure = element("figure", undefined, "chart");
    figure.append(element("h3", title));
    const legend = element("div", undefined, "legend");
    for (const item of series) {
      const label = element("span");
      const swatch = element("i", undefined, "swatch");
      swatch.style.borderColor = item.color;
      if (item.dashed) swatch.style.borderTopStyle = "dashed";
      swatch.setAttribute("aria-hidden", "true");
      label.append(swatch, document.createTextNode(item.name));
      legend.append(label);
    }
    figure.append(legend);
    const svg = svgElement("svg", { viewBox: "0 0 700 260", role: "img", "aria-label": `${title}; horizontal axis day; vertical axis ${unit}` });
    svg.append(svgElement("title", {}, `${title}. Inspect a point or use the daily ledger for exact values.`));
    const left = 100, right = 680, top = 26, bottom = 220;
    const values = series.flatMap((s) => s.points.map((p) => p.value)).filter(Number.isFinite);
    const count = state.result.optimized.rows.length;
    const low = Math.min(0, ...values), high = Math.max(0, ...values);
    const span = high - low || 1;
    const yMin = low < 0 ? low - span * 0.05 : 0;
    const yMax = high + span * 0.08;
    const x = (day) => count <= 1 ? (left + right) / 2 : left + (day - 1) * (right - left) / (count - 1);
    const y = (value) => bottom - (value - yMin) * (bottom - top) / (yMax - yMin);
    for (let tick = 0; tick <= 4; ++tick) {
      const value = yMin + (yMax - yMin) * tick / 4;
      svg.append(svgElement("line", { x1: left, y1: y(value), x2: right, y2: y(value), class: "grid-line" }));
      svg.append(svgElement("text", { x: left - 8, y: y(value) + 4, "text-anchor": "end" }, new Intl.NumberFormat("en", { maximumFractionDigits: 0 }).format(value)));
    }
    const days = new Set([1, count]);
    for (let tick = 1; tick < 5; ++tick) days.add(Math.max(1, Math.round(1 + (count - 1) * tick / 5)));
    for (const day of [...days].sort((a, b) => a - b)) svg.append(svgElement("text", { x: x(day), y: bottom + 16, "text-anchor": "middle" }, day));
    svg.append(svgElement("line", { x1: left, y1: top, x2: left, y2: bottom, class: "axis" }),
      svgElement("line", { x1: left, y1: bottom, x2: right, y2: bottom, class: "axis" }),
      svgElement("text", { x: left, y: 14 }, unit),
      svgElement("text", { x: (left + right) / 2, y: 252, "text-anchor": "middle" }, "Day"));
    for (const item of series) {
      let segment = [];
      const flush = () => {
        if (segment.length) svg.append(svgElement("polyline", { points: segment.join(" "), fill: "none", stroke: item.color, "stroke-width": 2, ...(item.dashed ? { "stroke-dasharray": "6 4" } : {}) }));
        segment = [];
      };
      for (const point of item.points) {
        if (!Number.isFinite(point.value)) { flush(); continue; }
        segment.push(`${x(point.day)},${y(point.value)}`);
      }
      flush();
      for (const point of item.points) {
        if (!Number.isFinite(point.value)) continue;
        const marker = svgElement("circle", { cx: x(point.day), cy: y(point.value), r: count > 90 ? 2 : 3,
          fill: item.color, tabindex: 0, class: "chart-point", role: "button",
          "aria-label": `${item.name}, day ${point.day}: ${format(point.value)} ${unit}; inspect day` });
        marker.append(svgElement("title", {}, `${item.name} · day ${point.day}: ${format(point.value)} ${unit}`));
        marker.addEventListener("click", () => selectDay(point.day, true));
        marker.addEventListener("keydown", (event) => {
          if (event.key === "Enter" || event.key === " ") { event.preventDefault(); selectDay(point.day, true); }
        });
        svg.append(marker);
      }
    }
    figure.append(svg);
    return figure;
  }

  function renderCharts() {
    const charts = $("charts");
    charts.replaceChildren();
    const seriesFor = (key) => modes.map((mode) => ({ name: modeNames[mode], color: colors[mode], dashed: mode === "fixed",
      points: state.result[mode].rows.map((row) => ({ day: row.day, value: row[key] })) }));
    for (const [key, title, unit] of [
      ["closing_cash", "Closing cash", "cents"],
      ["cumulative_economic_result", "Cumulative realized FIFO economic result", "cents"],
      ["reserve_balance", "Earmarked reserve", "cents"],
      ["unmet_demand", "Daily unmet demand", "units"],
      ["wage_arrears", "Unpaid wage balance", "cents"],
      ["waste_units", "Daily waste", "units"]
    ]) charts.append(chart(title, unit, seriesFor(key)));
    for (const product of state.result.config.products) {
      for (const [key, title, unit] of [["selected_price", "public price", "cents"], ["closing_stock", "closing stock", "units"]]) {
        charts.append(chart(`${product.label} (${product.sku}): ${title}`, unit, modes.map((mode) => ({
          name: modeNames[mode], color: colors[mode], dashed: mode === "fixed",
          points: state.result[mode].rows.map((row) => ({ day: row.day, value: row.products.find((p) => p.sku === product.sku)?.[key] ?? null }))
        }))));
      }
    }
    charts.append(element("p", "Price gaps mean no price was selected on a closed day. Click a chart point to inspect its day. Exact numbers appear in the ledger and day details.", "note"));
  }

  function selectDay(day, scroll) {
    $("selected-day").value = String(day);
    renderDay();
    if (scroll) $("day-detail").scrollIntoView({ behavior: "smooth", block: "start" });
  }

  function renderDay() {
    if (!state.result) return;
    const day = Number($("selected-day").value);
    const target = $("product-detail");
    target.replaceChildren();
    for (const row of $("ledger").querySelectorAll("tbody tr")) row.classList.toggle("selected", Number(row.dataset.day) === day);
    for (const mode of modes) {
      const record = state.result[mode].rows.find((row) => row.day === day);
      if (!record) continue;
      const section = element("section");
      section.append(element("h3", `${modeNames[mode]} — day ${day}`));
      const description = element("p", `${record.status}: ${record.detail}`,
        record.status === "recommended" ? "status-ok" : "status-failed");
      section.append(description);
      section.append(element("p", `Shock ${record.shock_active ? "active" : "inactive"}; demand factor ${record.demand_factor_bps} bp; cost factor ${record.cost_factor_bps} bp. Expected forecast surplus at replacement cost: ${format(record.expected_worker_surplus)} cents. Scenario forecast surpluses at replacement cost: ${record.scenario_worker_surplus ? record.scenario_worker_surplus.map(format).join(", ") : "—"} cents.`, "note"));
      section.append(element("p", "Realized FIFO economic result uses the historical acquisition cost of sold and wasted stock. Its difference from forecast surplus can reflect cost basis and reserve treatment as well as demand; reserve allocations are transfers within cash.", "note"));
      const productMetrics = [
        ["sku", "SKU"], ["label", "Label"], ["unit_cost", "Replacement cost (cents)"],
        ["opening_stock", "Opening units"], ["requested_units", "Requested units"],
        ["purchased_units", "Purchased units"], ["purchase_cost", "Procurement (cents)"],
        ["selected_price", "Selected price (cents)"], ["realized_noise_bps", "External demand draw (bp)"],
        ["actual_demand", "Actual demand units"], ["sales_units", "Sold units"],
        ["unmet_demand", "Unmet units"], ["waste_units", "Waste units"],
        ["closing_stock", "Closing units"], ["closing_inventory_value", "Closing inventory value (cents)"],
        ["revenue", "Revenue (cents)"], ["cost_of_goods_sold", "FIFO cost sold (cents)"],
        ["waste_cost", "FIFO waste cost (cents)"]
      ];
      const scroll = element("div", undefined, "table-scroll");
      scroll.append(table(productMetrics.map(([, title]) => title), record.products.map((product) => productMetrics.map(([key]) => product[key])), "Product flows"));
      section.append(scroll);
      const accountLabels = {
        expected_worker_surplus: "expected_worker_surplus — forecast surplus at replacement cost (cents)",
        economic_result: "economic_result — realized FIFO economic result (cents)",
        cumulative_economic_result: "cumulative_economic_result — cumulative realized FIFO economic result (cents)"
      };
      const scalarRows = Object.entries(record).filter(([, value]) => value === null || typeof value !== "object")
        .map(([key, value]) => [accountLabels[key] || key, value]);
      const details = element("details");
      details.append(element("summary", "All daily account fields and reconciliation checks"), table(["Field", "Value"], scalarRows));
      section.append(details);
      const exact = element("details");
      exact.append(element("summary", "Complete daily record, including forecasts and inventory reconciliation"),
        element("pre", JSON.stringify(record, null, 2)));
      section.append(exact);
      target.append(section);
    }
  }

  function download(filename, data, mime) {
    const url = URL.createObjectURL(new Blob([data], { type: mime }));
    const link = element("a");
    link.href = url;
    link.download = filename;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  function exportJson() {
    if (!state.result) return;
    const envelope = { export_format: "post-profit-exchange.simulation.v1", configured_horizon: state.runConfig.periods,
      configuration: state.runConfig, result: state.result };
    download(`post-profit-exchange-seed-${state.result.config.seed}-${state.result.config.periods}-days.json`, JSON.stringify(envelope, null, 2), "application/json");
  }

  function csvCell(value) {
    if (value === null || value === undefined) return "";
    let text = typeof value === "object" ? JSON.stringify(value) : String(value);
    // User-provided labels must not become formulas when opened in a spreadsheet.
    if (typeof value === "string" && /^[=+\-@\t\r]/.test(text)) text = "'" + text;
    return '"' + text.replaceAll('"', '""') + '"';
  }
  function exportCsv() {
    if (!state.result) return;
    const rows = [];
    for (const mode of modes) for (const record of state.result[mode].rows) {
      const { products, ...ledger } = record;
      rows.push({ record_type: "daily", policy: mode, currency: state.result.config.currency, ...ledger });
      for (const product of products) rows.push({ record_type: "product", policy: mode, currency: state.result.config.currency, day: record.day,
        ...Object.fromEntries(Object.entries(product).map(([key, value]) => [`product_${key}`, value])) });
    }
    const headers = [...new Set(rows.flatMap((row) => Object.keys(row)))];
    const text = [headers.map(csvCell).join(","), ...rows.map((row) => headers.map((key) => csvCell(row[key])).join(","))].join("\r\n");
    download(`post-profit-exchange-seed-${state.result.config.seed}-${state.result.config.periods}-days.csv`, text, "text/csv;charset=utf-8");
  }

  function validateEditorShape(config) {
    const exactKeys = (object, keys, where) => {
      if (!object || typeof object !== "object" || Array.isArray(object) ||
          Object.keys(object).length !== keys.length || keys.some((key) => !Object.hasOwn(object, key))) throw new Error(`${where} has missing or unknown fields.`);
    };
    exactKeys(config, [...globalFields.map(([key]) => key), "shock", "scenarios", "products"], "Configuration");
    exactKeys(config.shock, shockFields.map(([key]) => key), "Shock");
    if (!Array.isArray(config.scenarios) || config.scenarios.length !== 3) throw new Error("Exactly three scenarios are required.");
    for (const scenario of config.scenarios) exactKeys(scenario, scenarioFields.map(([key]) => key), "Scenario");
    if (!Array.isArray(config.products) || config.products.length < 1 || config.products.length > 3) throw new Error("Use one through three products.");
    for (const product of config.products) {
      exactKeys(product, [...productFields.map(([key]) => key), "candidate_prices"], "Product");
      if (!Array.isArray(product.candidate_prices) || product.candidate_prices.length < 1 || product.candidate_prices.length > 9) throw new Error("Each product needs one through nine candidate prices.");
    }
  }

  $("config-form").addEventListener("submit", (event) => event.preventDefault());
  $("config-form").addEventListener("input", configChanged);
  $("run").addEventListener("click", () => run(false));
  $("step").addEventListener("click", () => run(true));
  $("defaults").addEventListener("click", defaults);
  $("reset").addEventListener("click", () => { clearResults(); status("Results reset. Configuration preserved; next Step starts at day 1."); });
  $("stop").addEventListener("click", () => {
    disposeWorker();
    state.busy = false;
    updateButtons();
    status("Stopped. Last completed result preserved; the next run creates a fresh C++ worker.");
  });
  $("export-json").addEventListener("click", exportJson);
  $("export-csv").addEventListener("click", exportCsv);
  $("selected-day").addEventListener("change", renderDay);
  $("add-product").addEventListener("click", () => mutateConfig((config) => {
    const product = clone(config.products[0]);
    let number = config.products.length + 1;
    while (config.products.some((p) => p.sku === `product-${number}`)) ++number;
    product.sku = `product-${number}`;
    product.label = `Product ${number}`;
    config.products.push(product);
  }));
  $("refresh-json").addEventListener("click", () => {
    try { const config = readConfig(); if (config) $("config-json").value = JSON.stringify(config, null, 2); }
    catch (error) { showError(error.message); }
  });
  $("load-json").addEventListener("click", () => {
    try {
      const config = JSON.parse($("config-json").value);
      validateEditorShape(config);
      buildConfig(config);
      configChanged();
      $("config-form").reportValidity();
    } catch (error) { showError(`Configuration JSON: ${error.message}`); }
  });
  window.addEventListener("beforeunload", disposeWorker);
  updateButtons();
  defaults();
})();
