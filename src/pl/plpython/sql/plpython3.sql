create language plpython3u;

CREATE FUNCTION import_succeed() returns text AS $$
  import sys
  import numpy
  return "succeeded, as expected"
$$ LANGUAGE plpython3u;

SELECT import_succeed();

CREATE TYPE named_value AS (
	  name  text,
	  value  integer);

CREATE OR REPLACE FUNCTION make_pair_sets (name text)
RETURNS SETOF named_value AS $$
  import numpy as np
  return ((name, i) for i in np.arange(1))
$$ LANGUAGE plpython3u;

SELECT * from make_pair_sets('test');
