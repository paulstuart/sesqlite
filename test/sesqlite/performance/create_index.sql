# Create indices
#
BEGIN;

  CREATE INDEX i1a ON t1(a);
  CREATE INDEX i1b ON t1(b);
  CREATE INDEX i1c ON t1(c);

COMMIT;