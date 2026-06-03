# Care Mic Firmware

Firmware voor de Care Geluidsmonitor op M5Stack Atom EchoS3R C126-ECHO.

## Mappen

```text
firmware/stable.json
firmware/beta.json
src/care_mic_echos3r_v1_6_ota.cpp
```

## Stable en beta updatekanaal

De sensor kijkt naar een van deze bestanden:

```text
https://raw.githubusercontent.com/JOUW_GITHUB_GEBRUIKER/care-mic-firmware/main/firmware/stable.json
https://raw.githubusercontent.com/JOUW_GITHUB_GEBRUIKER/care-mic-firmware/main/firmware/beta.json
```

In de webpagina van de sensor moet de Update basis URL staan als:

```text
https://raw.githubusercontent.com/JOUW_GITHUB_GEBRUIKER/care-mic-firmware/main/firmware
```

Daarna kiest de sensor zelf:

```text
stable.json
```

of

```text
beta.json
```

## Release maken

1. Compileer in Arduino IDE:
   `Sketch -> Export Compiled Binary`

2. Maak in GitHub een release:
   `v1.6.0`

3. Upload het .bin bestand als:
   `care-mic-echos3r-stable.bin`

4. Controleer of `firmware/stable.json` naar dezelfde release en bestandsnaam wijst.

## Belangrijk

Vervang overal:

```text
JOUW_GITHUB_GEBRUIKER
```

door jouw echte GitHub gebruikersnaam of organisatienaam.
