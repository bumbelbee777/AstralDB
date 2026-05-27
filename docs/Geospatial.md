# Geospatial (2D and 3D)

AstralDB exposes **2D geometry** (`POINT`, `POLYGON`, GeoJSON/WKT) and **3D triangle meshes** (`MESH`, glTF import/export) as first-class cell types with SQL builtins.

## 2D geometry

| Area | Builtins (sample) |
|------|-------------------|
| Points | `ST_POINT`, `ST_X`, `ST_Y`, `ST_AS_TEXT`, `ST_DISTANCE`, `ST_DISTANCE_SPHERICAL` |
| Polygons | `ST_POLYGON`, `ST_POLYGON_WKT`, `ST_GEOM_AREA`, `ST_GEOM_PERIMETER`, `ST_GEOM_CONTAINS`, `ST_GEOM_INTERSECTS`, `ST_GEOM_UNION`, `ST_GEOM_BUFFER`, `ST_GEOM_SIMPLIFY` |
| Exchange | `ST_GEOJSON_EXPORT`, `ST_GEOJSON_IMPORT` |

Column types: `POINT`, `POLYGON`, or `GEOMETRY(…)` variants.

## 3D mesh

| Area | Builtins (sample) |
|------|-------------------|
| Define / IO | `ST_MESH`, `ST_MESH_IMPORT_GLTF`, `ST_MESH_EXPORT_GLTF` |
| CSG / repair | `ST_MESH_SEW`, `ST_MESH_UNION`, `ST_MESH_INTERSECTION`, `ST_MESH_DIFFERENCE` |
| Metrics | `ST_MESH_VOLUME`, `ST_MESH_SURFACE_AREA`, `ST_MESH_CENTROID`, `ST_MESH_BOUNDS` |
| Transform | `ST_MESH_TRANSLATE`, `ST_MESH_SCALE`, `ST_MESH_ROTATE` |

Column type: `MESH` or `GEOMETRY(MESH)`.

## Terrain / elevation (related)

`ST_POINTZ`, `ST_ELEVATION`, `ST_DEM_SAMPLE`, `ST_TERRAIN_SLOPE` support raster-style elevation grids alongside vector geometry.

## Examples

| Script | Focus |
|--------|--------|
| [`examples/sql_geospatial.sql`](../examples/sql_geospatial.sql) | Points, distances, WKT |
| [`examples/sql_geospatial_polygon.sql`](../examples/sql_geospatial_polygon.sql) | Polygons, predicates, booleans |
| [`examples/sql_geospatial_mesh.sql`](../examples/sql_geospatial_mesh.sql) | Meshes, glTF, CSG |
| [`examples/sql_geospatial_terrain.sql`](../examples/sql_geospatial_terrain.sql) | DEM sampling, slope |

## Related

- [`docs/Overview.md`](Overview.md) — dialect and type overview
- [`examples/torture_omnibus.sql`](../examples/torture_omnibus.sql) — blended geo + analytics workload
