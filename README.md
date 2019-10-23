# anotherSelfJoin
Remove Self Joins extension for PostgreSQL

## Example
**Query:**

`CREATE TABLE tt(a INT PRIMARY KEY, b TEXT);`
`explain SELECT p.* FROM tt AS p JOIN (SELECT * FROM tt WHERE b ~~ 'a%') AS q ON p.a = q.a;`

**Non-optimized plan:**
                                QUERY PLAN                                 
---------------------------------------------------------------------------
 Nested Loop  (cost=0.15..50.90 rows=6 width=36)
   ->  Seq Scan on tt  (cost=0.00..25.88 rows=6 width=4)
         Filter: (b ~~ 'a%'::text)
   ->  Index Scan using tt_pkey on tt p  (cost=0.15..4.17 rows=1 width=36)
         Index Cond: (a = tt.a)

**Plan with removed self join:**
                       QUERY PLAN
──────────────────────────────────────────────────────
  Seq Scan on tt p  (cost=0.00..25.88 rows=6 width=36)
    Filter: (b ~~ 'a%'::text)
    
## Objectives
We want to replace some types of join of relation with itself by one scan.

## Reasons
Such joins are generated by various ORMs, so from time to time customers ask us to look into this.

## Conditions for join removal
1. Given the join and the restriction clauses, both sides are unique on the same set of columns. That is, if we fix the values of these columns, both sides have at most one matching row.
  a. For each of these columns, we have either
    i) a join clause that equates some expression referencing the outer column to the same expression referencing the same inner column.
    ii) a clause for each relation that equates the same expression referencing the outer and inner column to some other arbitrary expression, possibly a different one for each side. This expression may be a Const or some expression that references a Var of some third relation.
2. All the resulting columns can be calculated using either side of the join. For now, just require that both sides are base relations that refer to the same physical relation.
