<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# Events Extension for DuckDB

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/events.html)
[![v1.5 build](https://github.com/Query-farm/events/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/events/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

A DuckDB extension that hooks into database events and sends JSON-formatted notifications to external programs via stdin. Created by [Query.Farm](https://query.farm).

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/events](https://query.farm/products/extensions/events)**

## Installation

```sql
INSTALL events FROM community;
LOAD events;
```

## Development

For instructions on building the extension from source and running its tests, see [BUILDING.md](BUILDING.md).
