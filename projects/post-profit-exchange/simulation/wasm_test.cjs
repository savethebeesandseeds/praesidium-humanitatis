// SPDX-License-Identifier: MIT
// Run with Node: wasm_test.cjs <single-file Emscripten JS> <native simulator>
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const path = require('node:path');
async function main() {
  const factory = require(path.resolve(process.argv[2]));
  const module = await factory();
  const invoke = input => JSON.parse(module.ccall('exchange_run', 'string', ['string'], [input]));
  const native = input => JSON.parse(execFileSync(path.resolve(process.argv[3]), {input, encoding: 'utf8', maxBuffer: 32*1024*1024}));
  const compare = (a, b, field = '') => {
    if (typeof a === 'number' && typeof b === 'number' && (!Number.isInteger(a) || !Number.isInteger(b))) {
      assert(Math.abs(a-b) <= 1e-8 * Math.max(1, Math.abs(a)), `floating mismatch ${field}`);
    } else if (a && b && typeof a === 'object' && typeof b === 'object') {
      assert.deepEqual(Object.keys(a), Object.keys(b), field);
      for (const key of Object.keys(a)) compare(a[key], b[key], `${field}.${key}`);
    } else assert.deepEqual(a, b, field);
  };
  const defaults = invoke('{"op":"defaults"}');
  compare(defaults, native('{"op":"defaults"}'));
  const fixtures = [];
  const add = edit => { const config = structuredClone(defaults.config); edit(config); fixtures.push(JSON.stringify({op:'simulate', config})); };
  add(c => {});
  add(c => {c.periods=365; c.seed=4294967295;});
  add(c => {c.initial_cash=0; c.initial_reserve=0; c.products.forEach(p=>{p.initial_stock=0; p.target_stock=0;});});
  add(c => {c.worker_wages=100000000;});
  add(c => {c.demand_noise_bps=10000; c.scenarios.forEach(s=>s.factor_bps=3000);});
  add(c => {c.shock={start_day:1,end_day:30,demand_factor_bps:0,cost_factor_bps:30000};});
  add(c => {c.products.forEach(p=>{p.unit_cost=0; p.spoilage_bps=10000;});});
  add(c => {c.scenarios[0].probability=.1;c.scenarios[1].probability=.2;c.scenarios[2].probability=.7;});
  add(c => {const p=structuredClone(c.products[1]);p.sku='third';c.products.push(p); c.products.forEach(p=>p.candidate_prices=[160,180,200,220,240,260,280,300,320]);});
  add(c => {c.worker_wages=1.5;});
  add(c => {c.products[0].sku='<img src=x onerror=alert(1)>';});
  fixtures.push('{"op":"defaults","op":"defaults"}', '{"op":"simulate","config":{}}', 'null');
  let count = 0;
  for (const input of fixtures) {
    const result = invoke(input);
    compare(result, native(input));
    assert.deepEqual(invoke(input), result, 'WASM deterministic replay');
    if (result.status === 'ok') for (const mode of ['optimized','fixed']) for (const row of result[mode].rows) {
      for (const key of ['cash_reconciliation_error','equity_reconciliation_error','inventory_reconciliation_error','valuation_reconciliation_error']) assert.equal(row[key], 0);
      assert(row.closing_cash >= row.reserve_balance && row.reserve_balance >= 0);
    }
    count++;
  }
  console.log(`${count} native/WASM fixtures agree; repeatable replay and all exact ledger identities pass.`);
}
main().catch(error => { console.error(error); process.exitCode=1; });
