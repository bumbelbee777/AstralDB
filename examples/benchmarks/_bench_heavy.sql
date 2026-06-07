-- 3-way star join + GROUP BY + SIMD JSON/XML/REGEXP filters (warehouse bulk fast path).

SELECT

    c.country,

    p.category,

    COUNT(*) AS order_count,

    SUM(o.amount) AS total_amount

FROM customers c

JOIN orders o ON c.cust_id = o.cust_id

JOIN products p ON o.prod_id = p.prod_id

WHERE o.order_date >= '2024-01-01'

  AND JSON_EXTRACT(o.metadata, 'payment_method') = 'credit_card'

  AND JSON_EXTRACT(c.profile, 'active') = 'true'

  AND XML_VALID(c.xml_data) = '1'

  AND o.review_text REGEXP 'good|excellent|recommend'

GROUP BY c.country, p.category

ORDER BY total_amount DESC

LIMIT 1000;

