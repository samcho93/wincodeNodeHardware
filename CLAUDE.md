# Wincode Node Hardware

Circuit diagrams + ESP32 firmware for the nodes of the Wincode Node Manager (`D:\Work\Web\wincodeNodeManager`). Details: [README.md](README.md).

## Build
- ESP-IDF **5.5** (`D:\ESP\.espressif\v5.5\esp-idf`, `IDF_TOOLS_PATH=D:\ESP\.espressif`), target esp32.
- Winch: `idf.py build` (default `sdkconfig`). eDMX node: same with `edmx`. Pixel node: `-B build-pixel -D SDKCONFIG=sdkconfig.pixel -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.pixel"`.
  In PowerShell call `python $env:IDF_PATH\tools\idf.py` with an argument array: the `idf.py` shell function splits `-D`.
- Warnings are errors (`-Werror=misleading-indentation` etc.): one statement per `if` line.

## Rules
- The firmware implements `wincodeNodeManager/docs/DEVICE-API.md`; the executable reference is `wincodeNodeManager/web/js/vdevice.js`. Change the three together: config fields and ranges, status fields, actions, pixel mapping (`renderPixels`), winch behaviour.
- Pin defaults live in `firmware/main/Kconfig.projbuild` and are drawn by `tools/diagrams.py` → `site/img/*.svg`. Change both, regenerate, and check that no label overflows (the layout is absolute).
- Motion safety stays in the firmware, never in the manager: limits in the step ISR, jog time-out, brake closed when standing, homing before DMX. The E-stop is hardware.
- All text and comments in English. `site/` is published by GitHub Pages.
