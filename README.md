# AC400Controller
Replacement controller board for Record Power AC400 air filtering system.

Original board has very simplictic power supply that tends to overheat and burn that makes the use of the device hindered or not possible.

This is Arduino based drop-in replacement board that doesn't have this problem and also allows customizations.

![Assembled PCB](https://github.com/thikone/AC400Controller/blob/4cbf95d95334eb8950539fc47dbb31bb6bd63e37/Pictures/Assembled%20PCB%202.jpg "Assembled PCB")

Ready for installation to Record Power AC400 air filtration unit:

![Assembled PCB in the case](https://github.com/thikone/AC400Controller/blob/c241d6417647482133be38830b8f9bde76ba4821/Pictures/Assembled%20PCB%20inside%20case.jpg "Assembled PCB in 3D printed case")

The PCB and Schematic `.json` files can be opened and viewed with EasyEDA [online](https://easyeda.com/)

## Changes in this fork

This fork runs the controller on an [Arduino Nano ESP32](https://store.arduino.cc/products/nano-esp32) and adds Wi-Fi control.

### Arduino Nano ESP32 port

- Uses the same header pins as the original Nano, so the PCB is unchanged.
- Uses [IRremote](https://github.com/Arduino-IRremote/Arduino-IRremote) 4.x instead of IRLremote, which doesn't support the ESP32.
- The sketch is now `AC400Controller.ino` in the repo root, so the repo folder must be named `AC400Controller` for the Arduino IDE to open it.

### Wi-Fi web API

1. Copy `secrets.example.h` to `secrets.h` and enter your Wi-Fi network name and password. `secrets.h` is ignored by git. The ESP32 only supports 2.4GHz networks.
2. Flash the sketch. The controller joins Wi-Fi and is available at `http://ac400.local`.

| Request | Effect |
| --- | --- |
| `GET /status` | Returns the current state as JSON |
| `POST /speed?value=0..3` | 0 is off, then low, medium, high |
| `POST /timer?value=0..3` | 0 cancels, then 1, 2 or 4 hours |

Both `POST` requests return the new state, for example:

```json
{"speed":1,"timer":0,"timer_remaining_s":0,"lease_remaining_s":900,"source":"web"}
```

`source` shows whether the last change came from the web or from the buttons and remote.

A fan started from the web stops after 15 minutes unless the same speed is sent again, which renews it without beeping. This stops the fan running forever if whatever turned it on goes offline. Setting a timer, or using the buttons or remote, cancels this.

The buttons and remote work as before, with or without Wi-Fi.

Example from a Mac (`-4` avoids a slow IPv6 lookup of `ac400.local`):

```sh
curl -4 http://ac400.local/status
curl -4 -X POST 'http://ac400.local/speed?value=2'
```
