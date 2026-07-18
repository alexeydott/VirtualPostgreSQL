WITH changed AS (DELETE FROM guarded RETURNING id)
SELECT id FROM changed;
