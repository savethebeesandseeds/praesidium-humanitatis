// SPDX-License-Identifier: MIT
// Run with Node: wasm_test.cjs <single-file Emscripten JS> <native simulator>
const assert = require('node:assert/strict');
const {execFileSync} = require('node:child_process');
const path = require('node:path');
const modes = ['optimized', 'fixed'];
async function main() {
  const factory = require(path.resolve(process.argv[2]));
  const module = await factory();
  const invoke = input => JSON.parse(module.ccall('exchange_run', 'string', ['string'], [input]));
  const native = input => JSON.parse(execFileSync(path.resolve(process.argv[3]), {input, encoding:'utf8', maxBuffer:128*1024*1024}));
  const compare = (a, b, field = '') => {
    if (typeof a === 'number' && typeof b === 'number' && (!Number.isInteger(a) || !Number.isInteger(b))) {
      assert(Math.abs(a-b) <= 1e-8 * Math.max(1, Math.abs(a)), 'floating mismatch '+field);
    } else if (a && b && typeof a === 'object' && typeof b === 'object') {
      assert.deepEqual(Object.keys(a), Object.keys(b), field);
      for (const key of Object.keys(a)) compare(a[key], b[key], field+'.'+key);
    } else assert.deepEqual(a, b, field);
  };
  const defaults = invoke('{"op":"defaults"}');
  assert.equal(defaults.schema_version, 'exchange.sim.v3');
  assert.equal(defaults.status, 'ok');
  compare(defaults, native('{"op":"defaults"}'));
  const fixtures = [];
  const add = (name, edit, verify = () => {}, history) => {
    const config = structuredClone(defaults.config); edit(config);
    const command = {op:'simulate', config}; if (history) command.history = history;
    fixtures.push({name, input:JSON.stringify(command), status:'ok', verify});
  };
  const reject = (name, edit, history) => {add(name, edit, () => {}, history); fixtures.at(-1).status='error';};
  const simple = config => {
    Object.assign(config, {periods:5, initial_cash:100000, initial_reserve:0, reserve_target:0,
      reserve_contribution:0, procurement_budget:0, worker_wages:1, operating_cost:0});
    Object.assign(config.shock, {start_day:0, end_day:0}); config.products.splice(1);
    Object.assign(config.products[0], {candidate_prices:[200], reference_price:200, fixed_price:200,
      initial_stock:100, target_stock:100, spoilage_bps:0});
  };
  const history = stockout => ({schema_version:'exchange.history.v1', observations:
    Array.from({length:7}, (_, i) => ({day:i-7, sku:'bread', price:200, sales_units:12+i, stockout}))});
  add('defaults', () => {});
  add('365 periods', c => {
    simple(c); c.periods=365; c.seed=4294967295; c.procurement_budget=15000; c.products[0].unit_cost=0;
  }, result => {
    for (const mode of modes) {
      assert.equal(result[mode].rows.length, 365); assert.equal(result[mode].summary.terminal_status, 'completed');
    }
  });
  add('opening insolvency', c => {c.initial_cash=0; c.initial_reserve=0;}, result => {
    for (const mode of modes) {
      assert.equal(result[mode].rows.length, 0); assert.equal(result[mode].summary.terminal_day, 0);
      assert(result[mode].events.some(e => e.type === 'insolvency_declared' && e.day === 0));
    }
  });
  add('no visitors exhaust cash', c => {
    simple(c); c.initial_cash=2500; c.worker_wages=1000; c.consumers.visit_probability_bps=0;
  }, result => {
    for (const mode of modes) {
      const p=result[mode];
      assert.equal(p.rows.length, 3); assert.equal(p.summary.terminal_status, 'insolvent');
      assert.equal(p.summary.terminal_day, 3); assert.equal(p.summary.closing_cash, 0);
      assert.equal(p.summary.wage_arrears, 500); assert.equal(p.summary.sales_units, 0);
      assert(p.rows.every(row => row.visitors === 0 && row.no_visit === 100));
      assert(p.events.some(e => e.type === 'assurance_requested'));
      assert(p.events.some(e => e.type === 'insolvency_declared' && e.day === 3));
    }
  });
  add('low cash continuity keeps trading', c => {
    simple(c); c.periods=1; c.initial_cash=100; c.worker_wages=1000; c.forecast.prior_sigma_units=100;
    Object.assign(c.consumers, {visit_probability_bps:10000, need_probability_bps:10000,
      max_units_per_product:1, budget_min:1000, budget_max:1000}); c.products[0].base_demand=100;
  }, result => {
    for (const mode of modes) {
      const p=result[mode], row=p.rows[0];
      assert.equal(row.status, 'continuity'); assert.equal(row.optimization_status, 'infeasible');
      assert.equal(row.expected_absolute_balance, null); assert.equal(row.products[0].selected_price, 200);
      assert(row.sales_units > 0 && row.closing_cash > 0); assert.equal(p.summary.terminal_status, 'completed');
      assert(p.events.some(e => e.type === 'assurance_requested' && e.reason === 'forecast_coverage_shortfall'));
    }
  });
  add('imported warm history', c => {simple(c); c.periods=1;}, result => {
    assert.deepEqual(result.history, history(false));
    for (const mode of modes) {
      const forecast=result[mode].rows[0].products[0].forecast_before_sale;
      assert.equal(forecast.eligible_history, 7); assert.equal(forecast.warmed_up, true);
    }
  }, history(false));
  add('stock-censored history cannot warm forecast', c => {simple(c); c.periods=1;}, result => {
    for (const mode of modes) {
      const forecast=result[mode].rows[0].products[0].forecast_before_sale;
      assert.equal(forecast.eligible_history, 0); assert.equal(forecast.warmed_up, false);
      assert(result[mode].summary.products[0].forecast_diagnostics.stock_censored >= 7);
    }
  }, history(true));
  add('real stockout is censored', c => {
    simple(c); c.periods=1; c.products[0].initial_stock=1; c.products[0].target_stock=1;
    c.products[0].base_demand=100;
    Object.assign(c.consumers, {visit_probability_bps:10000, need_probability_bps:10000,
      max_units_per_product:1, budget_min:1000, budget_max:1000});
  }, result => {
    for (const mode of modes) {
      const p=result[mode].rows[0].products[0];
      assert.equal(p.sales_units, 1); assert(p.unmet_demand > 0);
      assert.equal(p.forecast_update.stock_censored, true); assert.equal(p.forecast_update.eligible, false);
      assert.equal(p.forecast_update.reason, 'stock_censored_no_learning');
    }
  });
  add('12 products with bounded candidate grid', c => {
    simple(c); c.periods=3; c.products=Array.from({length:12}, (_, i) => ({...structuredClone(c.products[0]),
      sku:'product-'+i, label:'Product '+i, candidate_prices:[180,200]}));
  }, result => {assert.equal(result.optimized.rows[0].products.length, 12);});
  add('arrival and cost shock', c => {c.shock={start_day:1,end_day:30,demand_factor_bps:0,cost_factor_bps:30000};}, result => {
    for (const mode of modes) assert(result[mode].rows.every(row => row.visitors === 0 && row.shock_active));
  });
  add('free procurement and complete daily waste', c => {c.products.forEach(p => {p.unit_cost=0; p.spoilage_bps=10000;});});
  add('non-default scenario weights', c => {c.scenarios[0].probability=.1; c.scenarios[1].probability=.2; c.scenarios[2].probability=.7;});
  add('price explanation neutral direction and equal-contribution safeguard', c => {
    simple(c); c.periods=2; c.consumers.potential_visitors=0; c.products[0].base_demand=0;
    Object.assign(c.products[0], {candidate_prices:[160,200,220], max_change_bps:10000});
    c.forecast.prior_sigma_units=0; c.forecast.sigma_multiplier_bps=0;
  }, result => {
    const rows=result.optimized.rows;
    const candidate=(row,price)=>row.products[0].candidate_forecasts.find(c=>c.price===price).price_comparison;
    assert.deepEqual(candidate(rows[0],160).exclusion_reasons,['operating balance price direction violated for bread']);
    const guarded=candidate(rows[1],220);
    assert.equal(guarded.affordable_alternative_price,200); assert.equal(guarded.admissible,false);
    assert.deepEqual(guarded.exclusion_reasons,['affordable alternative 1 preserves scenario provision and contribution for bread']);
    assert.equal(guarded.hypothetical_balance_score,rows[1].expected_absolute_balance);
  });
  add('literal HTML-like product data', c => {c.periods=1; c.products[0].sku='<img src=x onerror=alert(1)>';});
  reject('float money', c => {c.worker_wages=1.5;});
  reject('obsolete aggregate noise setting', c => {c.demand_noise_bps=10000;});
  reject('unknown consumer field', c => {c.consumers.latent_future_sales=100;});
  reject('boolean visitor count', c => {c.consumers.potential_visitors=true;});
  reject('unknown forecast model', c => {c.forecast.model='tft';});
  reject('too many products', c => {c.products=Array.from({length:13}, (_, i) => ({...c.products[0],sku:'p'+i}));});
  reject('duplicate history day and SKU', c => {simple(c);}, {
    schema_version:'exchange.history.v1', observations:[history(false).observations[0],history(false).observations[0]]});
  reject('future history observation', c => {simple(c);}, {
    schema_version:'exchange.history.v1', observations:[{...history(false).observations[0],day:0}]});
  reject('unknown history SKU', c => {simple(c);}, {
    schema_version:'exchange.history.v1', observations:[{...history(false).observations[0],sku:'missing'}]});
  for (const input of ['{"op":"defaults","op":"defaults"}', '{"op":"simulate","config":{}}', 'null', '[1e10000]'])
    fixtures.push({name:'malformed command', input, status:'error', verify:()=>{}});
  for (const fixture of fixtures) {
    const result=invoke(fixture.input);
    assert.equal(result.status, fixture.status, fixture.name+': '+JSON.stringify(result.error));
    compare(result, native(fixture.input), fixture.name);
    assert.deepEqual(invoke(fixture.input), result, fixture.name+': deterministic replay');
    if (result.status === 'ok') verifyAccounts(result);
    fixture.verify(result);
  }
  console.log(fixtures.length+' native/WASM v3 fixtures agree; consumer/history, continuity, insolvency, replay and ledger checks pass.');
}
function verifyAccounts(result) {
  assert.equal(result.schema_version, 'exchange.sim.v3');
  assert.equal(result.objective, 'operating_balance_tracking');
  assert.equal(result.objective_version, 2);
  for (const mode of modes) {
    const p=result[mode];
    assert(p.rows.length <= result.config.periods);
    assert(['completed','insolvent','model_error'].includes(p.summary.terminal_status));
    if (p.summary.terminal_status === 'completed') assert.equal(p.rows.length, result.config.periods);
    let cash=result.config.initial_cash, cumulative=0, funding=0;
    for (const [index,row] of p.rows.entries()) {
      assert.equal(row.day, index+1);
      for (const key of ['cash_reconciliation_error','equity_reconciliation_error','inventory_reconciliation_error','valuation_reconciliation_error']) assert.equal(row[key], 0);
      assert.equal(row.opening_cash, cash);
      assert.equal(row.closing_cash, cash-row.procurement+row.revenue-row.worker_wages_paid-row.operating_cost_paid);
      assert.equal(row.economic_result, row.revenue-row.cost_of_goods_sold-row.waste_cost-row.worker_wages_due-row.operating_cost_due);
      cumulative+=row.economic_result; funding+=row.economic_result-row.reserve_requirement;
      assert.equal(row.funding_balance_after, funding);
      assert.equal(result.config.initial_cash+p.summary.initial_inventory_value+cumulative,
        row.closing_cash+row.closing_inventory_value-row.wage_arrears-row.operating_arrears);
      assert(row.closing_cash >= row.reserve_balance && row.reserve_balance >= 0);
      assert.equal(row.visitors+row.no_visit, result.config.consumers.potential_visitors);
      assert(row.no_purchase >= 0 && row.no_purchase <= row.visitors);
      assert.equal(row.consumers.spending, row.revenue);
      assert.equal(row.consumers.opening_budget-row.consumers.remaining_budget, row.revenue);
      for (const product of row.products) {
        assert.equal(product.opening_stock+product.purchased_units, product.sales_units+product.waste_units+product.closing_stock);
        assert.equal(product.opening_inventory_value+product.purchase_cost, product.cost_of_goods_sold+product.waste_cost+product.closing_inventory_value);
        assert.equal(product.actual_demand, product.sales_units+product.unmet_demand);
        assert.equal(product.desired_units, product.budget_rejected_units+product.actual_demand);
        assert.equal(product.closing_lots.reduce((n,lot)=>n+lot.units*lot.unit_cost,0), product.closing_inventory_value);
        if (row.status === 'continuity') assert.equal(product.selected_price, product.previous_price);
        const published=product.candidate_forecasts.filter(candidate=>candidate.price_comparison.published);
        assert.equal(published.length,1,'one public price is identified in each explanation');
        assert.equal(published[0].price,product.selected_price);
        const evidence=published[0].price_comparison;
        if (row.status === 'recommended') {
          assert.equal(evidence.admissible,true); assert.deepEqual(evidence.exclusion_reasons,[]);
          assert.deepEqual(evidence.scenario_funding_balance,row.scenario_funding_balance);
          assert(Math.abs(evidence.hypothetical_balance_score-row.expected_absolute_balance)<=1e-8*Math.max(1,row.expected_absolute_balance));
        } else {
          assert.equal(row.expected_absolute_balance,null,'continuity does not acquire a certified score through diagnostics');
          assert.equal(row.expected_funding_balance,null);
          assert.equal(typeof evidence.hypothetical_balance_score,'number');
          assert.equal(evidence.admissible,false,'continuity publication remains distinguishable from a valid recommendation');
        }
        for (const candidate of product.candidate_forecasts) {
          const comparison=candidate.price_comparison;
          assert.equal(comparison.admissible,comparison.exclusion_reasons.length===0);
          assert.equal(comparison.scenario_contribution.length,result.config.scenarios.length);
          assert.equal(comparison.scenario_funding_balance.length,result.config.scenarios.length);
          candidate.saleable_scenario_units.forEach((quantity,index)=>{
            // Integer money has one zero; JS can produce -0 for a negative margin times zero sales.
            assert.equal(comparison.scenario_contribution[index],0+(candidate.price-product.unit_cost)*quantity);
          });
        }
      }
      cash=row.closing_cash;
    }
    assert.equal(p.summary.closing_cash, cash); assert.equal(p.summary.economic_result, cumulative);
    assert.equal(p.summary.funding_balance, funding);
    assert.equal(new Set(p.events.map(event=>event.id)).size, p.events.length);
    for (const event of p.events) {
      assert.equal(event.schema_version, 'exchange.assurance.v1'); assert.equal(event.policy, mode);
      assert.equal(event.settlement_status, 'unfunded_request');
      assert(['assurance_requested','insolvency_declared'].includes(event.type));
      assert(event.day >= 0 && event.required_support >= 0);
    }
  }
  for (let day=0; day<Math.min(result.optimized.rows.length,result.fixed.rows.length); day++) {
    assert.equal(result.optimized.rows[day].visitors,result.fixed.rows[day].visitors,'paired arrival draws');
    assert.equal(result.optimized.rows[day].consumers.opening_budget,result.fixed.rows[day].consumers.opening_budget,'paired consumer budget draws');
  }
}
main().catch(error => {console.error(error); process.exitCode=1;});
