# CSI Motion and Position Demo

Arduino Wi-Fi CSI motion and room-zone sensing demo for the Olimex ESP32-S3-DevKit-LiPo and other ESP32-S3 boards.

This project lets you test two related ideas:

- Motion detection: show when something changes in the radio path.
- Position / room-zone detection: collect labels such as `empty`, `center`, `door`, or `desk`, then show a live browser-side guess.

Start with the one-board example. After that, use the multi-board receiver and transmitter examples for the real experiment.

## What You Need

- Arduino IDE 2.x, or Arduino IDE 1.8.x.
- ESP32 Arduino core installed in Arduino IDE.
- One Olimex ESP32-S3-DevKit-LiPo for the first test.
- For the recommended room test: three boards total.
  - 1 board runs `MultiBoardReceiverNode`.
  - 2 boards run `MultiBoardTransmitterNode`.
- USB cable for programming.
- Optional LiPo batteries for testing transmitters away from the computer.

No extra Arduino libraries are required. The examples use libraries that come with the ESP32 Arduino core: `WiFi`, `WebServer`, `WiFiUdp`, and `Preferences`.

## Install Arduino IDE

1. Download Arduino IDE from:
   - https://www.arduino.cc/en/software
2. Install it.
3. Start Arduino IDE once.

## Install ESP32 Board Support

Arduino IDE does not support ESP32 boards until you install the ESP32 board package.

1. Open Arduino IDE.
2. Go to `File > Preferences`.
3. Find `Additional boards manager URLs`.
4. Add this URL:

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

5. Press `OK`.
6. Go to `Tools > Board > Boards Manager...`.
7. Search for `esp32`.
8. Install `esp32 by Espressif Systems`.
9. Restart Arduino IDE.

This is the stable ESP32 package URL from Espressif's Arduino-ESP32 install documentation:

- https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html

## Get This Project

When this project is on GitHub:

1. Open the GitHub repository page.
2. Press `Code`.
3. Press `Download ZIP`.
4. Extract the ZIP somewhere easy to find.

The project folder should contain files like this:

```text
CSI-Motion-and-Position-Demo/
  library.properties
  README.md
  src/
    MotionEsp32S3.h
    MotionEsp32S3.cpp
  examples/
    SingleBoardDashboard/
      SingleBoardDashboard.ino
    MultiBoardReceiverNode/
      MultiBoardReceiverNode.ino
    MultiBoardTransmitterNode/
      MultiBoardTransmitterNode.ino
    LoggerOnly/
      LoggerOnly.ino
```

The exact top folder name can be different. What matters is that `library.properties`, `src`, and `examples` are inside the same project folder.

## Install This Project In Arduino IDE

The easiest method is Arduino's ZIP library installer.

1. Make sure the whole project folder contains `library.properties`, `src`, and `examples`.
2. Zip the whole project folder.
3. In Arduino IDE, go to `Sketch > Include Library > Add .ZIP Library...`.
4. Select the ZIP file.
5. Wait for Arduino IDE to import it.
6. Check `File > Examples > CSI Motion and Position Demo`.

You should see:

- `SingleBoardDashboard`
- `MultiBoardReceiverNode`
- `MultiBoardTransmitterNode`
- `LoggerOnly`

If Arduino says the ZIP is invalid, the ZIP is probably nested incorrectly. Open the ZIP and check that it contains one project folder, and inside that folder are `library.properties`, `src`, and `examples`.

## Manual Install Option

Use this only if the ZIP installer does not work.

1. In Arduino IDE, open `File > Preferences`.
2. Note the `Sketchbook location`.
3. Open that folder in Windows Explorer.
4. Open or create the `libraries` folder.
5. Copy the whole project folder into `libraries`.

On Windows the final layout should look similar to:

```text
C:\Users\YOUR_NAME\Documents\Arduino\libraries\CSI_Motion_and_Position_Demo\
  library.properties
  src\
  examples\
```

Restart Arduino IDE after copying manually.

## Olimex ESP32-S3-DevKit-LiPo Board Settings

Connect the board through the onboard `USB-OTG1` native USB Type-C connector.

In Arduino IDE, select the board:

- `Tools > Board > esp32 > OLIMEX ESP32-S3-DevKit-Lipo`

If that exact board name is not available, update the ESP32 board package first. For quick experiments, a compatible ESP32-S3 board profile can work, but the Olimex profile is recommended for this board.

Settings tested with the USB-OTG1 connector:

| Arduino IDE menu | Value |
| --- | --- |
| Upload Speed | `921600` |
| USB Mode | `Hardware CDC and JTAG` |
| USB CDC On Boot | `Enabled` |
| USB Firmware MSC On Boot | `Disabled` |
| USB DFU On Boot | `Disabled` |
| Upload Mode | `USB-OTG CDC (TinyUSB)` |
| CPU Frequency | `240MHz (WiFi)` |
| Flash Mode | `QIO 80MHz` |
| Flash Size | `8MB (64Mb)` |
| Partition Scheme | `8M with spiffs (3MB APP/1.5MB SPIFFS)` |
| Core Debug Level | `None` |
| PSRAM | `OPI PSRAM` |
| Arduino Runs On | `Core 1` |
| Events Run On | `Core 1` |
| Erase All Flash Before Sketch Upload | `Disabled` |
| JTAG Adapter | `Disabled` |
| Port | the detected `ESP32 Family Device` COM port |

If you use an external USB-UART adapter instead of the native USB connector, the port and upload mode can be different.

## First Test: One Board

Use this first. It proves that the library, board package, upload, serial port, Wi-Fi, and dashboard all work.

1. Open Arduino IDE.
2. Go to `File > Examples > CSI Motion and Position Demo > SingleBoardDashboard`.
3. Edit these lines near the top:

```cpp
const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
```

4. Select your board and port in `Tools`.
5. Press `Upload`.
6. Open `Tools > Serial Monitor`.
7. Set baud rate to `115200`.

After Wi-Fi connects, Serial Monitor prints something like:

```text
Dashboard: http://192.168.0.139/
```

Open that address from a phone or computer on the same Wi-Fi network.

This one-board example joins your normal Wi-Fi network. It does not create its own access point.

Expected serial output looks like:

```text
score:0.55 threshold:1.10 motion:0 confidence:50 packets:656 links:1 best:F0:F5:BD:02:28:A0 rssi:-51
```

If you see changing packet counts and a dashboard in the browser, the basic setup works.

## One-Board Placement

For `SingleBoardDashboard`:

- Put the router/AP and the ESP32-S3 on opposite sides of the test area.
- Keep the board and router fixed.
- Keep the room still for the first 8 seconds after boot.
- Walk between the router and the board and watch the score.

This mode is only a quick proof. It depends on router traffic and the local Wi-Fi environment. The multi-board setup is more repeatable.

## Recommended Test: Three Boards

For a practical motion and position test, use three boards:

| Board | Example to flash | Role |
| --- | --- | --- |
| Board 1 | `MultiBoardReceiverNode` | Creates Wi-Fi AP and dashboard |
| Board 2 | `MultiBoardTransmitterNode` | Sends packets to receiver |
| Board 3 | `MultiBoardTransmitterNode` | Sends packets to receiver |

The receiver creates its own Wi-Fi network:

```text
SSID: MOTION-RX
Password: motion1234
Dashboard: http://192.168.4.1
```

Your room router is not needed for the multi-board demo.

## Flash The Receiver

1. Open `File > Examples > CSI Motion and Position Demo > MultiBoardReceiverNode`.
2. Select the Olimex ESP32-S3 board and port.
3. Upload.
4. Open Serial Monitor at `115200`.
5. Confirm it prints:

```text
Receiver access point started.
SSID: MOTION-RX
Password: motion1234
Dashboard: http://192.168.4.1/
```

## Flash The Transmitters

For the first transmitter:

1. Open `File > Examples > CSI Motion and Position Demo > MultiBoardTransmitterNode`.
2. Set:

```cpp
const char *NODE_ID = "node-1";
```

3. Upload to board 2.

For the second transmitter:

1. Keep the same sketch open.
2. Change:

```cpp
const char *NODE_ID = "node-2";
```

3. Upload to board 3.

The transmitters should keep these defaults unless you changed the receiver:

```cpp
const char *RX_SSID = "MOTION-RX";
const char *RX_PASS = "motion1234";
```

Serial Monitor on each transmitter prints its MAC address:

```text
Node MAC: B0:4E:26:6E:B2:73
```

Write down each transmitter MAC if you later want to use a custom allowlist.

## Open The Multi-Board Dashboard

1. Power the receiver.
2. Power the transmitter boards.
3. On your phone or laptop, connect to Wi-Fi network `MOTION-RX`.
4. Use password `motion1234`.
5. Open:

```text
http://192.168.4.1
```

The page title is `CSI Motion and Position Demo`.

The page has two tabs:

- `Motion`: live motion score and calibration.
- `Position`: label collection and live room-zone guess.

## Multi-Board Placement

Start simple:

- Put the receiver near one wall or corner.
- Put transmitter 1 on another side of the room.
- Put transmitter 2 on a different side of the room.
- The three boards should roughly form a triangle around the area.
- Keep antennas vertical.
- Keep all boards fixed during a test.
- Do not let boards swing from USB cables.

A person should cross at least one transmitter-to-receiver radio path when moving through the room.

For bigger rooms, 3 to 5 transmitters can be useful. The receiver tracks up to 8 active links, but the phone/laptop also uses one SoftAP client slot. If the dashboard `Dropped` counter rises quickly, reduce `PACKETS_PER_SECOND` in every transmitter to `25` to `35`.

## Motion Calibration

Use this after placing the receiver and transmitters:

1. Open `http://192.168.4.1`.
2. Go to the `Motion` tab.
3. Wait until active links appear.
4. Press `Reset baseline`.
5. Keep the room still.
6. Press `Learn idle`.
7. Stay still during the 8 second idle sample.
8. Press `Learn motion`.
9. Move through the path you want to detect during the 20 second motion sample.
10. Press `Apply suggested`.
11. Test normal movement and no movement.
12. Press `Save settings` if the threshold works well.

Lower threshold means more sensitive but more false positives. Higher threshold means fewer false positives but it can miss small motion.

## Position / Room-Zone Learning

Position mode is for labels such as:

- `empty`
- `center`
- `door`
- `desk`
- `bed`
- `sofa`

The model runs in the browser. It learns from the labels you collect during the current browser session.

Basic test with `empty` and `center`:

1. Open `http://192.168.4.1`.
2. Go to the `Position` tab.
3. Select label `empty`.
4. Leave the room empty and press `Start 60s`.
5. Wait until recording stops, or press `Stop`.
6. Select label `center`.
7. Stand in the center of the room and press `Start 60s`.
8. Wait until recording stops, or press `Stop`.
9. Watch `Live guess`.
10. Leave the room empty and check that it tends toward `empty`.
11. Stand in the center and check that it tends toward `center`.

The live guess is a simple nearest-centroid classifier. It compares the current CSI rows against the labels stored in the browser. It is meant for quick experiments, not precision indoor positioning.

Important:

- Keep all boards fixed while collecting all labels.
- If you move a board, collect the labels again.
- If you refresh or close the browser tab, the temporary model is lost.
- Press `Download CSV` before leaving the page if you want to keep the dataset.

## LoggerOnly Example

`LoggerOnly` is a smaller receiver sketch for data collection only.

Use it when:

- You want the simplest phone-friendly CSV logger.
- You do not need the full motion dashboard.
- You want optional CSV over USB Serial by setting:

```cpp
#define PRINT_SERIAL_CSV 1
```

For most users, start with `MultiBoardReceiverNode` instead.

## Filtering Links

The dashboard can show several links because it sees CSI from different Wi-Fi transmitters.

For `SingleBoardDashboard`, the default is to filter to the connected router/AP.

For `MultiBoardReceiverNode`, the default is:

```cpp
#define CSI_FILTER_MODE CSI_FILTER_NONE
```

This lets all transmitter nodes appear.

If phones or other clients appear as unwanted links, edit the receiver sketch:

```cpp
#define CSI_FILTER_MODE CSI_FILTER_CUSTOM_LIST
#define CSI_CUSTOM_MAC_COUNT 2
const uint8_t CSI_CUSTOM_MAC_ALLOWLIST[MotionEsp32S3::kMaxLinks][6] = {
  {0xB0, 0x4E, 0x26, 0x6E, 0xB2, 0x73},
  {0xF0, 0xF5, 0xBD, 0x02, 0x28, 0xA0},
};
```

Use the MAC addresses printed by each transmitter as `Node MAC: ...`.

## What The Dashboard Shows

- Motion score
- Motion/calm state
- Threshold
- Confidence
- Best link MAC
- Active links
- Per-link RSSI, score, packets, and state
- Dropped queue count
- Free heap and minimum free heap
- Motion calibration controls
- Position labels and live guess
- CSV download

Hover the mouse over dashboard buttons to see short tooltips.

## Troubleshooting

### The examples do not appear in Arduino IDE

- Restart Arduino IDE.
- Check that the ZIP was imported with `Sketch > Include Library > Add .ZIP Library...`.
- Check that `library.properties`, `src`, and `examples` are inside the installed library folder.

### ESP32 boards do not appear in the Board menu

- Recheck the Boards Manager URL.
- Install `esp32 by Espressif Systems` from Boards Manager.
- Restart Arduino IDE.

### Upload fails

- Check that the correct COM port is selected.
- Check the USB cable supports data, not only charging.
- For the Olimex USB-OTG1 connector, use the settings in this README.
- Try pressing the board reset button and uploading again.

### Serial Monitor shows nothing

- Set baud rate to `115200`.
- Press reset on the board.
- Make sure the sketch actually uploaded to this board.

### Single-board dashboard does not open

- Make sure the board joined your Wi-Fi.
- Your phone/computer must be on the same Wi-Fi network.
- Use the IP printed in Serial Monitor.

### Multi-board dashboard does not open

- Connect your phone/laptop to Wi-Fi `MOTION-RX`.
- Open `http://192.168.4.1`.
- Your normal room router is not used in this mode.

### No active links

- Make sure transmitter boards are powered.
- Make sure transmitters use `RX_SSID = "MOTION-RX"`.
- Keep the receiver powered before powering transmitters.

### Dropped count rises quickly

- Lower `PACKETS_PER_SECOND` in every `MultiBoardTransmitterNode`.
- Try `25` to `35` when using 4 or 5 transmitters.
- Keep the browser polling to one open dashboard tab.

### Position guess is wrong

- Collect longer samples, such as 120 seconds per label.
- Keep boards fixed.
- Make labels very different at first: `empty` and `center`.
- Add more transmitter paths if labels are not separable.
- Recollect all labels after moving any board.

## Library API For Custom Sketches

Minimal use:

```cpp
#include <MotionEsp32S3.h>

MotionEsp32S3 motion;

void setup() {
  // Start Wi-Fi first, then:
  MotionEsp32S3::Config config;
  config.threshold = 1.1f;
  motion.begin(config);
}

void loop() {
  if (motion.update()) {
    auto r = motion.reading();
    Serial.println(r.score);
  }
}
```

Start Wi-Fi before `motion.begin()`. The examples show both station mode and SoftAP mode.

For CSV logging:

- `fingerprintCsv()` returns the current highest-scoring active link.
- `fingerprintCsvAll()` returns one row per active link and is used by `MultiBoardReceiverNode` and `LoggerOnly`.

## Stability Notes

The ESP-IDF Wi-Fi CSI callbacks run from the Wi-Fi task. This library keeps the callback short by copying raw CSI bytes into a queue. Binning and detector math run later from `loop()`.

The dashboard examples pause CSI briefly while `Save settings` writes threshold/sensitivity to ESP32 flash. This avoids doing flash writes while CSI callbacks are active.

The dashboard shows heap as `currentK / minimumK`. If reboots continue, note the heap numbers before the reboot and save the full panic log.

## GitHub Publishing Checklist

Before publishing the repository, check that the root contains:

```text
README.md
library.properties
src/
examples/
```

Also consider adding:

- `LICENSE`
- screenshots of the dashboard
- photos or a diagram showing receiver/transmitter placement

Do not rename the `.ino` files without also renaming their containing folders. Arduino sketches require the folder name and `.ino` filename to match.
