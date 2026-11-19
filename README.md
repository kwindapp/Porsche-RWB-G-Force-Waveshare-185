# Porsche / RWB G-force meter — minimalist peak markers

Waveshare ESP32-S3 Touch LCD 1.85, 360x360, USB-C down.

- Original Porsche carbon/bronze 360x360 background retained.
- Live white dot and amber trail.
- Persistent amber lateral and longitudinal peak dots with small g values.
- No overlapping MAX/LIVE text rows.
- Tap to clear peaks and trail (CST816-family touch controller at I2C 0x15, unverified on physical device).
- Reboot also clears peaks. Peaks are signed internally and compare absolute magnitude.

## Upload
Open this folder in VS Code/PlatformIO and run `pio run -t upload`.

## Notes
The touch implementation assumes CST816-family register layout at 0x15. If tapping does not work, check the Waveshare board revision and touch controller. Sensor axis signs are configurable near the top of src/main.cpp. The project has not been compiled or hardware tested here.
