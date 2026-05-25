# Vector Format (Binary)

All vector files are little-endian and begin with `rdb_vec_hdr_t`.

## Header (`rdb_vec_hdr_t`)
- `magic` (u32): 0x54424452 ("RDBT")
- `version` (u16): 1
- `kind` (u16): 1=KV, 2=TS
- `count` (u32): number of entries

## KV Entry (kind=1)
- `op` (u8): 1=set, 2=delete, 3=get
- `klen` (u8)
- `vlen` (u16)
- `key` (klen bytes)
- `value` (vlen bytes) only when `op` == set

## TS Entry (kind=2)
- `time` (u32)
- `dlen` (u16)
- `data` (dlen bytes)

## Usage
These vectors can be replayed on embedded targets to reproduce workloads and validate functional behavior, GC behavior, and CRC handling.
