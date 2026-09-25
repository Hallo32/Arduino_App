# ESP32-C6 Blinky

Ein minimales Arduino-Blinky-Projekt für den ESP32-C6.

## Lokal kompilieren

```sh
arduino-cli config add board_manager.additional_urls \
	https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12
arduino-cli compile \
	--export-binaries \
	--output-dir build/esp32c6 \
	--fqbn esp32:esp32:esp32c6:PartitionScheme=no_fs \
	blinky
```

## CI und Releases

Die GitHub-Actions-Pipeline kompiliert den Sketch bei jedem Push und Pull Request
für den ESP32-C6 mit dem Partitionslayout `no_fs`.

Bei jedem Push wird zusätzlich ein GitHub-Release mit dem Namen `Build <Nummer>`
erstellt. Die kompilierten Dateien werden als Release-Assets und als CI-Artefakt
veröffentlicht. Pull Requests erzeugen kein Release.