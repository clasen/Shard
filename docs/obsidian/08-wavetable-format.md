# Formato de wavetable `.tbl`

Binario, **little-endian**. Contiene los frames de la tabla ya **filtrados por banda
(mipmaps)** para tocar sin aliasing en todo el teclado. La estructura se validó en
las 25 tablas de la app: el último offset más su tamaño da exactamente el tamaño del
archivo **[P]**.

Lo que dice este documento sale del análisis de los datos, no del código del loader.
Los campos marcados **[?]** se confirman decompilando la función que lee el archivo.

## Layout

```
offset  tipo        campo
0x00    u32         file_size           tamaño total del archivo
0x04    u32         version             = 1
0x08    u32         num_frames          frames de la tabla (2..9 observado)
0x0C    u32         num_mips            = 44
0x10    u32         mips_per_octave     = 4                          [I]
0x14    f32         gain                = 0.945 en todas              [?] headroom
0x18    u32         unknown_a           = 1                           [?]
0x1C    u32         num_pairs           1 u 11 observado              [?]
0x20    f32[2*num_pairs] pairs          pares (a, b)                  [?]
....    f32[num_frames]  frame_scale    escala int16 → float por frame
....    MipEntry[num_mips]               40 bytes cada uno
....    datos de muestras (int16)
```

`header_size = 32 + 8·num_pairs + 4·num_frames`. Las entradas empiezan ahí.

### `MipEntry` (10 × u32 = 40 bytes)

| # | Campo | Ejemplo (mip 0) | Notas |
|---|---|---|---|
| 0 | `index` | 0..43 | |
| 1 | `harmonic_class` | 1024, 512, … 1 | Se divide por 2 cada 4 mips **[I]** |
| 2 | `step` | 1, 2, 2, 4, 4 … | Siempre igual a `table_len / harmonic_class` **[P]** |
| 3 | `table_len` | 1024 … 4 | Muestras por ciclo, potencia de 2 |
| 4 | `log2_len` | 10 … 2 | |
| 5 | `len_plus_guard` | `table_len + 1` | Una muestra de guarda para interpolar |
| 6 | `phase_mask` | `(table_len << 16) - 1` | Fase en punto fijo de 16 bits fraccionarios |
| 7 | `magic` | `0xFEEDFACE` | Marca de sanidad, siempre igual |
| 8 | `data_offset` | desde el inicio del archivo | |
| 9 | `reserved` | 0 | Probablemente un puntero en memoria, que se rellena al cargar **[I]** |

### Datos de cada mip

En `data_offset` hay `num_frames` bloques consecutivos (frame-major), cada uno de
`table_len + 1` muestras **int16**. La última repite la primera (guarda para la
interpolación lineal) **[P]**.

```
muestra_float = int16 · frame_scale[frame]
```

Ejemplo en `Analog.tbl` **[P]**:
- El frame 0 es un seno de pico exacto 32767 × 3.0514e-5 = 1.0.
- Los frames 1 y 2 tienen pico 1.224 y el frame 3, 0.712. La escala por frame conserva
  la diferencia de nivel entre formas de onda.

### Banda limitada

Medido en el frame 2 de `Analog.tbl`, armónicos presentes por mip (aproximado) **[P]**:

| Mip | 0 | 1 | 2 | 3 | 4 | 8 | 12 |
|---|---|---|---|---|---|---|---|
| table_len | 1024 | 1024 | 1024 | 1024 | 1024 | 512 | 512 |
| armónico más alto | ~464 | ~407 | ~350 | ~293 | ~243 | ~122 | ~61 |

Cada mip tiene unos 2^(1/4) menos armónicos que el anterior: 4 mips por octava, uno
cada 3 semitonos **[I]**. La regla de selección probable **[I]** es: con frecuencia `f`
a sample rate `sr`, usar el primer mip cuyo armónico más alto × `f` < `sr/2`. La fórmula
exacta que usa NS2, y si hace crossfade entre mips vecinos, **[?]**.

## Lectura (pseudocódigo)

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

## Para un port

- Tamaño: entre 59 KB (`Hum`, 2 frames) y 289 KB (`Resonant`) por tabla. Las 25
  juntas pesan 4.6 MB **[P]**. Entran en la flash del ESP32-S3 N16R8 tal cual,
  como int16.
- En el ESP32 conviene copiar a SRAM solo los mips de la tabla activa que realmente se
  usan en el rango de notas tocado.
- Para web, se puede pasar a `Float32Array` al cargar o leer int16 directamente en el
  AudioWorklet.
- Ojo con la licencia: estas tablas son contenido de NS2. Para algo distribuible hay
  que generar tablas propias con el mismo formato.
