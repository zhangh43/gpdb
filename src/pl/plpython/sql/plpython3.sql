create language plpython3u;

CREATE FUNCTION import_succeed3() returns text AS $$
  import sys
  import numpy
  return "succeeded, as expected"
$$ LANGUAGE plpython3u;

SELECT import_succeed3();

CREATE TYPE named_value3 AS (
	  name  text,
	  value  integer);

CREATE OR REPLACE FUNCTION make_pair_sets3 (name text)
RETURNS SETOF named_value3 AS $$
  import numpy as np
  return ((name, i) for i in np.arange(1))
$$ LANGUAGE plpython3u;

SELECT * from make_pair_sets3('test');
