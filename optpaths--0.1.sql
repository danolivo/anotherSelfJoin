CREATE TABLE test (a INT UNIQUE, b INT);
INSERT INTO test (a, b) VALUES (1, NULL);
INSERT INTO test (a, b) VALUES (2, 0);
-- EXPLAIN SELECT p.* FROM test p, test q where q.a = p.a;

