# ESP32-C6 Blinky

Ein minimales Arduino-Blinky-Projekt für den ESP32-C6.

## Lokal kompilieren

```sh
arduino-cli config add board_manager.additional_urls \
	https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.2.0
arduino-cli compile --fqbn esp32:esp32:esp32c6 blinky
```

Die CI führt denselben Build bei jedem Push und bei jedem Pull Request aus.