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

Se abre desde el Homebrew Launcher. Las partidas se guardan en `sdmc:/3ds/sotn/carda.sav`
(formato de tarjeta de memoria de PSX) y el registro en `sdmc:/3ds/sotn/log.txt`.

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
entrada de nombre y prólogo jugable (Richter en el castillo de Drácula).

- El juego corre a velocidad completa (60 ciclos de lógica por segundo) con
  **frameskip automático** (hasta 2 frames seguidos); en Azahar se muestran ~25-30 FPS
  en el prólogo.
- En hardware real la GPU reparte la rasterización entre los dos núcleos (franjas de
  filas balanceadas dinámicamente). Azahar mantiene los hilos de un `.3dsx` en el core 0,
  así que ahí el runtime lo detecta y usa un solo núcleo.
- Los parches del proyecto de PC que dependen de C# (pantalla ancha, randomizer, trucos,
  calidad de vida) no están portados. Sí lo están las cuatro correcciones de errores del
  juego de `patches/qol/FunctionFixes.cs`.
- Sin dithering en la ruta rápida de la GPU (la salida de la 3DS es RGB565).

## Variables útiles del host de PC

`RT_HEADLESS=1`, `RT_DUMP=n` (volcar un BMP cada n frames), `RT_FRAMES=n`,
`RT_INPUT=frame:mascara,...` (botones PSX en hex), `RT_NO_GPU_THREAD=1`.
