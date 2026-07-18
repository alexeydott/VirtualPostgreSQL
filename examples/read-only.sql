-- credential_ref разрешается host provider; secret не хранится в SQL.
CREATE VIRTUAL TABLE temp.remote_orders USING VirtualPostgreSQL(
  credential_ref='app/reporting',
  source=table,
  schema='reporting',
  table='orders',
  mode=ro,
  key_columns='order_id'
);

SELECT order_id, created_at
FROM remote_orders
WHERE order_id IN (101, 102)
ORDER BY order_id
LIMIT 10;

CREATE VIRTUAL TABLE temp.recent_orders USING VirtualPostgreSQL(
  credential_ref='app/reporting',
  source=query,
  query_profile='recent_orders_v1',
  key_columns='order_id',
  query_materialization=memory,
  query_indexes='by_customer=customer_id'
);

SELECT * FROM recent_orders WHERE customer_id=7;
