# Wincode Node Hardware

Circuit diagrams and ESP32 firmware for the nodes that the
[Wincode Node Manager](https://github.com/samcho93/wincodeNodeManager) finds, configures and watches.

**Documentation (diagrams, pins, parts, bring-up):** https://samcho93.github.io/wincodeNodeHardware/

| Device | What it is |
|---|---|
| **WN-WINCH** | Kinetic winch: closed-loop stepper with brake on a 16-bit DMX position, a 60-pixel lamp on the cable, limit switches, hardware E-stop chain, homing |
| **WN-PIX4** | Pixel node: four WS2812B / WS2811 / SK6812 / SK6812-RGBW / UCS2903 outputs |

Both are the same **core board** (ESP32-WROOM-32E, LAN8720 Ethernet, 24 V in, USB-C programming) with the I/O of
their type. A WT32-ETH01 module matches the core's pins for a first prototype.

## Layout

```
firmware/      ESP-IDF 5.5 project — one source, two builds (winch, pixel node)
  main/        wn_main · wn_net · wn_config · wn_api · wn_dmx · wn_pixels · wn_winch · wn_pixelnode · Kconfig.projbuild
site/          the documentation page (GitHub Pages) and the diagrams in site/img/
tools/         diagrams.py — draws site/img/*.svg
```

## Build

```powershell
$env:IDF_TOOLS_PATH = "D:\ESP\.espressif"; . D:\ESP\.espressif\v5.5\esp-idf\export.ps1
cd firmware
idf.py build                                   # winch
idf.py -B build-pixel -D SDKCONFIG=sdkconfig.pixel -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pixel" build   # pixel node
idf.py -p COM4 flash monitor
```

Pins, Wi-Fi, steps per millimetre: `idf.py menuconfig` → *Wincode node*.

## What the firmware implements

- The device API of the Node Manager, version 1
  ([DEVICE-API.md](https://github.com/samcho93/wincodeNodeManager/blob/main/docs/DEVICE-API.md)): discovery on
  UDP 7788, `GET /api/info`, `GET`/`PUT /api/config` (deep-merged patch, stored in NVS), `GET /api/status`,
  `POST /api/action`.
- Art-Net (ArtDmx, ArtPoll replies) and sACN / E1.31 multicast, 2.5 s signal loss.
- The same behaviour as the Node Manager's virtual devices (`vdevice.js`): pixel mapping, winch ramp and manual control.
  Its *Simulator* tab shows what this hardware should do with the same DMX.

## Status

Both builds compile with ESP-IDF 5.5. **Nothing has run on hardware yet.** The diagrams are block diagrams with parts
and pins; a KiCad schematic and PCB are the next step. Not built yet: APA102 pixels, the eDMX node and the hub,
network firmware update, RDM.

A winch lifts loads: the E-stop chain must cut motor and brake power in hardware, and a real product needs a
certified hoist design. These files are a development reference.
