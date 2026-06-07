# Column shard format (ASHD v1)

AstralDB column shards use a single **v1** on-disk layout. There is no v2 magic and no migration layer; layout evolution reuses the 8-byte header and repurpose flag bytes.

## Header (8 bytes)

| Offset | Size | Field |
|--------|------|--------|
| 0 | 4 | Magic `ASHD` |
| 4 | 1 | Compression mode (`ColumnShardCompression`: Off=0, Lz4=1, Lzx=2) |
| 5 | 1 | Layout flags low byte |
| 6 | 1 | Layout flags mid byte |
| 7 | 1 | Layout flags high byte |

### Layout flags (`AshdLayoutFlags`)

- `TypedChunks` (0x01): payload is typed chunk catalog + encoded bytes (not newline text)
- `PartialAggFooter` (0x02): optional partial-aggregate blob at end of shard
- `ChunkIndex` (0x04): typed chunk index precedes encoded chunk bytes

## Payload (breaking v1 evolution)

### Legacy text mode (default)

Plain newline-delimited column values, optionally LZ4/LZX compressed as a single blob after the header.

### Typed mode (`TypedChunks | ChunkIndex`)

```
[chunk count: u32 LE]
repeat count:
  [name len: u8][name bytes]
  [payload offset: u32 LE]
  [payload size: u32 LE]
  [encoding: u8]   (ColumnEncoding)
  [row count: u32 LE]
[encoded chunk bytes…]
[optional partial-agg footer when PartialAggFooter is set]
```

Chunk bytes may be page-compressed via `ChunkCompressor` (LZ4/LZX on encoded bytes).

## Readers

- `ColumnShardStream`: legacy text + full-column decompress
- `LsmBypassReader`: mmap shard, decode chunk index, selective per-column decompress
