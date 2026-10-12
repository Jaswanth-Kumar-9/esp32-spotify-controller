# ESP32 Spotify OLED Player

A public-safe ESP32 Spotify status display using an ESP32, a 128x64 SH1106 SPI OLED, a push button, and a Python bridge running on a laptop.

## Public-repository safety

This package intentionally contains placeholders only. Do not put real Wi-Fi passwords, Spotify client secrets, OAuth tokens, bridge API keys, or private IP details into a public GitHub repository.

The file .env.example documents the Python bridge settings and the matching ESP32 settings. Copy it to .env locally and fill in your private values. The Python bridge reads its Spotify values from .env.

The Arduino sketch cannot read a dotenv file directly. Before flashing a device, replace the four clearly labeled placeholders in the local copy of Spotify_V6.ino. Do not commit that edited copy. This keeps the public ZIP limited to four files while preserving the wiring/configuration reference.

## Project files

- Spotify_V6.ino: ESP32 firmware for the SH1106 128x64 SPI OLED.
- app.py: Flask bridge that handles Spotify OAuth and exposes playback endpoints.
- .env.example: placeholder configuration template for the bridge and ESP32.
- README.md: project overview, setup, wiring, and public-repository guidance.

## Python bridge setup

1. Install Python 3.10 or newer.
2. Create and activate a virtual environment.
3. Install dependencies with: python -m pip install Flask spotipy python-dotenv
4. Copy .env.example to .env and fill in your Spotify Client ID, Client Secret, redirect URI, and a local bridge API key.
5. In the Spotify Developer Dashboard, register exactly: http://127.0.0.1:8888/callback
6. Start the bridge with: python app.py
7. Open http://127.0.0.1:8888/login and authorize Spotify.

The bridge stores its OAuth token cache locally. Never upload .env or the token cache.

## Arduino setup

1. Install ESP32 board support in Arduino IDE.
2. Install the ArduinoJson and U8g2 libraries.
3. In a private working copy of Spotify_V6.ino, replace YOUR_WIFI_SSID, YOUR_WIFI_PASSWORD, YOUR_LAPTOP_LAN_IP, and YOUR_BRIDGE_API_KEY.
4. Use the same bridge API key as BRIDGE_API_KEY in your local .env.
5. Upload the sketch to the ESP32.

### OLED wiring

| OLED pin | ESP32 pin |
|---|---|
| SCL / SCK | GPIO 18 |
| SDA / MOSI | GPIO 23 |
| RES | GPIO 25 |
| DC | GPIO 26 |
| VCC | 3.3V |
| GND | GND |

Connect the widget button between GPIO 27 and GND. The sketch uses INPUT_PULLUP.

## Features

- Displays current Spotify track, artist, progress, playback state, and device.
- Scrolls long titles and artist names.
- GPIO 27 cycles between Spotify player, clock, and weather pages.
- Weather is read from the public Open-Meteo endpoint.
- Uses the supplied local bridge endpoints and SH1106 SPI layout.

## Publishing checklist

Before publishing this folder, confirm the ZIP contains only the four intended files and that .env, token caches, and any edited private sketch are absent. If any real credential was previously exposed, rotate or revoke it before using the project.

