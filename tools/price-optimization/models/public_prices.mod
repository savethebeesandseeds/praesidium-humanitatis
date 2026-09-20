# SPDX-License-Identifier: LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
# One horizon, one currency, one public posted price per product. Demand at
# every price/scenario is supplied evidence, not estimated elasticity.
param product_count integer > 0;
param scenario_count integer > 0;
set PRODUCTS := 1..product_count;
set SCENARIOS := 1..scenario_count;
param candidate_count {PRODUCTS} integer > 0;
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

var choose {CHOICES} binary;
subject to OnePublicPrice {i in PRODUCTS}:
    sum {k in 1..candidate_count[i]} choose[i,k] = 1;
subject to Affordability {(i,k) in CHOICES}:
    (price[i,k] - affordability_ceiling[i]) * choose[i,k] <= 0;
subject to PriceStability {(i,k) in CHOICES}:
    (abs(price[i,k] - previous_price[i]) * 10000
        - previous_price[i] * max_change_bp[i]) * choose[i,k] <= 0;
subject to InventoryBound {i in PRODUCTS, s in SCENARIOS}:
    sum {k in 1..candidate_count[i]} forecast_units[i,k,s] * choose[i,k] <= inventory[i];
subject to ProtectedCoverage {s in SCENARIOS}:
    sum {(i,k) in CHOICES} (price[i,k] - unit_cost[i])
        * forecast_units[i,k,s] * choose[i,k]
    >= worker_wage_floor + operating_cost + reserve_floor;
maximize ExpectedWorkerSurplus:
    sum {s in SCENARIOS} probability[s] *
        (sum {(i,k) in CHOICES} (price[i,k] - unit_cost[i])
            * forecast_units[i,k,s] * choose[i,k]
         - worker_wage_floor - operating_cost - reserve_floor);
