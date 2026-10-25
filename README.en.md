# M1 sensor MCU firmware

Open-source firmware for the ATSAMD20G18A controller in the Phicomm M1 air monitor. It handles sensors, display, clock, buttons, buzzer and controller updates. The separate [network firmware](https://github.com/phicomm-io/phicomm-community-firmware) provides Wi-Fi, MQTT and WebUI; the two processors are updated separately.

Factory **11.x** users are the recommended starting group. Factory **17.1.2.3 / 17.x** differs substantially and is not recommended until its image has been obtained and analysed. WebUI displays the raw sensor MCU version separately from the network firmware. Factory and open-source version numbers are independent; an unread version stays unknown.

Features include formaldehyde, PM1.0/PM2.5/PM10, particle counts, temperature/humidity, full-frame diagnostics, display and clock, configurable alert thresholds, CO₂ display, tones, RTTTL music, alarms and MCU OTA. v1.0.0 brings these existing features together. Existing `0x70` opcodes are retained; diagnostics and hardware overrides are documented separately.

PM2.5 uses spike suppression and adaptive smoothing, with a faster response to sustained rises. Raw sensor frames remain available for diagnostics.

HCHO and PM2.5 threshold buzzer alerts can be enabled separately and default to off. Color thresholds, clock alarms and manual tones remain independently controlled.

```sh
make test sanitize
make ota
```

The build requires a C compiler, Make, Python 3 and GNU Arm Embedded GCC/Binutils. OTA files and checksums are in `build/ota/`. The application links at `0x4000`; the independent bootloader links at `0`. Images cannot be interchanged.

See [OTA and recovery](docs/ota.md), [protocol](docs/protocol.md), [extension commands](docs/debug-protocol.md), [hardware](docs/hardware.md) and [contributing](CONTRIBUTING.md).

The application uses the factory staging region at `0x22000`, with a 65535-byte image limit. The independent bootloader uses a CRC32 journal for interrupted copies. CRC does not provide signatures or version rollback. Keeping the factory bootloader retains its recovery behaviour. SWD is required when no valid application remains.

Licensed under [GPL-3.0](LICENSE). Files in [third_party](third_party) retain their own licences.

[中文](README.md)
