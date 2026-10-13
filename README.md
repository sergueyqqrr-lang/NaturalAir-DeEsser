# NaturalAir De-Esser

De-esser inteligente (VST3 / AU / AAX) que controla la sibilancia sin perder aire ni naturalidad.
Motor STFT con lookahead, detección por ratio sibilancia/cuerpo, zona dura + zona de aire y control **Air Keep**.

## Estructura
- `Source/DSP/` motor sin dependencias de JUCE (C++17, solo cabeceras)
- `Source/PluginProcessor.*`, `Source/PluginEditor.*` plugin JUCE 8 e interfaz
- `Tests/engine_test.cpp` tests del motor (null test, voz sintética, modos)
- `Dockerfile`, `railway.json` build en Railway
- `.github/workflows/build.yml` build Windows/macOS/Linux

## Railway (VST3 de Linux)
1. Sube esta carpeta a un repositorio de GitHub.
2. En Railway: New Project > Deploy from GitHub repo. Detecta el `Dockerfile` automáticamente.
3. El build compila JUCE (varios minutos), ejecuta los tests (si fallan, el build falla) y empaqueta el plugin.
4. Genera un dominio público (Settings > Networking). Descarga `NaturalAir-DeEsser-linux-VST3.zip` desde la raíz de esa URL.

Limitaciones: Railway compila binarios de **Linux** y no puede ejecutar audio. Para Windows y macOS (VST3 + AU) usa
GitHub Actions: cada push genera artefactos descargables. AAX requiere el SDK de Avid:
`cmake -S . -B build -DNA_ENABLE_AAX=ON -DAAX_SDK_PATH=/ruta/al/sdk`.

## Compilar en local
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
```
Solo el motor y sus tests (sin JUCE): `cmake -S . -B build -DNA_BUILD_PLUGIN=OFF`.

## Estado
- Verificado: null test a -139 dB (44.1/48/96/192 kHz, bloques 1/37/512), voz sintética, compilación VST3 Linux.
- Pendiente: calibrar umbrales del detector con voces reales (A/B), revisar el layout de la interfaz
  (nunca renderizado), probar en DAWs, AU/AAX, firma y notarización.
- Latencia: 1024 muestras a 44.1/48 kHz (21,3 ms), fija por sample rate (el plugin la reporta al DAW).

## Calibración offline con tus propias voces
`process_wav` procesa un WAV de 16 bits y escribe la salida, el delta y un CSV con ratio, umbral, puntuación y reducción:
```
process_wav voz.wav salida.wav delta.wav traza.csv airKeep=65 threshold=50 range=8 adaptive=1
```
Escucha `delta.wav`: debe sonar solo a las "s" que se quitan. Los umbrales del detector están en `Source/DSP/SibilanceDetector.h`
(`score`) y la lógica de umbral adaptativo en `Source/DSP/AdaptiveThreshold.h` (`threshold`).

## Velocidad (Attack / Release)
- **Attack** 0,05–50 ms: lo rápido que baja la ganancia al detectar una "s". Para eses rápidas, déjalo por debajo de 1 ms.
  (La ganancia se actualiza cada ~1,3 ms y el lookahead ya adelanta la reducción, así que por debajo de eso es "instantáneo").
- **Release** 1–1000 ms: lo rápido que suelta. Más corto = recupera el brillo entre eses; más largo = más suave pero puede apagar.

## Range, Width y naturalidad
- **Range** (0 a 30 dB): reducción máxima en el centro de la zona de la ese.
- **Width** (0,15 a 1,6 octavas): anchura de la zona que se atenúa. Con valores altos la atenuación cubre de ~3 a ~12 kHz.
- La zona nunca baja de ~3 kHz (0,45 x la frecuencia de la ese, mínimo 2 kHz): el cuerpo y la presencia de la voz no se tocan.
- Tras cada ese, la reducción se suelta rápido para que la vocal siguiente no pierda agudos (medido: 0,0 dB de cambio).

## Precision (detección fina por espectro)
Con Precision alto el plugin mira el espectro real de cada ese y atenúa solo las frecuencias donde esa ese tiene energía
(varios picos a la vez, p. ej. "sh" + "s"); los valles entre picos y lo que no es sibilancia no se tocan.
Precision 0 % = zona suave (campana) como antes; 100 % = solo los puntos exactos. Width controla el suavizado de los bordes.

## Motor de dos caminos (frecuencia exacta + reacción rápida)
- **Camino rápido** (ventana de 5 ms): decide CUÁNDO y CUÁNTO (detección, umbral, Attack/Release).
- **Camino fino** (ventana de 21 ms, resolución de 47 Hz): decide DÓNDE; encuentra las frecuencias exactas de cada ese
  y extrae solo ese componente. La reducción solo toca ese componente: sin ese, la señal sale idéntica (error -300 dB).

## Eses rápidas (v21)
Una ese de 10-30 ms tiene un espectro muy ruidoso: la máscara espectral saltaba de un frame al siguiente y los flancos de la ese
quedaban sin atenuar (el centro se cortaba, pero 6-7 kHz y 9-10 kHz casi no). Dos cambios en `computeSpecMask` (`DeEsserEngine.h`):
- **Máscara con subida instantánea y caída lenta** (`kMaskHoldMs` = 20 ms): cualquier frecuencia que muestre sibilancia en la ese
  queda cubierta durante toda la ese.
- **Cobertura según profundidad** (`kDepthWiden` = 4 dB): cuanto más reducción se pide, más ancho el criterio de qué frecuencias
  cuentan como ese.
Medido en CASI_SIEMPRE_-_MELODIA.wav (ese de ~1,28 s, ajustes por defecto): reducción 5-11 kHz de 8 dB a 13,6 dB; con Range 30, de 8 a 18,7 dB.
Sin ese, la salida sigue siendo idéntica (null test -300 dB, todos los tests pasan).

## Eses muy agudas y muy rápidas (v22)
Tres problemas medidos con eses sintéticas (voz sorda sobre vocal, `Tests/engine_test.cpp > testSharpFast`):
- **Por encima de ~11 kHz la ese no se veía ni se atenuaba.** El detector solo buscaba picos hasta 11 kHz, el tracker se detenía en 12 kHz y la
  protección de aire (Air Keep) cerraba la zona a partir de 10,5-12,5 kHz. Ahora el pico se busca hasta 15 kHz (`kPeakMaxHz`), el nivel de la ese
  usa también el pico de sub-banda (`sibDb`), el tracker llega a 15 kHz y la protección de aire empieza en `max(10,5 kHz, 1,1 x fc)`: para fc < 9,5 kHz
  todo queda igual. Medido con fc = 12 kHz: reducción de 2 dB a ~10 dB (Range 12).
- **Attack con "salto" al detectar la ese.** La reducción arrancaba al 60 % de su profundidad y subía 10 % por frame (`1 - 0,1 x j` en el lookahead),
  y una ese corta o débil perdía otro 20-40 % por la confirmación. Ahora arranca al ~92 % (`kLookaheadDerate` = 0,025), la confirmación nunca baja
  del 85 % (`kConfirmFloor`) y la puerta de score se abre antes (`kGateLo/Hi` = 0,14/0,40). El primer escalón pasa de ~6,7 dB a ~10 dB en una ese de 10 ms.
- **Frecuencia que llegaba tarde a la ese.** El tracker promediaba con el mismo peso los frames parciales del inicio (picos ruidosos) y luego suavizaba
  3 ms. Ahora pondera por score al cuadrado y, mientras hay una ese, suaviza en 1 ms (`kFcEventSmoothMs`).

Límites conocidos: una ese de 3 ms y mucho más débil que la media sigue reduciéndose menos (la ventana rápida es de 5,3 ms), y a 96 kHz las eses
más cortas (5 ms) a 12 kHz quedan algo por debajo de las de 48 kHz. La ley de reducción (`GainComputer`) no se ha tocado: llega al Range completo
con 5 dB de exceso sobre el umbral, así que el paso de "no reduce" a "reduce todo" es corto por diseño.
