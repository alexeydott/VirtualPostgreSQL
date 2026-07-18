CREATE VIRTUAL TABLE temp.remote_places USING VirtualPostgreSQL(
  credential_ref='app/spatial-reader', source=table,
  schema='gis', table='places', mode=ro,
  geometry=ewkb, srid=4326
);

SELECT place_id, geometry FROM remote_places WHERE place_id=1;

-- SpatiaLite output, coordinate swap и automatic SRID transform отсутствуют.
