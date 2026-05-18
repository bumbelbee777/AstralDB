# Graph / GQL

AstralDB exposes a **property-graph layer** on ordinary tables: register a graph, build an in-memory **adjacency index**, then run **GQL-style** queries. Traversals use adjacency lists (not repeated SQL self-joins), which keeps neighbor expansion, shortest paths, and PageRank fast enough for typical social and financial workloads.

Runnable scripts: **`examples/graph_social.sql`**, **`examples/graph_finance.sql`**, **`examples/graph_analytics.sql`**.

---

## Data model

| Piece | Role |
|-------|------|
| **Vertex table** | One row per node; a single id column keys the graph. |
| **Edge table** | Directed arcs `(src → dst)`; optional **label** and **weight** columns. |
| **Named graph** | Catalog entry + adjacency built at `CREATE GRAPH`. |
| **Projection** | Filtered view of a base graph (same tables, subset of edges by label). |

New edge rows on the base edge table are **appended incrementally** to the adjacency index (and to projections when the label matches). Bulk deletes/updates on edges may require re-running `CREATE GRAPH`.

---

## DDL

```sql
CREATE GRAPH social
  VERTEX TABLE users (id)
  EDGE TABLE follows (src, dst, kind)
  WEIGHT (weight_col)   -- optional, for WEIGHTED shortest path
  UNDIRECTED;           -- optional: treat arcs as bidirectional

DROP GRAPH social;
```

### Graph projection

Materialize a derived graph without copying tables—only the adjacency filter changes:

```sql
CREATE GRAPH PROJECTION payments FROM ledger EDGE WHERE e.kind = 'payment';
```

Use the projection name in all subsequent `GRAPH … IN payments` statements.

---

## GQL surface (95% toolkit)

### One-hop match

```sql
GRAPH MATCH (a)-[e]->(b) IN social INTO edges;
GRAPH MATCH (a)<-[e]-(b) IN social INTO inbound;   -- reverse direction
GRAPH MATCH (a)-[e]->(b) IN ledger WHERE e.kind = 'payment' INTO payments;
```

| Column | Meaning |
|--------|---------|
| `src_id`, `dst_id` | Arc endpoints |
| `edge_label` | Present when the graph has a label column |

### Variable-length paths

```sql
-- All paths of 1–3 hops from Alice (id 1):
GRAPH MATCH (a)-[e*1..3]->(b) FROM 1 IN social INTO paths;

-- Exactly 2 hops (omit FROM on small graphs only):
GRAPH MATCH (a)-[e*2]->(b) IN social INTO two_hop;

-- Unbounded upper bound defaults to 64 hops (see Limits):
GRAPH MATCH (a)-[e*]->(b) FROM 1 IN social INTO reach;
```

| Column | Meaning |
|--------|---------|
| `start_id` | Anchor vertex |
| `end_id` | Reachable vertex |
| `path_length` | Hop count |
| `path` | Comma-separated vertex ids along one path |

On large graphs, **always use `FROM start_id`**—without it, AstralDB expands from every vertex (capped at 4096 sources).

### Multi-hop traverse (reachability)

```sql
GRAPH TRAVERSE FROM 1 IN social DEPTH 3 BFS INTO reach;
GRAPH TRAVERSE FROM 101 IN ledger DEPTH 4 DFS INTO from_alice;
```

Result: `vertex_id`, `depth`.

### Shortest path

```sql
-- Unweighted (fewest hops):
GRAPH SHORTEST PATH FROM 1 TO 5 IN social INTO route;

-- Weighted (uses CREATE GRAPH WEIGHT column):
GRAPH SHORTEST PATH FROM 101 TO 103 WEIGHTED IN ledger INTO cheapest;
```

| Column | Meaning |
|--------|---------|
| `found` | `1` if a path exists |
| `path_length` | Hops (unweighted) or total weight (weighted) |
| `path` | Full path as comma-separated ids |
| `position`, `vertex_id` | One row per vertex on the path |

### PageRank

```sql
GRAPH PAGERANK IN social DAMPING 0.85 ITERATIONS 25 INTO ranks;
SELECT vertex_id, rank FROM ranks ORDER BY rank DESC;
```

Classic power iteration over the registered adjacency (out-degree normalization). Defaults: damping **0.85**, **20** iterations.

---

## When to use graphs vs recursive SQL

| Use case | Prefer |
|----------|--------|
| Neighbor expansion, feed fan-out, counterparty reach | `GRAPH TRAVERSE` or `GRAPH MATCH … *` |
| Shortest route, settlement path, wire hops | `GRAPH SHORTEST PATH` |
| Influence, importance, hub scoring | `GRAPH PAGERANK` |
| Relationship-type slice (payments only, follows only) | `CREATE GRAPH PROJECTION` |
| Hierarchical reporting with aggregates per level | `WITH RECURSIVE` |

---

## Limits

| Guard | Value |
|-------|--------|
| Max traverse / var-length hops | 64 |
| Var-length without `FROM` | ≤ 4096 source vertices |
| Var-length result rows | 2,000,000 |
| PageRank iterations | 10,000 |

Graph catalog and adjacency metadata are **durable**: they are written into the main database snapshot (`<<<ASTRAL_DB_GRAPHS>>>` trailer) and logged on WAL as `GR|`, `GD|`, and `GP|` lines. `BEGIN` / `COMMIT` / `ROLLBACK` participate in the same snapshot + WAL path as tables and views.

Edge rows remain in ordinary tables; inserts update in-memory adjacency incrementally via `GraphsByEdgeTable_`.

---

## Cypher-style `MATCH`

OpenCypher-like patterns (no separate client):

```sql
MATCH (a)-[e]->(b) IN social WHERE e.kind = 'follow' RETURN a, b INTO edges;
MATCH (a)-[e*1..3]->(b) FROM 1 IN social INTO paths;
```

`RETURN` is optional; results land in the table named after `INTO` (same columns as `GRAPH MATCH`).

---

## Graph optimizer

After bytecode generation, `RunGraphOptimizerPipeline` runs modest passes when graph opcodes are present: hop normalization/clamping, redundant `GRAPH_DROP`+`GRAPH_REGISTER` elision, then the general SQL optimizer.

---

## Bytecode

| Opcode | Purpose |
|--------|---------|
| `GRAPH_REGISTER` | Primary graph + adjacency |
| `GRAPH_REGISTER_PROJECTION` | Label-filtered projection |
| `GRAPH_DROP` | Remove graph (and its projections) |
| `GRAPH_TRAVERSE` | BFS/DFS reachability |
| `GRAPH_MATCH` | One-hop or variable-length paths |
| `GRAPH_SHORTEST_PATH` | Unweighted BFS or weighted Dijkstra |
| `GRAPH_PAGERANK` | Power iteration |

Disassemble with `astraldb -db` on compiled `.abc` like any other statement.
