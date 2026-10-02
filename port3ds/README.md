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
./build_3ds.sh          # genera build/3ds/SymphonyRecomp.3dsx
./build_3ds.sh pc       # además compila el host de prueba de PC
```

## Instalar en la SD

```
sdmc:/3ds/SymphonyRecomp.3dsx
sdmc:/3ds/sotn/Castlevania - Symphony of the Night (USA).cue
sdmc:/3ds/sotn/Castlevania - Symphony of the Night (USA) (Track 1).bin
sdmc:/3ds/sotn/Castlevania - Symphony of the Night (USA) (Track 2).bin
```

Se abre desde el Homebrew Launcher **en modo título** (en Luma3DS: mantener R al abrir
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

Probado en Azahar en modo 3DS original: intro FMV, logo, título, selección de archivo,
entrada de nombre y prólogo jugable (Richter en el castillo de Drácula). Falta probarlo
en hardware real.

Rendimiento medido en Azahar (O3DS, un solo núcleo):

| Escena | FPS |
|--------|-----|
| Título y menús | 60 |
| Prólogo jugable (Richter) | 55-60 (sin frameskip: ~59) |
| Pelea con Drácula del prólogo | sin frameskip: 45-50 |
| Alucard en la entrada del castillo | sin frameskip: 60-95 |
| FMV (intro y prólogo) | 15-16, la tasa nativa de los videos |

- **Frameskip automático** (hasta 2 frames seguidos) solo cuando rasterizar es lo caro;
  la lógica del juego siempre corre a 60 Hz.
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
  de FPS muestra el rendimiento bruto.

## Variables útiles del host de PC

`RT_HEADLESS=1`, `RT_DUMP=n` (volcar un BMP cada n frames), `RT_FRAMES=n`,
`RT_INPUT=frame:mascara,...` (botones PSX en hex), `RT_NO_GPU_THREAD=1`,
`RT_VCLOCK=1` (reloj virtual: un frame por VSync), `RT_PCM=archivo` (audio s16 estéreo),
`RT_NO_IO_THREAD=1`.
