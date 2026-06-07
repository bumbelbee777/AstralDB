SELECT COUNT(*) AS n
FROM orders o
JOIN customers c ON c.cust_id = o.cust_id
WHERE JSON_EXTRACT(o.metadata, 'payment_method') = 'credit_card'
  AND JSON_EXTRACT(c.profile, 'active') = 'true'
  AND XML_VALID(c.xml_data) = '1'
  AND o.review_text MATCH 'good';
