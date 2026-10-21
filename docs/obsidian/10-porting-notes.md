# Notas para un port

Decisiones de diseño propias, no observaciones de NS2. Separan el *qué* (los otros
documentos) del *cómo* en cada plataforma.

## Estructura recomendada

Un núcleo de DSP en **C++ sin dependencias de plataforma**, con dos frontends:

```
core/            C++17, sin heap en el hilo de audio, procesa bloques de N samples
  preset/        parser XML .obs → struct Patch (fuera del hilo de audio)
  dsp/           osc, filter, env, lfo, modmatrix, fx
  voice/         asignación de voces, unísono, glide
web/             core compilado a WASM + AudioWorkletProcessor
esp32/           core + I2S + USB-MIDI (ESP-IDF)
tools/           conversores (.obs → binario compacto, .tbl → C array)
```

Así un preset suena igual en el navegador y en el hardware, y la web sirve para
iterar rápido y comparar contra renders de NS2.

## Prioridades (según uso en los presets de fábrica)

1. Un motor de wavetable con mipmaps (cubre Analog Sine/Tri/Saw/Square y Wavetable a la
   vez, ver [03](03-oscillators.md)), filtros LP 12-A / LP 24-A, EnvA/EnvF/EnvP, LFO
   seno, matriz de modulación, Chorus 1, Delay Stereo, reverb Small Room.
2. Modos Detune/Spread de la wavetable, Pulse, NanoSaw, Noise White, unísono, glide.
3. FM, PD, resto de filtros, resto de FX.
4. Sample y spectral loop.

## Web

- `AudioWorkletProcessor` con bloques de 128 samples; el core en WASM con
  `SharedArrayBuffer` o `postMessage` para los parámetros.
- El parser de `.obs` puede quedar en JS (DOMParser) y pasar un struct plano al worklet.
- No hay límite real de CPU para 32 voces con todo prendido.

## ESP32-S3 N16R8

- Bloques de 32 a 64 samples, audio en un core y MIDI/UI en el otro.
- Usar `float` en todo el DSP (el S3 tiene FPU de precisión simple); nada de `double`.
- Wavetables como int16 en flash, con los mips activos copiados a SRAM interna.
- Las líneas de delay y reverb en PSRAM (acceso secuencial).
- Parsear los presets en el PC y guardarlos en un formato binario compacto: no hace
  falta un parser XML en el micro.
- Medir ciclos por voz (`esp_cpu_get_cycle_count`) desde el primer prototipo para
  fijar la polifonía real.

### Mediciones en hardware (ESP32-S3 N16R8, 240 MHz, PSRAM OPI)

Benchmark en [`bench/obsidian_bench`](../../bench/obsidian_bench/obsidian_bench.ino):
bloques de 64 muestras, 48 kHz, Arduino-ESP32 3.3.8 (`-Os`), float. El presupuesto de un
core es 240 MHz / 48 kHz = **5000 ciclos por muestra**.

| Bloque | Ciclos/muestra | % de un core |
|---|---|---|
| Oscilador wavetable (int16, interp. lineal, crossfade de 2 frames) | 67.9 | 1.4 % |
| Sierra DPW (1 de las 7 del NanoSaw) | 54.8 | 1.1 % |
| NanoSaw (7 sierras DPW) | 435.6 | 8.7 % |
| Ladder 4 polos + clip con `fminf`/`fmaxf` | 205.6 | 4.1 % |
| Ladder 4 polos + clip con comparaciones | **77.9** | 1.6 % |
| Biquad (filtros `-D`, EQ) | 16.8 | 0.3 % |
| Envolvente racional por muestra (1 división) | 102.1 | 2.0 % |
| LFO seno (tabla de 512) | 56.8 | 1.1 % |
| Voz: 3 osc WT + 2 ladders + 2 envolventes, ingenua | 704.7 | 14.1 % |
| Voz optimizada (envolventes por bloque, clip con comparaciones) | **363.6** | 7.3 % |
| Freeverb 16 combs, buffers en SRAM interna | 1153.7 | 23.1 % |
| Freeverb 16 combs, buffers en PSRAM | 1274.8 | 25.5 % |
| Plate de Dattorro, SRAM interna | 700.4 | 14.0 % |
| Plate de Dattorro, PSRAM | 774.9 | 15.5 % |

**Polifonía estimada** con la voz optimizada: ~13.8 voces por core sin FX; ~11.6
compartiendo el core con la plate; **~17 voces** usando los dos cores al 70 % (el resto
queda para MIDI, matriz de modulación, I2S y margen), con la plate en PSRAM.

**Lecciones:**

- En Xtensa, `fminf`/`fmaxf` son **llamadas a librería**: reemplazarlas por comparaciones
  bajó el ladder de 206 a 78 ciclos.
- La división por muestra de la curva de envolvente es cara. Evaluarla a tasa de control
  (una vez por bloque) e interpolar linealmente reduce la voz a la mitad, y NS2 hace algo
  equivalente con sus parámetros (slews e interpolación de coeficientes por bloque).
- PSRAM octal cuesta solo ~10 % más que la SRAM interna para reverbs de acceso secuencial.
  Conviene: la SRAM interna (~300 KB libres) queda para wavetables y estado de voces.
- La plate de Dattorro rinde mejor que Freeverb (775 frente a 1275 ciclos) y cubre Dense
  Room, Plate y Hall. Freeverb solo hace falta para Small/Large Room.
- El NanoSaw es el oscilador más caro (7 sierras). Con NanoSaw en los 3 osciladores la voz
  sube unos 1100 ciclos: mejor limitar a un NanoSaw por voz o usar menos sierras.
- No medido todavía: FM (3 operadores int16), PD, Noise, Multi-FX y Delay. Por su
  estructura deberían costar del orden de un oscilador wavetable cada uno.

La placa quedó con el firmware del benchmark. El backup del firmware anterior está en
`bench/backup/esp32s3_boot_nvs_apps_0x0-0x410000.bin` (bootloader, NVS y las dos apps;
la partición FAT no se tocó). Para restaurarlo:

```bash
esptool --port /dev/cu.usbmodem214201 write_flash 0x0 bench/backup/esp32s3_boot_nvs_apps_0x0-0x410000.bin
```

### Primer firmware que suena

[`firmware/obsidian_esp32`](../../firmware/obsidian_esp32/) (Arduino-ESP32 3.3.8, mismas
opciones de placa que el benchmark):

- **Salida**: PCM5102A por I2S, estéreo, 16 bits, 48 kHz. Pines **BCK = GPIO17,
  LCK = GPIO16, DIN = GPIO15**.
- **Motor** (`synth.h`):
  - 8 voces.
  - Cada voz: 3 osciladores de wavetable (Sine/Tri/Saw/Square como el Analog de
    Obsidian) con mipmaps por octava generados en la placa por síntesis aditiva (145 ms
    al arrancar, sin usar tablas de NS2), ladder LP24, cutoff `20·900^x` con keytrack
    relativo a C3, y envolventes de amplitud y filtro con la curva racional a tasa de
    bloque.
  - Glide en semitonos.
  - Plate de Dattorro en PSRAM y saturación suave a la salida.
- **Audio en el core 1**, tarea de máxima prioridad. El `loop()` de Arduino (consola serie) corre en el mismo core con menor prioridad; el core 0 queda libre (Wi-Fi/BT apagados).
- **Carga medida**: ~77–81 % del core 1 con la demo (hasta 8 voces + plate).
- **Uso**: al arrancar toca una progresión Am–F–C–G con arpegio. Por la consola serie
  (115200) acepta `on <nota> [vel]`, `off <nota>`, `demo 0|1`, `cut`, `res`, `env`,
  `wave <osc> <0..3>`, `rev`, `vol`, `glide`, `stat` y `help`.

**Actualización: reverb en el core 0 y MIDI** (`obsidian_esp32.ino`, `usb_midi_host.h`):

- La plate corre en una tarea propia en el **core 0**. El core 1 renderiza las voces del
  bloque *n* mientras el core 0 procesa la reverb del bloque *n−1* (un bloque, 1.3 ms, de
  latencia solo en la señal húmeda). Medido con 5–6 voces: **core 1 ~33–40 %**, core 0
  ~30–35 %. Por eso la polifonía subió a **10 voces**.
- **MIDI**: notas, pitch bend ±2 semitonos, sustain (CC64), all notes off (CC123) y CCs
  asignables con **MIDI learn** (`learn <param>` por serie; el mapa se guarda en la NVS).
  Por defecto: CC74 cutoff, CC71 resonancia, CC1 cantidad de envolvente de filtro, CC91
  reverb, CC7 volumen, CC5 glide, CC73 ataque, CC75 decay, CC79 sustain, CC72 release.
  Parámetros asignables: `cut res env rev vol glide atk dec sus rel fdec det lforate lfoamt`.
  `lforate` y `lfoamt` mueven la velocidad (0.05–20 Hz) y la amplitud del LFO 2, que en el
  layout de Obsidian va al cutoff (436 presets): el wobble del filtro.
- La demo se apaga sola con la primera nota MIDI.
- La opción de placa **USB Mode** elige el transporte MIDI:

  | USB Mode | MIDI | Consola serie | Subida |
  |---|---|---|---|
  | `USB-OTG (TinyUSB)` | la placa es **dispositivo** USB-MIDI para la Mac (`ESP32S3_DEV`) | mismo puerto nativo (CDC) | `UploadMode=cdc` |
  | `Hardware CDC and JTAG` | la placa es **host**: un controlador class-compliant en el puerto nativo | puerto COM (UART0) | por el puerto COM |

- [`firmware/tools/midi_bridge.py`](../../firmware/tools/midi_bridge.py) (macOS, sin
  dependencias, usa CoreMIDI con `ctypes`): `list`, `test <dest>`,
  `bridge <origen> <destino>` y `monitor <origen>` para ver los CCs de un controlador.

**Korg nanoKEY Studio** (escena de fábrica, medido con `midi_bridge.py monitor nanokey`
por USB y con `mon 1` por Bluetooth). El Korg usa una disposición distinta en cada
conexión:

| Control | USB | Bluetooth | En el firmware |
|---|---|---|---|
| Teclado (con OCT −/+) | notas canal 1 | notas canal 1 | notas |
| X/Y pad | pitch bend + CC1 canal 1 | igual | bend ±2 st y cantidad de envolvente de filtro |
| Knobs 1–8 | CC16–23 canal 16 | CC20–27 canal 1 | cut, res, atk, rel, lforate, lfoamt, rev, vol |
| Pads 1–8 | notas 64–71 canal 2, con velocity | CC95–102 canal 1 (127 al apretar, 0 al soltar) | presets 0–7 |

Los CCs de los knobs se superponen entre las dos disposiciones (CC20–23), así que el
firmware las reconoce por canal (`kKorgLayouts`), no por el mapa de CCs. Un CC asignado con
`learn` tiene prioridad.

**Presets**: 8 patches en el firmware (Poly, Pad, Bass, Lead, Pluck, Organ, Brass y
Abandoned Factory). Los primeros siete son propios; **Abandoned Factory** (pad 8) es el
preset de fábrica `Factory/Instruments/Obsidian/Abandoned Factory.obs` traducido a mano. Se eligen con los pads, con Program Change (`programa % 8`) o con
`preset <0..7>` por serie. Los mensajes de los pads no suenan; el resto se procesa en omni.
`mon 1` imprime por serie cada mensaje MIDI que llega (USB o Bluetooth).

**Bluetooth MIDI** ([`ble_midi_central.h`](../../firmware/obsidian_esp32/ble_midi_central.h)):
en los dos modos USB la placa actúa como central BLE (librería `BLE` del core, Bluedroid).

- Escanea, se conecta sin emparejar al primer periférico que anuncia el servicio BLE-MIDI
  (`03B80E5A-…`) y se suscribe a la característica de E/S (`7772E5DB-…`).
- Decodifica los paquetes (header, timestamps, running status, SysEx descartado) hacia el
  mismo `handleMidi` que el USB.
- Si se corta, vuelve a escanear y reconecta solo.
- Probado con el nanoKEY Studio a pilas.
- El nanoKEY solo manda notificaciones sobre un enlace cifrado: el central se empareja
  (Just Works, con bonding y sin PIN) antes de suscribirse. Sin eso conecta pero no llega
  ningún mensaje.
- El periférico acepta una sola conexión: si estaba conectado a la Mac, hay que
  desconectarlo ahí.

**Módulos agregados a `synth.h` para traducir presets de fábrica**:

- Oscilador **Noise** (`kOscNoise`):
  - El generador entero, el sample & hold que sigue la nota (`Deci`) y el biquad de los
    tipos Low/High/Band Passed y Notched, con las fórmulas de `f` y `q` de
    [03](03-oscillators.md#noise-nois--cosctypenoise).
  - Pink y Brown no están.
- Oscilador de **wavetable importada** (`kOscTable`): frames de un `.tbl` de NS2 con
  crossfade lineal según `Pos` (+ modulación).
- **Ring** por oscilador: mezcla hacia `osc(n−1) × osc(n)`. Un oscilador con nivel 0 sigue
  corriendo si el siguiente lo usa para el ring.
- Filtro **LP 12-D** (biquad, `Q = 0.5 + 5.5·q²`) además del ladder LP 24-A, y `OGn`.
- **Envolventes**:
  - Curvas por segmento (`AC`/`DC`/`RC`) y `Levl`.
  - Velocity y key scaling decompilados (`VLev`, `VTim`, `KCtr`, `KLev`, `KTim`).
  - Los presets propios usan `VLev = 1` en la de amplitud.
- **Rampas por muestra**: los coeficientes del ladder y del biquad se calculan por bloque
  y se interpolan linealmente muestra a muestra, como en NS2. La posición de la wavetable
  también se interpola dentro del bloque. Sin eso, los LFOs rápidos sobre el cutoff
  producen escalones audibles.
- **Matriz de modulación general** ([06](06-modulation.md)): 24 slots
  `fuente × multiplicador × Dept/100` sumados por destino, evaluados una vez por bloque.
  - **Fuentes**: Const, Rand 1/2 (0..1 por nota **[I]**), Note (`nota/127` **[I]**),
    Velocity, Gate, las 5 envolventes, los 5 LFOs, Pitch Wheel (y sus mitades), Knob 1..8,
    Pad X/Y.
  - **Destinos**:
    - Por oscilador (`Osc All` suma a los tres): Pitch Coarse, Pitch Fine, Level, Ring
      Mod, WT Position y Noise Cutoff/Q/Decimate.
    - Filtro 1: Cutoff, Cutoff Slow (one-pole de 50 ms **[?]**), Q y Output Gain (`2·m`).
    - Amp Gain: `×clamp(1 + m, 0, 2)` **[?]**.
    - Envolventes: A/D/R en el dominio `x` con `Tmax` 10/20/20 s; S y Level se suman.
    - LFOs: Rate (`Rt + 60·m³`) y Level.
    - Del motor: cutoff y Q del filtro global, Reverb Mix y Multi-FX Mix.
  - Las envolventes y los LFOs usan la matriz del bloque anterior para sus propios
    parámetros. Los que ningún slot lee no se calculan.
  - Los destinos del motor no tienen voz dueña. Toman las fuentes por voz de la voz más
    nueva y los LFOs de 5 LFOs del motor que corren libres **[?]**.
  - `Pitch Coarse` está en semitonos con `m = 1` = 100 semitonos **[I]**: 304 presets
    de fábrica doblan con `Pitch Wheel → Pitch Coarse, Dept 2` (±2 semitonos), y los hats
    suben una octava con `Dept 12`. `Pitch Fine` usa ±1 semitono **[I]**.
  - Al cargar un preset, los knobs y el pad toman los valores de `Autm` `C02..C11`.
    Los knobs del Korg controlan lo mismo en todos los presets (cut, res, atk, rel, fdec,
    det, rev, vol) y no mueven los macros. Antes movían el macro del preset si este lo usaba.
    Así, en el Rhodes el knob 1 era Amp Attack y no el filtro.
  - El knob de volumen tiene curva de audio (`volume = x²`). Con el limitador lineal hasta
    0.5, la escala lineal llegaba al techo en el primer tercio del recorrido.
  - Los presets propios (0..6) arrancan con un slot `Pitch Wheel → Osc All Pitch Coarse`
    de ±2 semitonos. El bend dejó de ser un caso aparte del motor.
- **LFOs completos** (`cLFOSimple`, [05](05-envelopes-lfos.md#lfos-clfosimple)):
  - Formas: Sine (con `Crv`, sobre la tabla del seno), Square, Triangle, Saw Up/Down,
    Saw Double y Random (LCG decompilado).
  - Random Loop repite `RLen` pasos desde una semilla por `RPtn`; los patrones de NS2 son
    **[?]**.
  - `OutT`, `Warp`, `Qtz`, `Atk` (fade-in) y `PhOf`.
  - Sync: OFF (fase aleatoria), KEY (`PhOf`) y GLOBAL (la fase del LFO del motor
    reemplaza al transporte).
  - Las velocidades sincronizadas al tempo se tocan a 120 BPM. Faltan FM y FM Sync.
- **Filtro global comb** (F3 `Comb Pos`/`Comb Neg`), estéreo:
  - Algoritmo decompilado ([04](04-filters.md#formant-waveshapers-y-lo-fi-b)).
  - Keytrack sobre la última nota.
  - El retardo se interpola por bloque, sin el crossfade de lecturas de NS2.
- **Voces estéreo y unísono** (`Uni`, `UDtn`, `USte`): detune, paneo y nivel con las
  tablas decompiladas ([02](02-preset-format.md#unísono-b)).
  - Ley de paneo lineal con ganancia 1 al centro **[?]**.
- **Phaser** estéreo en la etapa Multi-FX:
  - Cadena de allpass con realimentación y LFO desfasado por `Wid`.
  - Rate `0.02 + 4·Rat²` Hz, barrido de 150 Hz a `150·2^(8·Dep)` Hz, realimentación
    `0.9·FB`. Las escalas de NS2 son **[?]**.
- Modo **mono** (`Poly=1`) con glide legato.
- Oscilador **Sample** (`kOscSample`), ver [abajo](#samples-en-flash):
  - Una zona por nota (la primera cuyo `Lo..Hi` contiene la tecla), sin el crossfade
    entre zonas solapadas ni la segunda zona de NS2.
  - Lectura a `2^((pitch + Tune − Root)/12)·sr_archivo/48000`, interpolación lineal y
    loop forward del chunk `smpl`; `Vol` lineal. Sin `Loop=Spectral`, `Fwd-Rev`, `Pan`
    de zona ni el `0.7071` del paneo al centro.
- **Envolventes de tipo AD y Bipolar**, más `EnvP`, `Env4` y `Env5` por voz:
  - AD baja a 0 después del decay e ignora el note-off. Bipolar arranca en `SLv` y en el
    release va a `RLv`. Las dos son lecturas del manual **[I]**: no se decompiló la
    máquina de estados.
  - `Level` de un oscilador sigue `clamp(Lev + 2·m, 0, 2)` y se interpola dentro del
    bloque.
- `KeyT` por oscilador (`60 + (nota − 60)·KeyT`). El detune de unísono y la modulación
  de pitch no se escalan.
- Oscilador **FM** (`kOscFM`), como en [03](03-oscillators.md#implementación-b-0x100565478):
  - Fases de 32 bits y las 15 formas de banda limitada extraídas del binario con
    [`fmwaves2h.py`](../../firmware/tools/fmwaves2h.py) (`fm_waves.h`, 43 KB, uso local).
  - Feedback promediado y moduladores con una muestra de retardo.
  - Cantidades `(1 − Levl + Levl·env) × velocity × (Amt + mod)` al cuadrado por `K`,
    interpoladas dentro del bloque. Las 8 envolventes de conexión corren por voz.
  - Destinos de matriz: `FM OPx->OPy`, `FB`, `Out` y `Freq` (`ratio + 10·m`).
  - Como en NS2, el render está especializado: un operador corre solo si llega a la
    salida en ese bloque.
- **Ranuras de filtro** (`Filter`) para F1, F2 y el filtro global F3, con LP 24-A,
  LP 12-A, HP 12-A, HP 24-A, BP 6-A, BP 12-A, Notch 6-A y Notch 12-A en el ladder, y
  LP 12-D, HP 12-D, EQ Peak y EQ Shelf en el biquad.
  - F3 que no es comb corre en estéreo después de la mezcla de voces. Sigue el keytrack de
    la última nota y toma cutoff y Q de la matriz.
  - El ladder global recibe la mitad de la mezcla, como F1, y la salida se compensa ×2.
- **F2 y ruteo de filtros** ([01](01-architecture.md#ruteo-de-filtros-rout-mp)):
  - Segundo filtro por voz, `FMix` por oscilador y `O1BP`. `PARALLEL` y `SERIES` como en
    NS2, y `Pan` de salida de F1 y F2 con la ley lineal del port.
  - `STEREO` pasa los dos buses por F1, porque F2 corre la configuración de F1.
  - Rutas de matriz a `Filter 2` (Cutoff, Q, Output Gain); `Filter 1+2` llega a los dos.
  - Falta el `Pan` por oscilador.
- **Waveshapers** (`WS Tube/Soft/Hard/Enhance/Sine/Tri`) como tipo de F1, F2 o F3, con las
  curvas decompiladas en tablas de 1025 puntos (en el `WaveBank`, PSRAM) y las ganancias de
  `Drv` y `WSGn` ([04](04-filters.md#formant-waveshapers-y-lo-fi-b)). Sin sobremuestreo:
  los plegadores (Sine, Tri) tienen aliasing.
- **NanoSaw** (`kOscNanoSaw`): las 7 sierras DPW con la curva de detune, el reparto y las
  ganancias decompiladas ([03](03-oscillators.md#nanosaw-ns--cosctypenanosaw)), en mono. Por
  debajo de 40 Hz la sierra se lee directa (DPW divide por la frecuencia).
- **PD** (`kOscPD`): la sierra como en NS2 (la curva de la cantidad, con el codo en 0.25 y
  `c = −0.3067` para que sea continua). Las otras ocho formas son lecturas del Casio CZ con
  esa curva y los exponentes documentados; las resonantes ventanean un seno a
  `r = 1 + 16·PD²` veces el pitch **[I]**. El tramo empinado dura al menos 4 muestras, en
  lugar del polinomio de NS2 por pitch **[I]**.
- El biquad no baja de 20 Hz: más abajo, el redondeo en float y la rampa de coeficientes lo
  hacían diverger (Slow Sweep Pad, con un LFO random sobre un LP 12-D de ~30 Hz).
- **Amp Gain** se suma al `Levl` de la envolvente de amplitud **[I]**. Arping On tiene
  `Levl` 0 y abre la amplitud con Env 4 → Amp Gain.
- **Delay** (`Dly`): Stereo, Cross y Pingpong 1:1..1:4, ver
  [07](07-effects.md#delay-dly--ceffectdelaysimple).
- **Ladder decompilado, con Drive** (2026-10-20): el `Ladder` sigue el setup y el render
  de NS2 ([04](04-filters.md#q-y-resonancia-b)). Eso incluye las etapas de 1 polo con
  cero (`0.769/0.231`), `G` y `k` polinómicos, el saturador cúbico con `sat0`/`sat1` que
  fija `Drv`, las compensaciones de Q por modo y los diez modos de la tabla.
  - `Drv` de F1, F2 y F3, y los destinos `Filter 1/2 Drive` de la matriz.
  - Reemplaza al ladder aproximado (`tanf`, `k = 3.6·q`, recorte duro en ±1.2). Ese
    recortaba las mezclas fuertes. Con 3 notas en host, la mediana del RMS de los 448
    presets subió ×1.7 (+4.6 dB). La salida sigue pasando por el limitador suave de
    `mixOut`, y en la placa los picos quedan entre −1 y −7 dBFS.
  - Electric Bubbles y Doppler's Bike, que estaban mudos, ahora suenan.
  - El límite `k ≤ 3/(sat0·sat1)` está activo **[I]**: sin él, los bass con Drive y algo de
    resonancia autooscilaban y sonaban mucho peor.
  - `kOutputTrim` (−3 dB) baja la salida antes del limitador suave para compensar el nivel
    más alto. El valor se eligió de oído en la placa.
  - El limitador de salida era `x/(1 + |x|)`, que comprime y distorsiona desde niveles bajos
    (a 0.2 ya baja un 17 %). Ahora es lineal hasta `kClipKnee` (0.5) y se curva suave hacia
    fondo de escala por encima.
- Al arrancar se carga `kBootPreset` (RhodesMK2Raw) si la imagen de contenido lo tiene.
- **Pitch Env Bipolar** (corregido, 2026-10-20): desde la matriz general, la Bipolar subía
  a 1 y se quedaba en `S`. Los 352 presets con la configuración por defecto sonaban una octava
  arriba. Ahora sigue el modelo de [05](05-envelopes-lfos.md#tipos-type).
  - Verificación por render en host, con la misma secuencia de notas: el código anterior al
    ladder nuevo, con esta corrección, da el Rhodes idéntico al de la primera versión con
    samples (correlación 1.00). El paso a la imagen de contenido no cambió el sonido.
  - Con el ladder decompilado, el Rhodes correlaciona entre 0.58 y 0.86 con esa versión: es
    algo más brillante y unos 2 dB más fuerte a velocidad alta.
- **Matriz al inicio de la nota** (corregido): las envolventes avanzan con la matriz del
  bloque anterior, y una voz libre la tenía en 0. Un attack corto terminaba en el primer
  bloque sin ver la modulación, y el Knob 1 → Amp Attack del Rhodes no hacía nada. Ahora el
  primer bloque evalúa la matriz una vez antes de avanzar las envolventes, con las
  envolventes y los LFOs en 0.
- En mono, un note-on hace legato solo si la tecla anterior sigue apretada. Así, una
  envolvente AD que todavía suena no impide re-disparar la nota.
- **Capas y splits por velocity** (`Osc` `Layer`/`Split 2`/`Split 3` con `LL1..LX2`): las
  ganancias por oscilador decompiladas de `cOscTriple`
  ([03](03-oscillators.md#capas-y-splits-por-velocity-osc--cosctriple-b)), calculadas al
  note-on y en el legato mono. Un oscilador con ganancia 0 no se renderiza.

Las wavetables se convierten con
[`firmware/tools/tbl2h.py`](../../firmware/tools/tbl2h.py):
- Toma un mip por octava de los 44 del `.tbl`, lo remuestrea a 1024 y guarda int16 en
  flash.
- Orchestral: 6 frames, ~135 KB.
- El header generado sale de contenido de NS2: es para uso local, no para distribuir.

Traducción de **Abandoned Factory**:

| Del preset | En el port |
|---|---|
| Osc1 WT `Instrument/Orchestral` Pos 0.5, Trn +7, Lev 0 | igual; solo alimenta el ring de Osc2 |
| Osc2 Analog Sine, Trn −24, Lev 2, `Ring` 1 | igual |
| Osc3 WT Orchestral Pos 0.5, Trn −24, Lev 0.73 | igual |
| F1 LP 12-D, `CutO` 0.255, `KeyT` 0, `OGn` 1.34 + Filter Env (Dept 100) | igual |
| EnvA/EnvF con curvas y `Levl`; EnvA `VLev` 0.4, `KLev` 0.4 (`KCtr` 24) | igual |
| LFO3 (36.6 Hz), LFO4 (0.15 Hz) → cutoff; LFO5 (0.28 Hz) → posición de WT de Osc1/Osc3 | igual |
| F3 Comb Neg, `CutO` 0.259, `KeyT` 1 + LFO5 → cutoff (Dept 100) | comb global |
| `Uni` 2, `UDtn` 0.12, `USte` 0.87 | unísono ×2: ±2 cents, paneo 0.07 / 0.93 |
| Flanger, Delay y Reverb con `Mix` 0 | apagados |

Lo que quedó afuera:

- `Rout=STEREO` con F2 apagado: el port pasa los dos canales por F1.
- El paneo por oscilador (LFO4 → paneo de Osc2).
- LFO5 → ancho de pulso de Osc2: es un seno, no tiene ancho de pulso.
- LFO3 → fase de Osc3 y `POff`.
- La envolvente de pitch (Bipolar con S = 1).
- Los knobs del preset: en el port, los knobs del Korg siguen su mapa propio.

Carga medida: 10 voces (5 notas × unísono 2) usan ~76 % del core 1.

### Imagen de contenido: presets, wavetables y samples en flash

Los presets de NS2 viajan como datos, junto con las wavetables y los samples que usan, en una
partición `content` de la flash. El firmware la lee con `esp_partition_mmap`, a través de la
caché, sin copiarla ([`content.h`](../../firmware/obsidian_esp32/content.h)).

- [`content2bin.py`](../../firmware/tools/content2bin.py) arma la imagen con presets o
  carpetas de presets:
  - Corre `obs2preset.py` y guarda cada asignación del C++ generado como un registro
    `(campo, valor)`. El campo va en una tabla de rutas compartida (`osc[0].fm.env[3].decay`).
  - Agrega las wavetables que usan los presets (convertidas con `tbl2h.py`) y los samples de
    los presets que se nombran con `--samples`.
  - Deja afuera, y los lista, los presets con un tipo de oscilador que el motor no tiene
    (NanoSaw, PD), los que usan samples sin `--samples` y los que usan samples propios de
    NanoStudio (`/AppRes`, 8.1 MB que no entran).
- El firmware resuelve cada ruta con un registro de los campos del `Patch`
  (`syn::forEachField` en `synth.h`). Para cargar un preset, arma el `Patch` por defecto y
  le aplica los registros en orden. En la Mac, un preset armado así coincide en los 818
  campos con el mismo preset compilado como C++.
- Categorías por pad:
  - Primero los bits de `Tags`: 19 batería, 2 bajo, 0 lead, 1 pad y 10 teclas.
  - Antes que eso, las palabras "pluck" y "brass/string" del nombre, porque `Tags` no tiene
    esas categorías.
  - Si nada aplica, otras palabras clave del nombre y, al final, "Other".
- [`partitions_content.csv`](../../firmware/tools/partitions_content.csv) (copiada como
  `partitions.csv` del sketch): app de 2 MB en `0x10000` y `content` de 14.2 MB en
  `0x210000`. Ya no hay FAT.

Con los 312 presets de fábrica y los 217 de usuario, más los samples del Rhodes, la imagen
tiene 448 presets, 23 wavetables y 57 samples: 13.8 MB, el 95 % de la partición. Quedan
afuera 81, todos por samples: 73 de usuario (habría que nombrarlos con `--samples`, y no
entran todos) y 8 de fábrica con samples propios de NanoStudio. Para armarla y grabarla:

```bash
python3 -I firmware/tools/content2bin.py content.bin "/Applications/NanoStudio 2.app/Wrapper/NanoStudio 2.app/Factory/Instruments/Obsidian" "<carpeta de presets>" --samples "<carpeta>/RhodesMK2Raw.obs"
esptool --chip esp32s3 --port /dev/cu.usbmodem21201 write-flash 0x210000 content.bin
```

La imagen se vuelve a grabar solo si cambian los presets; el firmware no se recompila. Es
contenido derivado de NS2 y de los samples de los presets: uso local.

En la placa, los presets 0..7 son los propios y los de la imagen siguen desde el 8.
- `list [pad]` y `find <texto>` los muestran por serial.
- Cada pad del Korg cicla por su categoría.
- Al cambiar de preset, los parámetros que se movieron (perillas, CC, serial) se vuelven a
  aplicar, y las perillas que mueven un macro del preset nuevo toman su posición física.

En la Mac, de los 448 presets 7 quedan mudos con una nota a velocity 100: Doppler's Bike
(comb como filtro de voz), Electric Bubbles, Synced Noises, BA 3, PD Chimes, PD Deeper
Chimes y PD Keys or Pad (sync analógico o niveles que solo llegan por macros).

Medido en la placa con la imagen anterior, solo de samples: los 9.3 MB se mapean enteros (el
espacio de direcciones externas del S3 es de 32 MB, compartido con la app y los 8 MB de
PSRAM). Con 10 voces a velocity 80 (dos osciladores Sample por voz, en el crossfade) el
core 1 queda en ~63 %. Cada oscilador activo lee de flash en forma secuencial unos 96 KB/s.

Traducción de **RhodesMK2Raw**:

| Del preset | En el port |
|---|---|
| 3 osciladores Sample (ROS/ROM/ROH, 18–20 zonas), `Trn` 1, `Dtn` ≈ 0.47, `Lev` 0.44/1.52/2 | igual |
| `Osc` `Split 3`: `S3L1` 0.353, `S3L2` 0.8745, `LX1` 1, `LX2` 0.0325 | igual |
| F1 LP 24-A, EnvA/EnvF | igual |

Lo que quedó afuera (lo lista `obs2preset.py`): `Rout=PARALLEL`, las rutas de velocity,
pitch envelope, pitch wheel, Pad X/Y y knobs de la matriz, `GTim` en poly y el Flanger.

### Presets de batería

Estos 8 presets de fábrica fueron los primeros de batería (antes compilados en el firmware;
ahora están en la imagen de contenido, en el pad 7):

| Preset | Qué falta (además de knobs, pads XY y pitch wheel) |
|---|---|
| Goa kick | F1 EQ Shelf, F2 EQ Peak, F3 WS Tube |
| Hardcore Kick | F1/F3 WS Hard, F2 EQ Peak; `Poly=0` tocado poly |
| x0x Snare | ruido Brown, `Drv`, F2 EQ Peak, F3 HP 12-A global |
| Snare 5 | `Drv`, ruteo STEREO, F3 WS Hard |
| x0x Hat 1 CL / OP | `Drv`, `Brk`/`D2`, F2 EQ Shelf, F3 LP 12-A global, Env 5 → Reverb Mix (la reverb queda en `Mix` 1) |
| Dirty Clap | `Drv`, `FMix`, F3 HP 12-A, Filter Env → Noise Cutoff, LFO Saw Down |
| Electro Perc | F2 WS Soft, F3 EQ Peak, rutas de velocity |

Medido en el host (nota 60, velocity 110): los kicks barren hacia ~60 Hz en 100 ms, y
todas las voces terminan solas por la AD (de 27 ms a 900 ms; Electro Perc es corto porque
su `Velocity → Amp Env Decay` resta en el dominio `x`). En la placa, una
voz de batería usa del 5 al 10 % del core 1. No se comparó contra un render de NS2.

### Cobertura de presets (2026-10-20, con Drive y el ladder decompilado)

Resultado de `obs2preset.py` sobre los 312 presets de `Factory/` y los 217 de usuario y de
terceros, con el mismo criterio que antes. No se cuenta lo que no cambia el sonido en
reposo: macros en 0, pitch wheel, `Rout` sin F2, cantidad de voces, `GTim` por defecto en
poly, `Rand → Phase` y el tipo de reverb. Quedan completos 25 de fábrica y 26 de usuario
(antes del Drive eran 6 y 14).

Ya no falta ningún tipo de oscilador ni el `Drv`. Con BP y Notch en el ladder, F2 bajó de
45/27 a 15/9 y el filtro global no comb de 30/3 a 22/0. El delay ya no falta: los 147/71
que lo marcan son delays a tempo tocados a 120 BPM.

| Falta | Fábrica (312) | Usuario (217) | Comentario |
|---|---|---|---|
| Tempo (LFOs y delay a 120 BPM) | 131 / 147 | 48 / 71 | sin transporte en el port |
| Paneo y ancho por oscilador | 93 | 16 | |
| Chorus / Ensemble / Flanger | 62 | 54 | |
| Sync, Pulse/Bi-Pulse | 46 / 45 | 61 / 25 | |
| Phase offset por oscilador | 45 | 58 | |
| Breakpoint y delay de las envolventes | 39 | 39 | |
| Tipos de filtro faltantes (Formant, Lo-Fi, LP 6-D, Comb en F1/F2) | 50 | 23 | |
| Macros hacia FX (Multi-FX Mix, Reverb Time) | 33 | 57 | |

### Rendimiento y memoria con FM (2026-10-20)

- `synth.h` compila con `-O2` (`#pragma GCC optimize`) en lugar del `-Os` del sketch. Con
  10 voces, Poly bajó de 75 % a 65 % del core 1 y Abandoned Factory de 87 % a 68 %.
- Un oscilador FM cuesta ~7 200 ciclos por bloque con dos operadores activos y ~10 400 con
  los tres (un oscilador de wavetable, ~1 750). Las tablas en flash o en RAM rinden igual.
- La FM y la matriz agrandaron la voz, y al iniciar BLE ya no quedaba RAM interna (la placa
  se reiniciaba en bucle). El banco de formas analógicas (`WaveBank`, ~90 KB) pasó a PSRAM:
  quedan 183 KB internos libres, y la carga de Poly no cambió de forma medible.
- **Protección de sobrecarga**: si la carga de voces, promediada sobre ~16 bloques, pasa
  el 90 %, se apaga la voz más vieja (`stat` cuenta las apagadas). Sin esto, un preset
  pesado deja sin CPU al `loop()` del mismo core, los note-off no llegan y la placa queda
  colgada. Con acordes de 10 notas, los presets FM se estabilizan entre 6 y 10 voces
  (79 % a 87 %); los otros no pierden voces.

- **Phaser inestable con `Dep` 1** (corregido): el barrido `150·2^(8·Dep)` llegaba a 38 kHz.
  Pasado Nyquist, el allpass diverge aunque `Mix` sea 0. El NaN quedaba en el phaser y en la
  plate, y desde ese momento todo preset con reverb salía mudo hasta reiniciar. Ahora el
  barrido se recorta a `0.45·sr`. `stat` muestra el pico de salida desde el último `stat`
  para detectar esto sin escuchar.

Los primeros presets FM probados (hoy en la imagen de contenido):

| Preset | Pad | Qué falta |
|---|---|---|
| Electric Bass, Bass Organ | 1 Bass | ruteo PARALLEL con F2 apagado, un macro a Multi-FX Mix, `Widt` |
| FM Stoned Keys, Electric Piano 2, Organ FM, Bell | 8 Keys | `Brk` de las envolventes, paneo por oscilador, `Drv`, macros a FX |
| Mcp Dance Pluck (usuario) | 4 Pluck | delay |

Organ FM usa la wavetable `Complex/Tech`.

**Próximos pasos naturales:**

- Paneo y ancho estéreo por oscilador (incluido el `Widt` del NanoSaw), chorus/ensemble.
- Confirmar contra renders de NS2 lo marcado [I] y [?], sobre todo la escala de Pitch
  Coarse, Amp Gain, el delay y las formas PD que no son sierra.

## Verificación contra NS2

Para cada módulo: un patch mínimo en NS2 (un oscilador, sin filtro ni FX), un render de
una nota fija y una comparación de espectro y envolvente contra el port. Las preguntas
marcadas con **R** en [09-open-questions.md](09-open-questions.md) se resuelven así.

### Renders de referencia en la Mac (2026-10-20)

NS2 2.1.2 corre en la Mac (Apple Silicon). Cada medición sigue estos pasos:
1. **MIDI**: un puerto CoreMIDI virtual ("ObsRef") toca una secuencia fija con
   timestamps: C3, C4, C5, C6 y C2, notas a velocity 40/90/127 y un acorde.
2. **Audio**: la salida va al dispositivo Loopback "Master", y se graba con AudioQueue a
   48 kHz. La captura de ffmpeg con avfoundation perdía ~10 % de las muestras.
3. **Presets**: los patches de prueba se escriben como `.obs` en
   `Documents/Instruments/Obsidian/User` del contenedor de NS2, y se cargan después de
   Manage → Refresh. NS2 solo reacciona a toques lentos (apretar, mover, soltar).
4. **Comparación**: la toma de NS2 contra el render del port en host, antes del volumen y
   del limitador, alineadas por correlación de envolventes. Por nota se mide nivel,
   envolvente, bandas de octava en el sustain y afinación.

Hallazgos corregidos con esto:
- La amplitud es el cuadrado de la envolvente ([05](05-envelopes-lfos.md)).
- Las formas analógicas tienen el RMS de un seno, como `Analog.tbl`: saw y tri pican a
  1.22 y la square a 0.71 ([03](03-oscillators.md)).
- EQ Shelf es de agudos, el ladder satura con el doble de nivel, y F1 apagado no filtra
  ([04](04-filters.md)).
- `Poly=0` es mono ([02](02-preset-format.md)).
- EQ Shelf es un shelf de graves con pendiente `Q²` y ganancia total `1/A`, o sea un EQ de
  inclinación ([04](04-filters.md)). EQ Peak, Output Gain, el ruteo SERIES y la FM básica
  (portadora, 2→3, 1→3 con ratio 2, feedback) coinciden con NS2.
- La Pitch Env Bipolar (corregida antes) y la evaluación de la matriz: las envolventes
  avanzan con la matriz del bloque anterior, como dice la decompilación.
- `Autm` C02..C11 son Pad X, Pad Y y Knob 1..8, en el orden de los nombres de los
  controles ([02](02-preset-format.md#autm)). Antes se leían como Knob 1..8 y Pad X/Y, y eso
  dejaba oscuros o mudos los presets que abren el filtro con el Pad X.
- La sierra arranca en 0 y sube, y la nota arranca sin fade-in ([03](03-oscillators.md),
  [05](05-envelopes-lfos.md)).
- En las conexiones FM, `Levl` 0 deja la envolvente activa ([03](03-oscillators.md)).
- Retrigger y el Random Loop sincronizado ([05](05-envelopes-lfos.md)).
- El nivel del unísono cuenta al cuadrado ([02](02-preset-format.md#unísono-b)). `Lev` 2
  dobla la amplitud, y el Drive del ladder con dos osciladores coincide.
- Waveshapers: el Cutoff es la mezcla seco/procesado y `WSGn` no hace nada
  ([04](04-filters.md#formant-waveshapers-y-lo-fi-b)).
- Una voz libre arranca con los filtros en cero: sin fade-in, el estado que quedaba de la
  nota anterior sonaba como un clic al empezar.
- Abierto: la cola de la envolvente FM con `Levl` 0 ([03](03-oscillators.md)).
- Efectos medidos y corregidos: delay (seco fijo, húmedo `Mix²`, Cross/Pingpong, EQ),
  reverb (ley de mezcla, caída y nivel por tipo, Freeverb para las Rooms, HDmp, cortes,
  pre-delay), chorus/flanger (nuevo) y phaser ([07](07-effects.md)).
- NanoSaw: la central a 0.88 y las laterales a 0.52 ([03](03-oscillators.md)). Formant-M/F
  implementado ([04](04-filters.md)).
- Paneo del unísono con suma constante, como NS2 ([02](02-preset-format.md#unísono-b)).
  Bi-Pulse de tres niveles, y `Osc N Phase` como destino de la matriz (desplaza la fase del
  oscilador; con `Rand` cada nota arranca en otra fase) **[I]**.
- Abierto: Fat Wide Pad (3 osciladores FM con feedback) sale ~13 dB más fuerte que NS2,
  aunque un solo oscilador coincide; Africa Brass y otros presets con unísono 2 y `USte` alto
  quedan 2–4 dB fuertes con la ley de suma constante, que el test directo confirma.

Con eso coinciden con NS2 en nivel (±0.3 dB entre tests), afinación (0 cents) y bandas
(±5 dB) los 17 patches de prueba de un módulo: saw, square, sine, tri, LP24 con y sin Q,
LP12 y LP24 con Drive, LP12-D, HP24, envolvente de filtro, envolvente de amplitud,
velocity y Levl.

Las herramientas están en [`firmware/tools/ref`](../../firmware/tools/ref) (`build.sh`
compila, `rec.sh` graba y `gen.py` arma los patches de prueba). La secuencia vive en
`seq.h`.

Para los efectos hay una segunda secuencia de notas cortas separadas 5 s (`kFxSeq` en
`seq.h`): `rec.sh <toma> fxseq` la toca en NS2 y `render <imagen> <preset> <salida> fx` en el
port, así las colas de delay y reverb se miden sin solaparse ([07](07-effects.md)).

`rank.py` (necesita numpy) renderiza con el port cada toma `<prefijo>NN <preset>.raw` y
ordena los presets por distancia a NS2: ganancia global, diferencia media de la envolvente
de 10 ms y de los espectros de 1/3 de octava del primer segundo de cada nota. La ganancia
de referencia es −5.6 dB (NS2 sale más bajo que el render del port antes del volumen). Las
tomas se graban en tandas de 9 (las filas visibles de la lista de NS2), con el grabador y
los toques sincronizados por reloj en ranuras de 30 s.

Estado de los 37 presets de fábrica grabados (después de los arreglos de arriba):
- Coinciden (envolvente y espectro ≤ 3 dB, nivel ±2 dB): Deep Bass, Bottle Bass, FM Stoned
  Keys, Rave Lead, Bold as Brass, Africa Brass, Percussive Bass, Hardstyle Lead, Hang Drum,
  Hard Glider, Snare 4, NanoSaw 80s, Analogue Kick y Classic Acid (salvo el espectro).
- Con forma correcta y nivel distinto: el port sale 3–9 dB más fuerte en Fat Wide Pad,
  Iron Drops, Ocean of Secrets, Electric Piano 2, Slow Sweep Pad y Magic Pitch, y 5–9 dB más
  bajo en Pluck or Pad, Dubstep Crush, 7th House y Dirty Saws.
- Con FX o modulaciones que el port no tiene: PsyDelay (Env 4 → Delay Time/FB), Arping On
  (el patrón del Random Loop), Dead Space y Liquid Resonance.
- La diferencia "espectral" de algunos presets FM es el piso de ruido de NS2 (−85 a
  −100 dBFS en armónicos pares), que no se oye.
