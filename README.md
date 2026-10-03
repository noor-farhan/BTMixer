# BTMixer

<img width="300" height="200" alt="1" src="https://github.com/user-attachments/assets/c5f47489-fcd8-4342-935c-ae77362d6c7a" />
<img width="300" height="200" alt="2" src="https://github.com/user-attachments/assets/16a663df-c28d-4076-a888-d0c6da7812d4" />
<img width="300" height="200" alt="3" src="https://github.com/user-attachments/assets/30cfb564-e106-419f-9d73-1ce1f5a5384e" />
<img width="300" height="200" alt="4" src="https://github.com/user-attachments/assets/f048c903-5158-484f-9634-a155ec565d04" />
<img width="300" height="200" alt="5" src="https://github.com/user-attachments/assets/0c996da3-f072-4a72-bcf5-1823827c1348" />
<img width="300" height="200" alt="6" src="https://github.com/user-attachments/assets/a416219b-492b-44ea-8285-8fda738211e3" />



A wireless, MaxMix-style per-app volume mixer for Windows. An ESP32-C3 Super Mini with a
0.96" OLED and a single rotary knob talks to a small Python app over Bluetooth Low Energy,
so you can control master volume and individual app volumes (Spotify, Discord, games...)
with a physical knob. The OLED uses a cyberpunk terminal look.

Inspired by the open-source MaxMix project.
This is a from-scratch rewrite for ESP32 + BLE, not a fork.

## Hardware

- ESP32-C3 Super Mini
- 0.96" SSD1306 OLED (128x64, I2C)
- Rotary encoder with push button

| Part            | ESP32-C3 pin |
|-----------------|--------------|
| OLED SDA        | GPIO8        |
| OLED SCL        | GPIO9        |
| Encoder A       | GPIO0        |
| Encoder B       | GPIO1        |
| Encoder switch  | GPIO3       |
| Encoder common  | GND          |

OLED VCC goes to 3V3 and GND to GND. Encoder pins use internal pull-ups.

## Controls

| Action                  | Result                         |
|-------------------------|--------------------------------|
| Rotate                  | Volume of the selected app     |
| Click                   | Mute / unmute                  |
| Double-click            | Next app (entry 1 is Master)   |
| Long press              | Previous app                   |
| Hold + rotate           | Switch app                     |

## Setup

### 1. Firmware
1. Install the Arduino IDE and the ESP32 board package.
2. Install the libraries **U8g2** and **NimBLE-Arduino (2.x)** from the Library Manager.
3. Select board **ESP32C3 Dev Module** (enable *USB CDC On Boot* if you want serial logs).
4. Open `firmware/btmixer/btmixer.ino` and upload.

### 2. Windows companion app
```
cd companion
pip install -r requirements.txt
python companion.py
```
No Bluetooth pairing is needed: the app finds the device by its service UUID and
reconnects automatically. Run it with `pythonw.exe` (or a startup shortcut) to hide the console.

## Tuning

At the top of `btmixer.ino`: `STEPS_PER_DETENT` (try 2 if the knob skips), `ENC_INVERT`,
`VOL_STEP`, `DOUBLE_CLICK_MS`, `LONG_PRESS_MS`.

## Protocol

Text messages over a Nordic-UART-style BLE service
(`6e400001-b5a3-f393-e0a9-e50e24dcca9e`):

- PC to device (write): `S|index|count|name|volume|mute`
- Device to PC (notify): `V|index|volume` and `M|index` (toggle mute)

## Troubleshooting

- `Thread is configured for Windows GUI but callbacks are not working`: make sure
  `sys.coinit_flags = 0` stays above all other imports in `companion.py`, and run from a
  plain terminal rather than an IDE console.
- Device not found: check it shows "SCANNING HOST" on the OLED and that Bluetooth is on.

## License

MIT
