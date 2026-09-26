# SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
# One horizon, one currency, one public posted price per product. Demand at
# every price/scenario is supplied evidence, not estimated elasticity.
param product_count integer > 0;
param scenario_count integer > 0;
set PRODUCTS := 1..product_count;
set SCENARIOS := 1..scenario_count;
param candidate_count {PRODUCTS} integer > 0;
param hold_candidate {i in PRODUCTS} integer >= 1, <= candidate_count[i];
set CHOICES := {i in PRODUCTS, k in 1..candidate_count[i]};
param probability {SCENARIOS} > 0;
param unit_cost {PRODUCTS} >= 0;
param previous_price {PRODUCTS} > 0;
param affordability_ceiling {PRODUCTS} > 0;
param inventory {PRODUCTS} integer >= 0;
param max_change_bp {PRODUCTS} >= 0, <= 10000;
param price {CHOICES} > 0;
param forecast_units {CHOICES, SCENARIOS} integer >= 0;
param worker_wage_floor > 0;
param operating_cost >= 0;
param reserve_floor >= 0;
param funding_balance;
param coverage_credit >= 0;
param liquidity_buffer >= 0;

# Only alternatives allowed by the existing per-product protections may
# displace another price. Derive this from the raw inputs independently of
# the C++ verifier; no caller-supplied admissibility or dominance flags.
param locally_admissible {(i,k) in CHOICES} binary :=
    if price[i,k] <= affordability_ceiling[i]
       and abs(price[i,k] - previous_price[i]) * 10000
           <= previous_price[i] * max_change_bp[i]
       and (if funding_balance > 0 then price[i,k] - previous_price[i]
            else if funding_balance < 0 then previous_price[i] - price[i,k]
            else abs(price[i,k] - previous_price[i])) <= 0
       and (forall {s in SCENARIOS} forecast_units[i,k,s] <= inventory[i])
       and (funding_balance >= 0 or price[i,k] <= previous_price[i]
            or (forall {s in SCENARIOS}
                (price[i,k] - unit_cost[i]) * forecast_units[i,k,s]
                >= (previous_price[i] - unit_cost[i])
                    * forecast_units[i,hold_candidate[i],s]))
    then 1 else 0;

var choose {CHOICES} binary;
subject to OnePublicPrice {i in PRODUCTS}:
    sum {k in 1..candidate_count[i]} choose[i,k] = 1;
# Do not retain a more expensive price solely to avoid earning surplus when
# a cheaper allowed offer serves at least as many units and contributes at
# least as much in every supplied scenario. This preserves protected coverage
# under replacement. The balance objective still ranks the remaining choices.
subject to AffordableProvision {(i,k) in CHOICES:
        exists {j in 1..candidate_count[i]:
            locally_admissible[i,j] = 1 and price[i,j] < price[i,k]}
        (forall {s in SCENARIOS}
            forecast_units[i,j,s] >= forecast_units[i,k,s]
            and (price[i,j] - unit_cost[i]) * forecast_units[i,j,s]
                >= (price[i,k] - unit_cost[i]) * forecast_units[i,k,s])}:
    choose[i,k] = 0;
subject to Affordability {(i,k) in CHOICES}:
    (price[i,k] - affordability_ceiling[i]) * choose[i,k] <= 0;
subject to PriceStability {(i,k) in CHOICES}:
    (abs(price[i,k] - previous_price[i]) * 10000
        - previous_price[i] * max_change_bp[i]) * choose[i,k] <= 0;
subject to FeedbackDirection {(i,k) in CHOICES}:
    (if funding_balance > 0 then price[i,k] - previous_price[i]
     else if funding_balance < 0 then previous_price[i] - price[i,k]
     else abs(price[i,k] - previous_price[i])) * choose[i,k] <= 0;
subject to SafeIncrease {(i,k) in CHOICES, s in SCENARIOS:
        funding_balance < 0 and price[i,k] > previous_price[i]}:
    ((previous_price[i] - unit_cost[i]) * forecast_units[i,hold_candidate[i],s]
     - (price[i,k] - unit_cost[i]) * forecast_units[i,k,s]) * choose[i,k] <= 0;
subject to InventoryBound {i in PRODUCTS, s in SCENARIOS}:
    sum {k in 1..candidate_count[i]} forecast_units[i,k,s] * choose[i,k] <= inventory[i];
var scenario_surplus {SCENARIOS};
subject to ScenarioAccounting {s in SCENARIOS}:
    scenario_surplus[s] = sum {(i,k) in CHOICES} (price[i,k] - unit_cost[i])
        * forecast_units[i,k,s] * choose[i,k]
    - worker_wage_floor - operating_cost - reserve_floor;
subject to ProtectedCoverage {s in SCENARIOS}:
    scenario_surplus[s] + coverage_credit + liquidity_buffer >= 0;
var absolute_balance {SCENARIOS} >= 0;
subject to BalancePositive {s in SCENARIOS}:
    absolute_balance[s] >= funding_balance + scenario_surplus[s];
subject to BalanceNegative {s in SCENARIOS}:
    absolute_balance[s] >= -funding_balance - scenario_surplus[s];
minimize ExpectedAbsoluteBalance:
    sum {s in SCENARIOS} probability[s] * absolute_balance[s];
