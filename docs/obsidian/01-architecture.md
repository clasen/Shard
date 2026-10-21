# Arquitectura

## Resumen

- Sintetizador polifónico de hasta **32 voces** **[M]**, con unísono de 1 a 4 voces por
  nota **[P]**.
- Cada voz tiene **3 osciladores** y cada uno puede usar uno de **7 métodos de síntesis**:
  Analog, Wavetable, PD, FM, NanoSaw, Noise y Sample **[M][P]**.
- Cada voz tiene **2 filtros**, en serie, en paralelo o como par estéreo **[M][P]**.
- Hay **5 envolventes** y **5 LFOs** por voz **[M]**. Solo la de amplitud tiene una
  conexión fija.
- La **matriz de modulación** tiene 24 slots **[P]**, con multiplicador opcional por macro.
- Hay **10 macros**: 8 knobs y un pad X/Y **[M]**.
- La mezcla de las voces pasa por un **filtro global estéreo** y después por la cadena
  **Multi-FX → Delay → Reverb**, cuyo orden se puede cambiar **[M]**.

## Flujo de señal

```
                 ┌────────────────── VOZ (×N, + unísono) ─────────────────────┐
 nota/vel ──►    │  Osc1 ─┐                                                   │
                 │  Osc2 ─┼─► [FilterMix por osc] ─► F1 ─┐                    │
                 │  Osc3 ─┘                        ─► F2 ─┴─► (ruteo) ─► VCA ─►│──┐
                 │   ▲ ring mod Osc(n-1)×Osc(n) [I]                    ▲      │  │
                 │   │                                          EnvA (fijo)   │  │
                 │  Osc1 puede saltear filtros / VCA (O1BP)                   │  │
                 └───────────────────────────────────────────────────────────┘  │
                                                                                 ▼
                                   Σ voces ─► Filtro global (F3, estéreo) ─► FX chain ─► out
                                                              MFX ─ Dly ─ Rev (orden = Ord)
```

### Ruteo de filtros (`Rout`) **[M][P]**

Decompilado de la voz (`0x100403f68`, ganancias en `0x1004036c0`) **[B]**:

- Cada oscilador se reparte entre dos **buses mono** según su `FMix`: bus F1 =
  `osc·(1 − FMix)`, bus F2 = `osc·FMix` (`0x1003af0cc`). El reparto no depende de `Rout`.
- Cada filtro activo procesa su bus. Un filtro apagado deja pasar el bus. La ganancia de
  salida (`OGn`) se aplica al mezclar.
- El enum de `Rout` es 0 `PARALLEL`, 1 `SERIES`, 2 `STEREO`.
  - `PARALLEL`: la salida de cada filtro va a la salida estéreo con su `Pan` (ley de
    potencia constante, seno/coseno).
  - `SERIES`: la salida de F1 se suma al bus de F2 (mono), y F2 va a la salida con su `Pan`.
  - `STEREO`: **F2 usa los parámetros de F1** (al calcular F2 se le pasa el patch de F1), y
    las dos salidas se panean igual, con ganancia ×2. El manual dice "F1 izquierdo, F2
    derecho" **[M]**. Que el `Pan` de los osciladores reparta entre los dos buses en este modo
    **[?]**.

`O1BP` (`Osc1Bypass`) = `FILTER` hace que el oscilador 1 no pase por los filtros de voz.
El manual también menciona saltear la envolvente de amplitud; en los presets solo se
observan los valores `OFF` y `FILTER` **[M][P]**.

## Mapa de clases internas **[B]**

Nombres reales del binario, útiles como guía para nombrar módulos en una
reimplementación.

| Clase interna | Rol |
|---|---|
| `Eden2::cEden2` | El instrumento Obsidian (patch: `Octave`) |
| `Eden2::cSynthVoice` | Gestión de voces: polifonía, unísono, glide |
| `Eden2::cOscTriple` | Los 3 osciladores con capas/splits por velocity |
| `Audio::cSynthOsc` | Un oscilador: parámetros comunes + tipo |
| `Audio::cOscTypeAnalogue` | Oscilador analógico |
| `Audio::cOscTypeWaveTable`, `Audio::cWaveTable` | Wavetable y su tabla cargada |
| `Audio::cOscTypePD` | Phase distortion |
| `Audio::cOscTypeFM` (`cOperator`, `cConnection`) | FM de 3 operadores |
| `Audio::cOscTypeNanoSaw` | Supersaw |
| `Audio::cOscTypeNoise` | Ruido filtrado |
| `Eden2::cFilters` | Ruteo de filtros |
| `Audio::cSynthFilter` | Un filtro multimodo (incluye waveshapers, comb, formant) |
| `Audio::cEnvelopeADSR` | Envolvente (ADSR / AD / Bipolar) |
| `Audio::cLFOSimple` | LFO |
| `Eden2::cEden2Effects` | Cadena de efectos (enable + orden) |
| `Audio::cEffectMultiFXSimple` | Chorus/Ensemble/Flanger/Phaser |
| `Audio::cEffectDelaySimple` | Delay estéreo/cross/pingpong |
| `Audio::cEffectReverbAlgoSimple` | Reverb algorítmica (versión "simple" de Algoverb) |

Los efectos de Obsidian son versiones reducidas (`*Simple`) de los efectos de inserción
completos de NanoStudio: `cEffectMultiFX`, `cEffectDelay`, `cEffectReverbAlgo` **[M][B]**.
Las versiones completas tienen más parámetros, listados en [07-effects.md](07-effects.md).

## Conceptos de la UI que importan para el motor

- **Rapid connection**: es solo UI; termina creando un slot en la matriz de modulación
  **[M]**.
- **Macros**: los knobs y el pad son fuentes de modulación (`Knob 1..8`, `Pad X`, `Pad Y`).
  También pueden ser multiplicadores (`Sca`) de otra conexión **[M][P]**.
- **Recently Modified / Default**: es gestión de patches, no afecta el sonido.
