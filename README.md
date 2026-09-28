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
	--fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_fs \
	blinky
```

## CI und Releases

Die GitHub-Actions-Pipeline kompiliert den Sketch bei jedem Push und Pull Request
für den ESP32-C6 mit dem Partitionslayout `no_fs`.
USB-CDC ist mit `CDCOnBoot=cdc` aktiviert; die serielle Schnittstelle ist nach
dem Boot über den USB-Port verfügbar.

Bei jedem Push wird zusätzlich ein GitHub-Release mit dem Namen `Build <Nummer>`
erstellt. Die kompilierten Dateien werden als Release-Assets und als CI-Artefakt
veröffentlicht. Pull Requests erzeugen kein Release.

## HTTPS-OTA

Beim Start verbindet sich der ESP32 mit dem zuletzt gespeicherten WLAN und fragt
die GitHub-API nach dem neuesten Release dieses Repositories ab. Ist dessen
`tag_name` nicht der in NVS gespeicherte installierte Tag, lädt er das Asset
`blinky.ino.bin` über HTTPS herunter. Der Tag wird erst nach erfolgreichem Update
in NVS gespeichert. TLS-Zertifikate werden gegen das CA-Bundle des ESP32-Arduino-
Cores geprüft.

Beim ersten Start fragt der serielle Monitor (115200 Baud) nach SSID und Passwort.
Die Eingabe wird in NVS gespeichert und nicht in die Firmware kompiliert. Zum
Ändern der Zugangsdaten im seriellen Monitor `wifi` senden und die neuen Werte
eingeben. Die Zugangsdaten liegen in NVS; ohne aktivierte Flash-Verschlüsselung
sind sie dort nicht verschlüsselt.

Der OTA-Pfad erwartet öffentliche GitHub-Releases mit dem Asset
`blinky.ino.bin`. Pull-Request-Builds erzeugen keine Releases und sind nicht als
OTA-Quelle vorgesehen.

## BLE-UART

Der ESP32-C6 stellt zusätzlich den Nordic UART Service (NUS) bereit. Mit einer
BLE-UART-App Befehle zeilenweise an die RX-Characteristic senden; Antworten
kommen als Notifications über TX. Die Service-UUID lautet
`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`, RX `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
und TX `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`.

Bei bestehender BLE-Verbindung wird alle fünf Minuten eine Meldung mit der
internen Chiptemperatur in Grad Celsius gesendet. Das ist die Temperatur des
ESP32-Chips, nicht die Umgebungstemperatur.