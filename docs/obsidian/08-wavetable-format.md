# Wavetable format `.tbl`

Binary, **little-endian**. It holds the table's frames already **band-limited (mipmaps)**, so
they play without aliasing across the whole keyboard. The structure was validated on the app's
25 tables: the last offset plus its size gives exactly the file size **[P]**.

What this document says comes from analysing the data, not from the loader's code. The fields
marked **[?]** can be confirmed by decompiling the function that reads the file.

## Layout

```
offset  type        field
0x00    u32         file_size           total file size
0x04    u32         version             = 1
0x08    u32         num_frames          frames in the table (2..9 observed)
0x0C    u32         num_mips            = 44
0x10    u32         mips_per_octave     = 4                          [I]
0x14    f32         gain                = 0.945 in all of them        [?] headroom
0x18    u32         unknown_a           = 1                           [?]
0x1C    u32         num_pairs           1 or 11 observed              [?]
0x20    f32[2*num_pairs] pairs          (a, b) pairs                  [?]
....    f32[num_frames]  frame_scale    int16 → float scale per frame
....    MipEntry[num_mips]               40 bytes each
....    sample data (int16)
```

`header_size = 32 + 8·num_pairs + 4·num_frames`. The entries start there.

### `MipEntry` (10 × u32 = 40 bytes)

| # | Field | Example (mip 0) | Notes |
|---|---|---|---|
| 0 | `index` | 0..43 | |
| 1 | `harmonic_class` | 1024, 512, … 1 | Halves every 4 mips **[I]** |
| 2 | `step` | 1, 2, 2, 4, 4 … | Always equal to `table_len / harmonic_class` **[P]** |
| 3 | `table_len` | 1024 … 4 | Samples per cycle, a power of 2 |
| 4 | `log2_len` | 10 … 2 | |
| 5 | `len_plus_guard` | `table_len + 1` | One guard sample for interpolation |
| 6 | `phase_mask` | `(table_len << 16) - 1` | Fixed-point phase with 16 fractional bits |
| 7 | `magic` | `0xFEEDFACE` | Sanity marker, always the same |
| 8 | `data_offset` | from the start of the file | |
| 9 | `reserved` | 0 | Probably an in-memory pointer, filled in on load **[I]** |

### Data of each mip

At `data_offset` there are `num_frames` consecutive blocks (frame-major), each of
`table_len + 1` **int16** samples. The last one repeats the first (a guard for linear
interpolation) **[P]**.

```
sample_float = int16 · frame_scale[frame]
```

Example in `Analog.tbl` **[P]**:
- Frame 0 is a sine with an exact peak of 32767 × 3.0514e-5 = 1.0.
- Frames 1 and 2 peak at 1.224 and frame 3 at 0.712. The per-frame scale keeps the level
  difference between waveforms.

### Band limiting

Measured on frame 2 of `Analog.tbl`, harmonics present per mip (approximate) **[P]**:

| Mip | 0 | 1 | 2 | 3 | 4 | 8 | 12 |
|---|---|---|---|---|---|---|---|
| table_len | 1024 | 1024 | 1024 | 1024 | 1024 | 512 | 512 |
| highest harmonic | ~464 | ~407 | ~350 | ~293 | ~243 | ~122 | ~61 |

Each mip has about 2^(1/4) fewer harmonics than the previous one: 4 mips per octave, one every
3 semitones **[I]**. The likely selection rule **[I]** is: with frequency `f` at sample rate
`sr`, use the first mip whose highest harmonic × `f` < `sr/2`. The exact formula NS2 uses, and
whether it crossfades between neighbouring mips, is **[?]**.

## Reading (pseudocode)

```text
read u32 file_size, version, num_frames, num_mips, mips_per_octave
read f32 gain; read u32 unknown_a, num_pairs
read f32 pairs[2*num_pairs]
read f32 frame_scale[num_frames]
for m in 0..num_mips-1:
    read MipEntry e; assert e.magic == 0xFEEDFACE
    for f in 0..num_frames-1:
        samples[m][f] = int16[e.table_len+1] at e.data_offset + f*(e.table_len+1)*2
                        × frame_scale[f]
```

## For a port

- Size: between 59 KB (`Hum`, 2 frames) and 289 KB (`Resonant`) per table. All 25 together take
  4.6 MB **[P]**. They fit in the ESP32-S3 N16R8's flash as they are, as int16.
- On the ESP32, copy to SRAM only the mips of the active table that are actually used in the
  range of notes being played.
- For the web, convert to `Float32Array` on load or read the int16 data directly in the
  AudioWorklet.
- Mind the licence: these tables are NS2 content. For something distributable you have to
  generate your own tables in the same format.
