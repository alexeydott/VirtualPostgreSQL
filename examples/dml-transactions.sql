CREATE VIRTUAL TABLE temp.remote_items USING VirtualPostgreSQL(
  credential_ref='app/writer', source=table,
  schema='inventory', table='items', mode=rw,
  optimistic_lock=column, version_column='version'
);

BEGIN;
SAVEPOINT before_change;
UPDATE remote_items
SET quantity=quantity+1, version=version+1
WHERE item_id=42;
ROLLBACK TO before_change;
RELEASE before_change;
COMMIT;

-- __vps_omit отличает server DEFAULT от explicit NULL при INSERT.
INSERT INTO remote_items(item_id, quantity, __vps_omit)
VALUES(1001, NULL, 'quantity');
