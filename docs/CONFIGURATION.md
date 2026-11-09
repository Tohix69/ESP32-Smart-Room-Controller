# Configuration quick reference

- `include/project_config.h`: POSIX time-zone string and OTA Basic Auth username/password.
- `secrets.h`: local Wi-Fi credentials, fallback AP credentials, static IP settings. Create this by copying `secrets.h.example`; it is intentionally ignored by Git.
- `src/main.cpp`: GPIO assignments, relay polarity, OLED dimensions, schedule capacity, and feature implementation.

Do not publish real credentials. If you accidentally commit a password, changing it in the file is not enough: rotate the password because Git history may retain the old value.
