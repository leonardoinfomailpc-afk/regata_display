# CrowPanel 4.3 Hello World

Questo progetto base mostra la scritta `Hello World` sul display CrowPanel ESP32 HMI da 4.3 pollici.

## Informazioni ricavate dai file presenti

Dal manuale locale `ESP32_Display_HMI_User_Manual.pdf`:
- modello 4.3": ESP32-S3-WROOM-1-N4R2
- risoluzione: 480x272
- driver display: NV3047
- link wiki ufficiale: https://www.elecrow.com/wiki/CrowPanel_ESP32_HMI_Wiki_Content.html

Dalla pagina wiki 4.3":
- pin mapping RGB panel
- timing RGB panel
- libreria consigliata: Arduino_GFX

## Come compilare e caricare con PlatformIO

1. Apri questa cartella come progetto PlatformIO.
2. Il progetto usa la piattaforma pioarduino (core ESP32 3.x) per compatibilita con la libreria grafica.
2. Collega il display via USB-C (porta UART0).
3. Esegui:

```bash
pio run -e crowpanel_43
pio run -e crowpanel_43 -t upload
pio device monitor -b 115200
```

## Note upload

Se il caricamento fallisce:
1. tieni premuto BOOT
2. premi RESET
3. rilascia RESET
4. rilascia BOOT
5. ripeti upload

## Procedura debug ordinata (consigliata)

Il progetto ora supporta debug a stadi tramite `CROW_DEBUG_STAGE` in `platformio.ini`.

Valori disponibili:
- `0`: seriale only (nessun init display)
- `1`: scan retroilluminazione (pin 2 e 38)
- `2`: init display senza draw
- `3`: init display + testo Hello World

### Step 1 - Verifica vita MCU
1. Imposta `-DCROW_DEBUG_STAGE=0` in `platformio.ini`.
2. Esegui upload.
3. Apri monitor seriale a 115200.
4. Premi RESET.

Atteso:
- banner `CrowPanel Boot Diagnostic`
- heartbeat `alive t=...`

Se non appare nulla, il problema e prima del display (porta seriale, boot mode, reset, alimentazione o cavo).

### Step 2 - Verifica backlight
1. Imposta `-DCROW_DEBUG_STAGE=1`.
2. Upload + monitor seriale.

Atteso:
- log `Backlight probe pin HIGH: 2` e `...: 38` alternati
- possibile variazione visiva della retroilluminazione

### Step 3 - Verifica init RGB
1. Imposta `-DCROW_DEBUG_STAGE=2`.
2. Upload + monitor seriale.

Atteso:
- `Initializing display...`
- `Display init OK` oppure `Display init failed`

### Step 4 - Draw finale
1. Imposta `-DCROW_DEBUG_STAGE=3`.
2. Upload + monitor seriale.

Atteso:
- schermo nero con scritta `Hello World`
