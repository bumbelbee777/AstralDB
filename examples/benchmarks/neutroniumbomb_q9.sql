-- Query 9: Semistructured
SELECT
    cust_id,
    JSON_EXTRACT(profile, '$.preferences.theme') AS theme,
    JSON_EXTRACT(profile, '$.preferences.notifications.email') AS email_notify,
    XML_EXTRACT(xml_data, '/customer/settings/language') AS language,
    REGEXP_EXTRACT(bio, '([A-Z][a-z]+ [A-Z][a-z]+)') AS proper_name,
    LENGTH(bio) AS bio_length,
    TEXT_RANK(bio, 'important keywords') AS relevance
FROM customers
WHERE JSON_EXTRACT(profile, '$.active') = true
  AND XML_VALID(xml_data) = 1
  AND bio MATCH 'SELECT|INSERT|UPDATE|DELETE'
ORDER BY relevance DESC
LIMIT 100000;

