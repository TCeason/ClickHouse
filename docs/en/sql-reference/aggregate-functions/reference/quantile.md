---
description: 'Computes an approximate quantile of a numeric data sequence.'
sidebar_position: 170
slug: /sql-reference/aggregate-functions/reference/quantile
title: 'quantile'
---

# quantile

Computes an approximate [quantile](https://en.wikipedia.org/wiki/Quantile) of a numeric data sequence.

This function applies [reservoir sampling](https://en.wikipedia.org/wiki/Reservoir_sampling) with a reservoir size up to 8192 and a random number generator for sampling. The result is non-deterministic. To get an exact quantile, use the [quantileExact](/sql-reference/aggregate-functions/reference/quantileexact#quantileexact) function.

When using multiple `quantile*` functions with different levels in a query, the internal states are not combined (that is, the query works less efficiently than it could). In this case, use the [quantiles](../../../sql-reference/aggregate-functions/reference/quantiles.md#quantiles) function.

Note that for an empty numeric sequence, `quantile` will return NaN, but its `quantile*` variants will return either NaN or a default value for the sequence type, depending on the variant.

**Syntax**

```sql
quantile(level)(expr)
```

Alias: `median`.

**Arguments**

- `level` — Level of quantile. Optional parameter. Constant floating-point number from 0 to 1. We recommend using a `level` value in the range of `[0.01, 0.99]`. Default value: 0.5. At `level=0.5` the function calculates [median](https://en.wikipedia.org/wiki/Median).
- `expr` — Expression over the column values resulting in numeric [data types](/sql-reference/data-types), [Date](/sql-reference/data-types/date) or [DateTime](/sql-reference/data-types/datetime).

**Returned value**

- Approximate quantile of the specified level.

Type:

- [Float64](/sql-reference/data-types/float) for numeric data type input.
- [Date](/sql-reference/data-types/date) if input values have the `Date` type.
- [DateTime](/sql-reference/data-types/datetime) if input values have the `DateTime` type.

**Example**

Input table:

```text
┌─val─┐
│   1 │
│   1 │
│   2 │
│   3 │
└─────┘
```

Query:

```sql
SELECT quantile(val) FROM t
```

Result:

```text
┌─quantile(val)─┐
│           1.5 │
└───────────────┘
```

**See Also**

- [median](/sql-reference/aggregate-functions/reference/median)
- [quantiles](/sql-reference/aggregate-functions/reference/quantiles#quantiles)
