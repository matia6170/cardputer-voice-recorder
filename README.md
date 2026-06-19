# Voice Recorder for M5 Cardputer

A PlatformIO firmware project for the M5Stack Cardputer that turns the device into a portable voice recorder and audio player.

- **Voice recording** — captures 16 kHz mono 16-bit PCM audio from the built-in microphone and saves it as standard WAV files to the SD card
- **File browser** — navigate, select, and delete recordings stored in the `/recordings` directory on the SD card
- **WAV playback** — play back any WAV file with adjustable volume using the left/right arrow keys during playback
- **Menu-driven UI** — a canvas-based display with a simple two-item main menu (File Browser / Voice Recorder), keyboard navigation, and status messages
- **SD card support** — mounts and reads from an SD card over SPI; recordings are auto-named and flushed periodically to prevent data loss on unexpected shutdown
