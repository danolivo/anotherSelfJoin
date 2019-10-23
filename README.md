# anotherSelfJoin
Remove Self Joins extension for PostgreSQL
## Conditions for join removal
1. Given the join and the restriction clauses, both sides are unique on the same set of columns. That is, if we fix the values of these columns, both sides have at most one matching row.
  a. For each of these columns, we have either
    i) a join clause that equates some expression referencing the outer column to the same expression referencing the same inner column.
    ii) a clause for each relation that equates the same expression referencing the outer and inner column to some other arbitrary expression, possibly a different one for each side. This expression may be a Const or some expression that references a Var of some third relation.
2. All the resulting columns can be calculated using either side of the join. For now, just require that both sides are base relations that refer to the same physical relation.
