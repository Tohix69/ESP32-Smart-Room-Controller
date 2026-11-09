# Contributing

1. Open an issue before large changes so the approach can be discussed.
2. Keep changes focused and preserve offline physical control if Wi-Fi is unavailable.
3. Build with PlatformIO (`pio run`) before opening a pull request.
4. Test touch inputs, relay states, OLED, Wi-Fi recovery, schedule behavior, and OTA where hardware is available.
5. Never commit `secrets.h`, firmware binaries, passwords, or private network credentials.
6. Include board details and sanitized logs when reporting bugs.
