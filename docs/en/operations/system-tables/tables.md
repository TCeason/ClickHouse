---
description: 'System table containing metadata of each table that the server knows
  about.'
keywords: ['system table', 'tables']
slug: /operations/system-tables/tables
title: 'system.tables'
---

# system.tables

Contains metadata of each table that the server knows about.

[Detached](../../sql-reference/statements/detach.md) tables are not shown in `system.tables`.

[Temporary tables](../../sql-reference/statements/create/table.md#temporary-tables) are visible in the `system.tables` only in those session where they have been created. They are shown with the empty `database` field and with the `is_temporary` flag switched on.

Columns:

- `database` ([String](../../sql-reference/data-types/string.md)) — The name of the database the table is in.

- `name` ([String](../../sql-reference/data-types/string.md)) — Table name.

- `uuid` ([UUID](../../sql-reference/data-types/uuid.md)) — Table uuid (Atomic database).

- `engine` ([String](../../sql-reference/data-types/string.md)) — Table engine name (without parameters).

- `is_temporary` ([UInt8](../../sql-reference/data-types/int-uint.md)) - Flag that indicates whether the table is temporary.

- `data_paths` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Paths to the table data in the file systems.

- `metadata_path` ([String](../../sql-reference/data-types/string.md)) - Path to the table metadata in the file system.

- `metadata_modification_time` ([DateTime](../../sql-reference/data-types/datetime.md)) - Time of latest modification of the table metadata.

- `metadata_version` ([Int32](../../sql-reference/data-types/int-uint.md)) - Metadata version for ReplicatedMergeTree table, 0 for non ReplicatedMergeTree table.

- `dependencies_database` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Database dependencies.

- `dependencies_table` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Table dependencies ([materialized views](/sql-reference/statements/create/view#materialized-view) the current table).

- `create_table_query` ([String](../../sql-reference/data-types/string.md)) - The query that was used to create the table.

- `engine_full` ([String](../../sql-reference/data-types/string.md)) - Parameters of the table engine.

- `as_select` ([String](../../sql-reference/data-types/string.md)) - `SELECT` query for view.

- `parameterized_view_parameters` ([Array](../../sql-reference/data-types/array.md) of [Tuple](../../sql-reference/data-types/tuple.md)) — Parameters of parameterized view.

- `partition_key` ([String](../../sql-reference/data-types/string.md)) - The partition key expression specified in the table.

- `sorting_key` ([String](../../sql-reference/data-types/string.md)) - The sorting key expression specified in the table.

- `primary_key` ([String](../../sql-reference/data-types/string.md)) - The primary key expression specified in the table.

- `sampling_key` ([String](../../sql-reference/data-types/string.md)) - The sampling key expression specified in the table.

- `storage_policy` ([String](../../sql-reference/data-types/string.md)) - The storage policy:

  - [MergeTree](../../engines/table-engines/mergetree-family/mergetree.md#table_engine-mergetree-multiple-volumes)
  - [Distributed](/engines/table-engines/special/distributed)

- `total_rows` ([Nullable](../../sql-reference/data-types/nullable.md)([UInt64](../../sql-reference/data-types/int-uint.md))) - Total number of rows, if it is possible to quickly determine exact number of rows in the table, otherwise `NULL` (including underlying `Buffer` table).

- `total_bytes` ([Nullable](../../sql-reference/data-types/nullable.md)([UInt64](../../sql-reference/data-types/int-uint.md))) - Total number of bytes, if it is possible to quickly determine exact number of bytes for the table on storage, otherwise `NULL` (does not includes any underlying storage).

  - If the table stores data on disk, returns used space on disk (i.e. compressed).
  - If the table stores data in memory, returns approximated number of used bytes in memory.

- `total_bytes_uncompressed` ([Nullable](../../sql-reference/data-types/nullable.md)([UInt64](../../sql-reference/data-types/int-uint.md))) - Total number of uncompressed bytes, if it's possible to quickly determine the exact number of bytes from the part checksums for the table on storage, otherwise `NULL` (does not take underlying storage (if any) into account).

- `lifetime_rows` ([Nullable](../../sql-reference/data-types/nullable.md)([UInt64](../../sql-reference/data-types/int-uint.md))) - Total number of rows INSERTed since server start (only for `Buffer` tables).

- `lifetime_bytes` ([Nullable](../../sql-reference/data-types/nullable.md)([UInt64](../../sql-reference/data-types/int-uint.md))) - Total number of bytes INSERTed since server start (only for `Buffer` tables).

- `comment` ([String](../../sql-reference/data-types/string.md)) - The comment for the table.

- `has_own_data` ([UInt8](../../sql-reference/data-types/int-uint.md)) — Flag that indicates whether the table itself stores some data on disk or only accesses some other source.

- `loading_dependencies_database` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Database  loading dependencies (list of objects which should be loaded before the current object).

- `loading_dependencies_table` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Table loading dependencies (list of objects which should be loaded before the current object).

- `loading_dependent_database` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Dependent loading database.

- `loading_dependent_table` ([Array](../../sql-reference/data-types/array.md)([String](../../sql-reference/data-types/string.md))) - Dependent loading table.

The `system.tables` table is used in `SHOW TABLES` query implementation.

**Example**

```sql
SELECT * FROM system.tables LIMIT 2 FORMAT Vertical;
```

```text
Row 1:
──────
database:                   base
name:                       t1
uuid:                       81b1c20a-b7c6-4116-a2ce-7583fb6b6736
engine:                     MergeTree
is_temporary:               0
data_paths:                 ['/var/lib/clickhouse/store/81b/81b1c20a-b7c6-4116-a2ce-7583fb6b6736/']
metadata_path:              /var/lib/clickhouse/store/461/461cf698-fd0b-406d-8c01-5d8fd5748a91/t1.sql
metadata_modification_time: 2021-01-25 19:14:32
dependencies_database:      []
dependencies_table:         []
create_table_query:         CREATE TABLE base.t1 (`n` UInt64) ENGINE = MergeTree ORDER BY n SETTINGS index_granularity = 8192
engine_full:                MergeTree ORDER BY n SETTINGS index_granularity = 8192
as_select:                  SELECT database AS table_catalog
partition_key:
sorting_key:                n
primary_key:                n
sampling_key:
storage_policy:             default
total_rows:                 1
total_bytes:                99
lifetime_rows:              ᴺᵁᴸᴸ
lifetime_bytes:             ᴺᵁᴸᴸ
comment:
has_own_data:               0
loading_dependencies_database: []
loading_dependencies_table:    []
loading_dependent_database:    []
loading_dependent_table:       []

Row 2:
──────
database:                   default
name:                       53r93yleapyears
uuid:                       00000000-0000-0000-0000-000000000000
engine:                     MergeTree
is_temporary:               0
data_paths:                 ['/var/lib/clickhouse/data/default/53r93yleapyears/']
metadata_path:              /var/lib/clickhouse/metadata/default/53r93yleapyears.sql
metadata_modification_time: 2020-09-23 09:05:36
dependencies_database:      []
dependencies_table:         []
create_table_query:         CREATE TABLE default.`53r93yleapyears` (`id` Int8, `febdays` Int8) ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 8192
engine_full:                MergeTree ORDER BY id SETTINGS index_granularity = 8192
as_select:                  SELECT name AS catalog_name
partition_key:
sorting_key:                id
primary_key:                id
sampling_key:
storage_policy:             default
total_rows:                 2
total_bytes:                155
lifetime_rows:              ᴺᵁᴸᴸ
lifetime_bytes:             ᴺᵁᴸᴸ
comment:
has_own_data:               0
loading_dependencies_database: []
loading_dependencies_table:    []
loading_dependent_database:    []
loading_dependent_table:       []
```
