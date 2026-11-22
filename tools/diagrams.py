"""diagrams.py — draws the circuit block diagrams of the Wincode nodes as SVG (site/img/*.svg).

    python tools/diagrams.py

The pin numbers here are the defaults of firmware/main/Kconfig.projbuild: change both together.
"""
import os

OUT = os.path.join(os.path.dirname(__file__), '..', 'site', 'img')
C = {'core': '#1d6fd1', 'power': '#c0392b', 'net': '#16a085', 'io': '#8e44ad', 'motor': '#d35400', 'safe': '#b00020', 'pix': '#2c3e50', 'ext': '#555'}


class Sheet:
    def __init__(self, w, h, title):
        self.w, self.h, self.parts = w, h, []
        self.text(20, 30, title, 18, 'bold')

    def text(self, x, y, s, size=12, weight='normal', color='#222', anchor='start'):
        s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        self.parts.append(f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{color}" text-anchor="{anchor}">{s}</text>')

    def box(self, x, y, w, h, title, lines=(), kind='core', dashed=False):
        col = C[kind]
        dash = ' stroke-dasharray="6 4"' if dashed else ''
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="#fff" stroke="{col}" stroke-width="2"{dash}/>')
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="22" rx="6" fill="{col}"/><rect x="{x}" y="{y + 14}" width="{w}" height="8" fill="{col}"/>')
        self.text(x + 8, y + 16, title, 12, 'bold', '#fff')
        for i, s in enumerate(lines):
            self.text(x + 8, y + 38 + i * 15, s, 11)
        return (x, y, w, h)

    def wire(self, pts, label=None, kind='core', lx=None, ly=None, arrow=True, width=1.6):
        col = C[kind]
        d = 'M' + ' L'.join(f'{x},{y}' for x, y in pts)
        m = f' marker-end="url(#a-{kind})"' if arrow else ''
        self.parts.append(f'<path d="{d}" fill="none" stroke="{col}" stroke-width="{width}"{m}/>')
        if label:
            (x0, y0), (x1, y1) = pts[0], pts[1]
            self.text(lx if lx is not None else (x0 + x1) / 2 + 4, ly if ly is not None else (y0 + y1) / 2 - 4, label, 10, 'normal', col)

    def save(self, name):
        marks = ''.join(f'<marker id="a-{k}" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="{v}"/></marker>' for k, v in C.items())
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}" width="{self.w}" height="{self.h}" font-family="Segoe UI, Arial, sans-serif">'
               f'<defs>{marks}</defs><rect width="100%" height="100%" fill="#fbfbfc"/>' + ''.join(self.parts) + '</svg>')
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, name), 'w', encoding='utf-8') as f:
            f.write(svg)


def core():
    s = Sheet(1100, 620, 'Core board — shared by every Wincode node (ESP32 + Ethernet + power)')
    s.box(20, 60, 200, 110, 'J1  24 V DC in', ['Phoenix MSTB 2-pin', 'P-MOSFET reverse protection', 'SMBJ30A TVS · 2 A polyfuse'], 'power')
    s.box(270, 60, 200, 110, 'U1  buck 24 V → 5 V', ['MP2315 / TPS54331, 2 A', '22 µH · 2 × 22 µF', '5 V for level shifters, CH340'], 'power')
    s.box(520, 60, 200, 110, 'U2  3.3 V', ['AMS1117-3.3 (or AP2112K)', '1 A, from 5 V', '10 µF in / 22 µF out'], 'power')
    s.wire([(220, 115), (270, 115)], '24 V', 'power')
    s.wire([(470, 115), (520, 115)], '5 V', 'power')
    s.box(380, 230, 260, 250, 'U3  ESP32-WROOM-32E (4 MB)', [
        'EN: 10 k + 1 µF, auto-reset from CH340', 'GPIO0 ← REF_CLK 50 MHz (boot: held high)',
        'RMII: TXD0 19 · TXD1 22 · TX_EN 21', '      RXD0 25 · RXD1 26 · CRS_DV 27', 'SMI: MDC 23 · MDIO 18',
        'GPIO16 → oscillator enable', 'GPIO32 (ADC1 4) ← V_IN / 11', 'GPIO33 (ADC1 5) ← NTC 10 k B3950',
        'GPIO15 ← button (to GND)', 'Status LED: GPIO14 (winch) / GPIO2 (pixel)',
        'Device I/O: see the winch and pixel sheets', '3.3 V: 10 µF + 100 nF at the module'], 'core')
    s.wire([(620, 170), (620, 230)], '3.3 V', 'power', 626, 205)
    s.box(720, 230, 210, 140, 'U4  LAN8720A', ['RMII PHY, address 1', '50 MHz oscillator, enable', '  from GPIO16 (boot-safe)', 'nINTSEL / REFCLKO: osc mode', '12.1 k RBIAS 1 %'], 'net')
    s.box(960, 230, 120, 140, 'J2  RJ45', ['HR911105A', 'magnetics', 'built in', 'link / act', 'LEDs'], 'net')
    s.wire([(640, 290), (720, 290)], 'RMII', 'net', 660, 284)
    s.wire([(720, 330), (640, 330)], 'REF_CLK', 'net', 652, 344)
    s.wire([(930, 300), (960, 300)], '', 'net')
    s.box(20, 230, 300, 130, 'U5  USB-C programming', ['CH340C (no crystal), 5 V from USB or U1', 'DTR / RTS → 2 × S8050 → EN, GPIO0', 'TX0 GPIO1 · RX0 GPIO3', 'CC1 / CC2: 5.1 k to GND'], 'io')
    s.wire([(320, 300), (380, 300)], 'UART0', 'io', 330, 294)
    s.box(20, 400, 300, 180, 'Board sensing', ['V_IN: 100 k / 10 k → GPIO32, 100 nF', 'NTC 10 k (next to the power stage)', '  with 10 k to 3.3 V → GPIO33', 'Button: GPIO15 to GND, 10 k pull-up', '  held 5 s at power-up = factory', 'Status LED: 1 k, GPIO14 / GPIO2'], 'io')
    s.wire([(320, 470), (380, 470)], 'ADC / GPIO', 'io', 326, 464)
    s.text(380, 520, 'Strapping pins: GPIO0 (REF_CLK input — the oscillator is enabled by GPIO16 after boot), GPIO2, GPIO5, GPIO12, GPIO15.', 11, color='#555')
    s.text(380, 538, 'GPIO12 stays unused (it would select 1.8 V flash if pulled high at boot). GPIO34–39 are inputs only, without pull-ups.', 11, color='#555')
    s.text(380, 556, 'Wi-Fi works as a fallback; show data at 40 frames per second wants the cable.', 11, color='#555')
    s.save('core.svg')


def winch():
    s = Sheet(1200, 820, 'WN-WINCH — kinetic winch (core board + motion I/O + lamp output)')
    s.box(20, 60, 230, 300, 'Core board (ESP32)', ['GPIO4  → STEP', 'GPIO5  → DIR', 'GPIO17 → ENABLE', 'GPIO2  → BRAKE release', 'GPIO13 → lamp data', 'GPIO34 ← top limit', 'GPIO35 ← bottom limit', 'GPIO36 ← driver alarm', 'GPIO39 ← E-stop chain sense', '', 'GPIO14 status LED', 'GPIO15 button'], 'core')
    s.box(320, 60, 210, 130, 'U10  74HCT245 at 5 V', ['3.3 V in → 5 V out', 'STEP, DIR, ENABLE, lamp', 'OE tied low; DIR high', '100 nF at VCC'], 'io')
    for i, (y, lab) in enumerate([(90, 'STEP'), (115, 'DIR'), (140, 'ENA')]):
        s.wire([(250, y), (320, y)], lab, 'io')
    s.box(600, 60, 280, 150, 'Stepper driver (external)', ['closed loop, e.g. HBS86H / CL86T', 'PUL+ DIR+ ENA+ ← 74HCT245 (5 V)', 'PUL− DIR− ENA− → GND', '1600 steps / rev (12.73 steps / mm', '  on a 40 mm drum)', 'ALM (open collector) → U12'], 'motor')
    s.wire([(530, 100), (600, 100)], '', 'motor')
    s.box(940, 60, 220, 150, 'M1  NEMA 34 + brake', ['hybrid stepper + encoder', '24 V spring-applied brake', 'drum 40 mm, wire rope', 'gearbox optional: change', '  steps / mm in menuconfig'], 'motor')
    s.wire([(880, 120), (940, 120)], '', 'motor')
    s.box(320, 230, 210, 110, 'Q10  brake switch', ['AO3400 N-MOSFET, gate 100 Ω', '  + 100 k to GND (closed at boot)', 'SS34 flyback diode', '→ M1 brake coil (24 V via K2)'], 'motor')
    s.wire([(250, 160), (290, 160), (290, 270), (320, 270)], 'BRAKE', 'motor', 256, 154)
    s.box(320, 380, 210, 150, 'U11–U14  PC817 inputs', ['24 V side: 2.2 k + LED', 'ESP32 side: 10 k pull-up', '  to 3.3 V, 100 nF', 'low = active (fail-safe:', '  a cut wire reads "pressed")'], 'io')
    s.wire([(320, 420), (250, 420), (250, 250)], '', 'io', arrow=True)
    s.text(244, 350, 'GPIO34·35·36·39', 10, color=C['io'], anchor='end')
    s.box(600, 380, 260, 110, 'Limit switches', ['S1 top (home reference), NC', 'S2 bottom (over-travel), NC', 'mechanical, 24 V, wired to the', '  drum frame'], 'safe')
    s.wire([(600, 430), (530, 430)], '24 V NC', 'safe', 545, 424)
    s.box(600, 520, 260, 200, 'E-stop chain (hardware)', ['NC buttons in series, 24 V', 'K1 safety relay (e.g. PNOZ s3 /', '  PSR-ESAM4), two channels', 'K2 contactor: motor supply AND', '  brake supply — cut when the', '  chain opens, whatever the', '  firmware does', 'K1 auxiliary contact → U14 → GPIO39'], 'safe')
    s.wire([(600, 600), (560, 600), (560, 500), (530, 500)], 'aux', 'safe', 566, 594)
    s.box(940, 260, 220, 130, 'Motor supply', ['48 V DC 7 A (driver input)', 'through K2 main contacts', '24 V for the brake through K2'], 'power')
    s.wire([(860, 640), (1180, 640), (1180, 340), (1160, 340)], 'K2 cuts motor and brake power', 'safe', 880, 634)
    s.wire([(940, 320), (870, 320), (870, 210)], '48 V', 'power', 876, 300)
    s.box(940, 420, 220, 150, 'Lamp on the cable', ['60 × WS2812B, 5 V', '74HCT245 → 33 Ω → data', '5 V 4 A buck on the lamp', '  (24 V down the cable)', 'cable: 3-core + data pair'], 'pix')
    s.wire([(530, 170), (580, 170), (580, 360), (900, 360), (900, 470), (940, 470)], 'lamp data (GPIO13)', 'pix', 590, 352)
    s.box(20, 400, 230, 160, 'Front panel', ['RJ45 (core board)', 'Status LED', 'Button (factory reset)', 'Motor: Speakon / Harting', 'Lamp: 4-pin XLR', 'E-stop: 5-pin XLR (loop)'], 'ext', dashed=True)
    s.text(20, 740, 'Safety is layered: the E-stop chain cuts motor and brake power in hardware; the firmware stops on the limit switches inside', 11, color='#555')
    s.text(20, 756, 'the step interrupt, clamps every target to the travel limits, ends a jog 1 s after its last command and closes the brake', 11, color='#555')
    s.text(20, 772, 'when the motor stands. Rigging over people needs a certified hoist design (e.g. DGUV V17 / BGV C1, IEC 60204-32): this', 11, color='#555')
    s.text(20, 788, 'diagram is a development reference, not a certified design.', 11, 'bold', '#b00020')
    s.save('winch.svg')


def pixel():
    s = Sheet(1180, 620, 'WN-PIX4 — pixel node, 4 outputs (core board + level shifter + power distribution)')
    s.box(20, 60, 230, 200, 'Core board (ESP32)', ['GPIO4  → output 1', 'GPIO5  → output 2', 'GPIO13 → output 3', 'GPIO14 → output 4', '', 'GPIO2  status LED', 'GPIO15 button', 'RMT: one channel per output'], 'core')
    s.box(320, 60, 230, 160, 'U20  74HCT245 at 5 V', ['four channels, 3.3 V → 5 V', 'OE low, DIR high, 100 nF', 'each output: 33 Ω series', '  + 100 Ω to the connector', '  (ringing on long data lines)'], 'io')
    for i, y in enumerate([100, 125, 150, 175]):
        s.wire([(250, y), (320, y)], f'D{i + 1}', 'io')
    s.box(640, 60, 250, 280, 'J20–J23  outputs', ['4 × 3-pin (V+, data, GND)', 'per output: ATO fuse 7.5 A', '  and a green LED on V+', 'GND shared with the logic', 'data: up to 10 m to the first', '  pixel; further with a', '  differential extender', '', 'max per output:', '  4096 pixels in config; real', '  limit = 40 fps × wire time'], 'pix')
    s.wire([(550, 140), (640, 140)], 'data 1–4', 'pix', 562, 134)
    s.box(960, 60, 200, 280, 'Pixel strips', ['WS2812B / WS2811 / SK6812', 'SK6812-RGBW, UCS2903', '(APA102 needs a clock line:', '  not built in this revision)', '', '5 V: inject every 2 m', '12 V: every 5 m', '', '170 RGB / 127 RGBW pixels', '  per universe'], 'ext', dashed=True)
    s.wire([(890, 200), (960, 200)], '', 'pix')
    s.box(320, 300, 230, 160, 'Pixel power', ['J24: 5 V or 12 V in, 2 × M5', '  ring terminals, up to 40 A', 'distributed through the four', '  fuses; logic does NOT take', '  current from this rail'], 'power')
    s.wire([(550, 380), (600, 380), (600, 300), (640, 300)], 'V+ 40 A', 'power', 560, 374)
    s.box(20, 300, 230, 160, 'Logic power', ['J1 on the core board:', '  12–24 V DC, 0.5 A', '(or a 5 V-in variant: fit a', '  jumper instead of U1)', 'separate from the pixel rail'], 'power')
    s.wire([(250, 380), (320, 380)], '', 'power', arrow=False)
    s.text(20, 510, 'The firmware maps channels exactly like the Node Manager simulator (renderPixels() in vdevice.js): R, G, B (, W) per controlled', 11, color='#555')
    s.text(20, 526, 'pixel; the first universe starts at startChannel, every following universe at channel 1; group, reverse, limit and gamma are', 11, color='#555')
    s.text(20, 542, 'applied on the node; `order` only says how the chip wants the colours on the wire.', 11, color='#555')
    s.text(20, 566, 'Power budget: 60 mA per WS2812B pixel at full white — 300 pixels = 18 A at 5 V. Use `limit` to stay inside the supply and the fuses.', 11, 'bold', '#222')
    s.save('pixel.svg')


def edmx():
    s = Sheet(1200, 700, 'WN-DMX2 — eDMX node, 2 DMX512 ports (core board + isolated RS-485 per port)')
    s.box(20, 60, 230, 250, 'Core board (ESP32)', ['UART1  TX GPIO4 · RX GPIO35', '       DE GPIO5', 'UART2  TX GPIO13 · RX GPIO36', '       DE GPIO14', '', 'GPIO2  status LED', 'GPIO15 button', '250 kbit/s 8N2, break by', '  inverting TX (120 µs)'], 'core')
    for k, y0 in enumerate([60, 330]):
        n = k + 1
        s.box(330, y0, 260, 200, f'U3{n}  ADM2587E (port {n})', ['isolated RS-485 transceiver', '  with its own isolated 5 V', '3.3 V logic side: DI ← TX,', '  RO → RX, DE + /RE ← DE pin', 'bus side: A, B, isolated GND', '2.5 kV isolation (the DMX', '  line never reaches the ESP32)'], 'io')
        s.wire([(250, 100 + k * 120), (290, 100 + k * 120), (290, y0 + 60), (330, y0 + 60)], f'TX / RX / DE {n}', 'io', 256, 94 + k * 120)
        s.box(670, y0, 220, 200, f'J3{n}  5-pin XLR', ['female: out (DMX to fixtures)', '  pin 1 GND (isolated)', '  pin 2 data − (B)', '  pin 3 data + (A)', '  pins 4, 5 unused', 'male version for DMX in', 'status LED: green = live,', '  yellow = hold, red = loss'], 'pix')
        s.wire([(590, y0 + 100), (670, y0 + 100)], 'A / B / GND', 'pix', 598, y0 + 94)
        s.box(950, y0, 220, 200, f'Port {n} protection', ['120 Ω termination, jumper', '  (DMX in, or end of line)', 'SM712 TVS on A / B', '2 × 4.7 Ω series in A / B', 'shield to chassis via', '  1 nF / 2 kV'], 'safe')
        s.wire([(890, y0 + 100), (950, y0 + 100)], '', 'safe')
    s.text(20, 580, 'DMX out: every frame is break (120 µs), mark-after-break (16 µs), start code 0 and 512 slots — 22.7 ms on the wire, so at most 44 frames a second.', 11, color='#555')
    s.text(20, 596, 'Several senders on one universe are merged on the node: HTP = the highest value per channel, LTP = the latest change. After 2.5 s without a sender', 11, color='#555')
    s.text(20, 612, 'the port holds the last frame or goes dark (blackout). DMX in: a break starts a frame; every frame goes out as ArtDmx (broadcast) or sACN (multicast).', 11, color='#555')
    s.text(20, 636, 'Four or eight ports need more than the two free UARTs: those boards drive the lines from the RMT peripheral (next revision). RDM is not built yet.', 11, 'bold', '#222')
    s.save('edmx.svg')


if __name__ == '__main__':
    core(); winch(); pixel(); edmx()
    print('wrote', os.path.normpath(OUT))
