# Lights Client

ESP-IDF firmware for an ESP32-C3 light client. The device connects to the
`lights` Wi-Fi network, polls `http://192.168.4.1/lights`, and mirrors the
server light state on GPIO 8.

## Requirements

- ESP-IDF installed and exported in your shell
- ESP32-C3 target hardware

## Build and Flash

```sh
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

If needed, pass the serial port explicitly:

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```

## Configuration

The Wi-Fi credentials, LED GPIO, and server URL are currently defined near the
top of `main/lights_client.c`:

```c
#define WIFI_SSID "lights"
#define WIFI_PASS "123456789"
#define LED_GPIO GPIO_NUM_8
#define SERVER_URL "http://192.168.4.1/lights"
```

The committed `sdkconfig` captures the ESP-IDF project configuration. Generated
build output lives in `build/` and is intentionally ignored by Git.
