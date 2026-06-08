-- Query 8: MCTS + NFP
WITH customer_features AS (
    SELECT
        o.cust_id,
        c.lifetime_value,
        COUNT(*) AS order_count,
        AVG(o.amount) AS avg_amount,
        STDDEV_POP(o.amount) AS amount_volatility
    FROM orders o
    JOIN customers c ON o.cust_id = c.cust_id
    GROUP BY o.cust_id, c.lifetime_value
),
policy_search AS (
    SELECT
        cust_id,
        MCTS_SEARCH(
            'L[4]:0.25,0.25,0.25,0.25',
            'L[4]:1,1,1,1',
            1000,
            1.414
        ) AS optimal_action
    FROM customer_features
    WHERE lifetime_value > 1000
),
wealth_distribution AS (
    SELECT
        NFP_MACRO_MARCH(
            g0, grid, drift, diffusion,
            neural_corr, aggK, kStar, barrierA, dt, steps
        ) AS g_final,
        NFP_MACRO_MOMENTS(g_final, grid) AS moments
    FROM policy_search, nfp_params
)
SELECT
    moments.mass,
    moments.mean,
    moments.variance,
    moments.gini,
    optimal_action
FROM wealth_distribution
ORDER BY moments.gini DESC
LIMIT 1000;

