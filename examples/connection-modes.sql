-- Preferred: the host resolves this opaque name through a registered provider.
CREATE VIRTUAL TABLE temp.orders_by_reference USING VirtualPostgreSQL(
  credential_ref='app/reporting',
  source=table,
  schema='reporting',
  table='orders',
  mode=ro
);

-- libpq loads the named service from its protected service configuration.
CREATE VIRTUAL TABLE temp.orders_by_service USING VirtualPostgreSQL(
  service='reporting_ro',
  source=table,
  schema='reporting',
  table='orders',
  mode=ro
);

-- The process environment supplies bounded VPS_PROFILE_REPORTING_* fields.
CREATE VIRTUAL TABLE temp.orders_by_profile USING VirtualPostgreSQL(
  profile='reporting',
  source=table,
  schema='reporting',
  table='orders',
  mode=ro
);

-- Compatibility mode only. This illustrative connstr contains no password.
-- TEMP avoids persistence, but the SQL remains visible inside this process.
CREATE VIRTUAL TABLE temp.orders_by_connstr USING VirtualPostgreSQL(
  connstr='host=db.example.invalid port=5432 dbname=reporting user=reporting_reader sslmode=verify-full channel_binding=require',
  source=table,
  schema='reporting',
  table='orders',
  mode=ro
);
