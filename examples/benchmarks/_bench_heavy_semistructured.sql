-- JSON + XML + regex + ORDER BY + LIMIT on semistructured bulk data.
SELECT
    o.order_id,
    o.cust_id,
    JSON_EXTRACT(o.metadata, 'payment_method') AS payment_method,
    o.review_text
FROM orders o
JOIN customers c ON c.cust_id = o.cust_id
WHERE JSON_EXTRACT(o.metadata, 'payment_method') = 'credit_card'
  AND JSON_EXTRACT(c.profile, 'active') = 'true'
  AND XML_VALID(c.xml_data) = '1'
  AND o.review_text REGEXP 'good|excellent|recommend'
ORDER BY o.order_id DESC
LIMIT 1000;
