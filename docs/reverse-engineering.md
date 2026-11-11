# Reverse-engineering notes

This document explains how the format in [file-format.md](file-format.md) was
recovered, so that each claim can be checked against the binary.

* **Target:** `C:\PaintToolSAI\sai.exe`, PaintTool SAI Ver.1 1.2.6-Beta.3
  (from `sysinfo.txt`), PE32 x86, SHA-256
  `68ecc23e03d8c1843b4b0460dae8597612c3054daa138d6e8a98a13e5a00090a`.
  The PE references `sai.pdb` (GUID `34facd8e-51c2-4919-a886-1042bc950879`),
  but no symbols ship with it, so every function name below is the
  analysis tool's default `FUN_<address>`.
* **Tools:** REA with the Ghidra 12.1.4 backend for decompilation and
  cross-references, plus small Python scripts that parse the PE sections,
  find immediate operands and dump `.rdata` tables.
* **Validation data:** two documents saved by SAI (512×512, from 2017 and
  2024), decoded by an independent Python prototype and then by libsai.

## How the code was located

1. A string search found the source file name `fio_sai.c` and the VFS paths
   `/canvas`, `/laytbl`, `/layers/%08x`, `/subtbl`, `/sublayers/%08x` and
   `/thumbnail` at VA `0x51e2ac`–`0x51e340`, alongside `BM32` and
   `VFS error: %d`.
2. Scanning `.text` for those addresses as 32-bit immediates gave the
   referencing instructions. Mapping them to their containing functions
   produced:
   * `FUN_00465140`: writes `/canvas`, `/laytbl`, `/subtbl` and each layer
     (open mode 2 = create). This is the **save** routine.
   * `FUN_00465be0`: reads them (open mode 3). This is the **load** routine.
   * `FUN_00462350`: writes `/thumbnail`.
3. The callees of these routines lead to the VFS module (`0x4bd720`–
   `0x4c1360`).

## Function map

| Address      | Role |
|--------------|------|
| `0x465140`   | save document (canvas chunks, tables, folders, layers, thumbnail) |
| `0x465be0`   | load document; dispatches canvas versions 0/1/2/0x10 |
| `0x463340`   | write one layer stream (header, blend, chunks, tiles or strokes) |
| `0x463c50`   | read one layer stream; FourCC → blend index mapping |
| `0x4648b0`   | read a layer in the legacy canvas format |
| `0x462e90`   | select the tile writer by layer type (3 → `0x462990`; 4/6/7 → `0x462ac0`) |
| `0x462ef0`   | select the tile reader by layer type (3 → `0x462b70`; 4/6/7 → `0x462dd0`) |
| `0x462990`   | write a raster tile: 4 streams from the plane at +0, then 4 from the plane at +0x1000 |
| `0x462ac0`   | write a mask tile: 2 streams, stride 2 |
| `0x4aefc0`   | RLE encoder, stride 4 |
| `0x4af110`   | RLE encoder, stride 2 |
| `0x4af080` / `0x4af1d0` | the matching RLE decoders |
| `0x462350`   | write thumbnail: `w, h, "BM32"`, BGRA rows with A forced to 0xFF |
| `0x46e2c0`   | write a chunk header: `{u32 tag, u32 size}` |
| `0x46e2f0`   | sum of a `{size, offset}` field table → chunk size |
| `0x46e390`   | write the struct fields listed in a field table |
| `0x46e310`   | read the struct fields listed in a field table |
| `0x4c0f80`   | create or open a VFS: callbacks, header "VFS-0001", initial state |
| `0x4c05a0`   | open: journal replay, header magic check |
| `0x4c04a0`   | legacy `VFS-0000` upgrade (switches the table cipher) |
| `0x4bd720` / `0x4bd860` | table block encrypt / decrypt (`(seed & 0x1ff) == 0`) |
| `0x4bd9c0` / `0x4bdb00` | legacy variants (`(seed & 0xfff) == 0`) |
| `0x4be2a0` / `0x4be170` | data block encrypt / decrypt |
| `0x4be3e0`   | data block checksum (words 0..1023) |
| `0x4be3b0`   | table block checksum (words 1..1023) |
| `0x4be410`   | get a table block (decrypt and verify) |
| `0x4be4f0`   | get a data block (decrypt and verify against its table checksum) |
| `0x4be610`   | flush dirty blocks: recompute checksums, encrypt, write |
| `0x4be760`   | create a table block and its free list |
| `0x4be830`   | allocate blocks from the free list |
| `0x4be8d0`   | free a chain back to the free list |
| `0x4bef50`   | directory lookup (64-byte entries, `_strnicmp` over 32 characters) |
| `0x4bf270`   | path resolution and open (modes 2/3/0x200) |
| `0x4bed80` / `0x4bfe60` | write into a file stream (follows and extends the block chain) |
| `0x4bfc90`   | read from a file stream |
| `0x4c0080`   | seek or skip in a file stream (skips unknown chunks) |
| `0x4bea40`   | write back a directory entry and refresh the header |

## Key facts and where they come from

### Key table

`FUN_00465140` and `FUN_00465be0` both build the VFS callback struct with
`key = *(ctx + 0x1b8) + 0x400`. The only store to `ctx + 0x1b8` is at
`0x405235`:

```
mov ecx, [0x536160]        ; ecx = 0x5188c0
mov [ebx + 0x1b8], ecx
```

So the document key is `0x5188c0 + 0x400 = 0x518cc0`. The system VFS worker
`FUN_004c6cc0` passes `&DAT_005194c0` as its key, which is the second table
shipped in `src/key.c`. A correct table-0 checksum when decrypting the
sample files with `0x518cc0` confirms the choice.

### Ciphers

The four cipher functions are short and were transcribed directly. The data
cipher feeds the **ciphertext** word back as the next seed (`prev = c`) in
both directions, and the table cipher does the same. With the wrong
direction the checksums would not match. They match on every block of both
samples.

### Field tables

The chunk bodies are produced by `FUN_0046e390(vfs, file, struct, table)`.
It walks a zero-terminated array of `{i32 size, i32 offset}` pairs; a
negative offset writes padding. These arrays are stored in `.rdata`:

| VA         | Chunk       | Pairs |
|------------|-------------|-------|
| `0x51df60` | `reso`      | {4,332} {2,336} {2,338} |
| `0x51df7c` | `lyid`      | {4,340} |
| `0x51df88` | `stid`      | {4,344} |
| `0x51df94` | `name`      | {256,372} |
| `0x51dfa0` | layer header| {4,32} {4,40} {4,184} {4,188} {4,192} {4,196} {4,76} {1,81} {1,83} {1,84} {1,85} {1,86} |
| `0x51e004` | `lorg`      | {4,176} {4,180} |
| `0x51e018` | `texp`      | {2,92} {1,91} |
| `0x51e02c` | `peff`      | {1,94} {1,95} {1,96} |
| `0x51e048` | `vmrk`      | {1,82} |
| `0x51e090` | stroke      | {4,56} {4,60} {4,64} … |

The blend-mode FourCCs are the string `"ssapmron lumnrcsrevo dda busbsdanibc"`
at VA `0x53a974`, indexed by `layer->blend_index * 4`.

## Validation performed

* **Tables and checksums.** Every table and data block of both samples
  decrypts with a matching checksum.
* **Header.** Both samples carry `VFS-0001`. The free-list head, table span
  and used-block count agree with the block chains.
* **Tree.** Both samples contain `#01.<id>`, `canvas`, `laytbl`, `layers/`
  and `thumbnail`.
* **Tiles.** All tiles decode to exactly 1024 samples per stream, and the
  stream ends exactly at the end of the layer file.
* **Premultiplication and 16-bit layout.** In 76,588 non-transparent
  pixels, combining stream *n* (high byte) with stream *n+4* (low byte)
  never gives a color channel above alpha. Low bytes are always multiples of
  4, and opaque pixels have a low alpha byte of `0xFC`, i.e. `A = 0xFFFC`.
* **Border offset.** The correlation between decoded layer alpha and the
  darkness of SAI's thumbnail peaks at offset 8 (r = 1.000 and 0.997).
* **Flattened output vs. SAI's thumbnail.** Downscaling libsai's composite
  over white differs from SAI's own thumbnail by 0.22/255 per channel on
  average.
* **Round trip.** `saitool resave` produces the same block layout and file
  size. After decryption, every block is identical except for the header's
  random words and the entry timestamps. Table entries match field by field
  (free list, chain links, header and table markers). Layer streams,
  `/canvas`, `/laytbl` and `/thumbnail` are byte-identical.

## Open questions

These items are preserved on round trip but not interpreted:

* What the struct fields at +76 and +86 of the layer header, `lorg`, `wsrc`,
  `stid`, `vmrk` and `lmfl` bit 1 mean in the UI.
* The UI names of layer types 4 and 7. They take the mask code path, but no
  sample contains them.
* The full stroke record layout of linework layers (type 5), and the
  meaning of `0x10` in the per-stroke header.
* Canvas versions 0, 1 and 2 (field tables at `0x51e054`, `0x51e0e8` and
  `0x51e140`) and the legacy layer reader `FUN_004648b0`.
* Whether SAI opens files produced by libsai has not been verified in the
  GUI. The files reproduce SAI's on-disk structure exactly, as described
  above.
