# SymphonyRecomp para Nintendo 3DS (modelo original)

Port de SymphonyRecomp a la 3DS **original** (O3DS / O2DS: ARM11 a 268 MHz, 64 MB).
No depende de nada exclusivo de la New 3DS.

## Cómo funciona

El proyecto original genera C# y corre sobre .NET, que no existe en la 3DS. Este port:

1. **`cgen/`** – herramienta en C# que reutiliza el análisis de RecompOne (detección de
   funciones, tablas de saltos, parches del SDK) sin modificar el submódulo, y emite
   **C** en lugar de C# (`build/gen/*.c`, ~60 MB de C, ~14.300 funciones).
2. **`runtime/`** – reimplementación en C del runtime de RecompOne:
   memoria (RAM de 2 MB con detección de carga de overlays), dispatcher de overlays,
   BIOS HLE (A/B/C, heap, eventos, tarjeta de memoria), libcd / streaming / XA,
   GTE, MDEC, SPU (24 voces + XA), CD-ROM, DMA, timers y una **GPU por software**
   con rutas rápidas especializadas para el ARM11.
3. **`runtime/host/host_3ds.c`** – capa de plataforma con libctru: pantalla superior
   (RGB565), controles, audio NDSP, y un hilo de GPU en el core 1 cuando el sistema
   lo permite.
4. **`runtime/host/host_pc.c`** – host de prueba para PC (SDL2), útil para depurar
   rápido sin emulador (modo sin ventana con volcado de frames y entrada por guion).

## Compilar

Requisitos: devkitPro (devkitARM + libctru), SDK de .NET 10, Python 3 con Pillow
(solo para inspeccionar capturas). La imagen del disco va en `../disc/` como indica el
README principal.

```sh
cd port3ds
./build_3ds.sh          # genera build/3ds/SymphonyRecomp.3dsx (y el .cia, ver abajo)
./build_3ds.sh pc       # además compila el host de prueba de PC
```

Para el `.cia` hacen falta `makerom` (releases de 3DSGuy/Project_CTR) y `bannertool`
(releases de diasurgical/bannertool) en `build/tools/` (`makerom.exe` y
`bt/windows-x86_64/bannertool.exe`). El CIA usa el modo de memoria normal (*Prod*,
64 MB) y su configuración está en `cia/app.rsf`.

## Instalar en la SD

```
sdmc:/cias/SymphonyRecomp.cia      (instalar con FBI; recomendado)
sdmc:/3ds/SymphonyRecomp.3dsx      (alternativa desde el Homebrew Launcher)
sdmc:/3ds/sotn/Castlevania - Symphony of the Night (USA).cue
sdmc:/3ds/sotn/Castlevania - Symphony of the Night (USA) (Track 1).bin
sdmc:/3ds/sotn/Castlevania - Symphony of the Night (USA) (Track 2).bin
```

Instalado como CIA tiene toda la memoria de aplicación. El `.3dsx` se abre desde el
Homebrew Launcher **en modo título** (en Luma3DS: mantener R al abrir
un juego instalado), porque necesita casi toda la memoria de aplicación de la O3DS
(64 MB): el ejecutable ocupa ~39 MB (33 MB de código del juego recompilado + 6 MB de
BSS) y en ejecución se reservan otros ~15 MB (pila del juego, audio, buffers). Lanzado
como applet (p. ej. desde el álbum) no hay memoria suficiente.

Las partidas se guardan en `sdmc:/3ds/sotn/carda.sav` (formato de tarjeta de memoria de
PSX) y el registro en `sdmc:/3ds/sotn/log.txt`.

## Controles

| 3DS | PlayStation |
|-----|-------------|
| B | Cruz (saltar) |
| A | Círculo |
| Y | Cuadrado |
| X | Triángulo |
| L / R | L1 / R1 |
| Tocar mitad izquierda / derecha de la pantalla táctil | L2 / R2 |
| ZL / ZR (solo New 3DS) | L2 / R2 |
| START / SELECT | START / SELECT |
| SELECT + START | mostrar FPS en la pantalla inferior |

## Estado

Probado en Azahar en modo 3DS original con una partida guionizada que va de la intro
al prólogo con Richter, la pelea con Drácula (ambas formas) y Alucard en la entrada del
castillo. Falta probarlo en hardware real.

Rendimiento medido en Azahar (O3DS, un solo núcleo). "Bruto" es sin frameskip ni
límite de 60 Hz (`bench.txt`):

| Escena | En juego (FPS mostrados / lógica) | Bruto |
|--------|-----------------------------------|-------|
| Título y menús | 60 / 60 | |
| Prólogo (Richter) | 60 / 60 | ~84 |
| Pelea con Drácula | 60 / 60 | 66-68 |
| Explosión final de Drácula | ~27 / 40 | |
| Entrada del castillo con zombis en llamas (Alucard) | 42-60 / 60 | 50-70 |
| Salas del castillo | 60 / 60 | ~105 |
| FMV (intro y prólogo) | 15-16, la tasa nativa de los videos | |

- **Frameskip automático** (hasta 2 frames seguidos) solo cuando rasterizar es lo caro;
  la lógica del juego sigue a 60 Hz. Como el juego usa doble búfer, se presenta según
  si se dibujó el frame anterior (presentar el búfer no dibujado congelaba la imagen).
- En hardware real la GPU reparte la rasterización entre los dos núcleos (franjas de
  filas balanceadas dinámicamente) y el audio corre en el núcleo 1. Azahar mantiene los
  hilos de un `.3dsx` en el núcleo 0, así que ahí el runtime lo detecta y usa uno solo.
- Los parches del proyecto de PC que dependen de C# (pantalla ancha, randomizer, trucos,
  calidad de vida) no están portados. Sí lo están las cuatro correcciones de errores del
  juego de `patches/qol/FunctionFixes.cs`.
- Sin dithering en la ruta rápida de la GPU (la salida de la 3DS es RGB565).

### Notas de implementación

- Los accesos a memoria del código recompilado se expanden en línea solo para la RAM;
  scratchpad y registros de hardware van por una función. Expandir más (p. ej. vigilar
  cada escritura) duplicaba el tamaño del binario.
- Los overlays (escenarios, armas, familiares) se detectan al leerlos del CD y se activan
  en la primera llamada a su rango de direcciones, verificando con una firma de sus
  primeros 256 bytes que el overlay anterior ya no está en RAM.
- Las optimizaciones de GPU, MDEC y SPU se verificaron bit a bit contra la versión de
  referencia con el host de PC (`RT_VCLOCK=1` hace las corridas deterministas y
  `RT_PCM` captura el audio).
- Si `sdmc:/3ds/sotn/bench.txt` existe, no hay frameskip ni límite de 60 Hz: el contador
  de FPS muestra el rendimiento bruto. `log.txt` registra cada 5 s los FPS, el tiempo
  por subsistema, el peor frame y los frames de más de 20 ms.
- Ayudas para partidas guionizadas (las mismas en PC y 3DS): `autoinput.txt`
  (`frame:máscara,...`, botones PSX en hex), `vclock.txt` (streaming del CD por frames,
  determinista) y `onehit.txt` (los enemigos quedan con 1 de vida). En PC:
  `RT_INPUT`, `RT_VCLOCK=1`, `RT_CHEAT_ONEHIT=1`, `RT_CHEAT_UNTIL=n`, `RT_FORCE_SKIP=n`.
- Optimizaciones de la GPU por software: rutas especializadas por profundidad de textura,
  modulación y modo de mezcla; quads 1:1 dibujados como sprites; paletas y tablas de
  modulación en caché; mosaicos de 4 bits con una búsqueda por cada dos texels; mezclas
  semitransparentes de a dos píxeles. Todas verificadas bit a bit en el host de PC.

## Variables útiles del host de PC

`RT_HEADLESS=1`, `RT_DUMP=n` (volcar un BMP cada n frames), `RT_FRAMES=n`,
`RT_INPUT=frame:mascara,...` (botones PSX en hex), `RT_NO_GPU_THREAD=1`,
`RT_VCLOCK=1` (reloj virtual: un frame por VSync), `RT_PCM=archivo` (audio s16 estéreo),
`RT_NO_IO_THREAD=1`.
