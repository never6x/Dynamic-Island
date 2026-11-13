<div align="center">
  <!-- Замени app-icon.png на путь к твоей иконке (или удали строку, если иконки нет) -->
  <img width="417" height="174" alt="изображение" src="https://github.com/user-attachments/assets/895f26ad-cafb-4be3-af25-88edb03408ec" />

  <h1>Aqua Wave</h1>
  <p><b>Dynamic Island for Windows</b></p>

  <!-- Бейджи (можно изменить версию и цвета) -->
  <a href="#"><img src="https://img.shields.io/badge/Windows-10%20%7C%2011-blue?style=flat-square" alt="Windows"></a>
  <a href="#"><img src="https://img.shields.io/badge/release-v1.0.0-green?style=flat-square" alt="Release"></a>
  <a href="#"><img src="https://img.shields.io/badge/interface-English%20%7C%20Russian-purple?style=flat-square" alt="Interface"></a>
</div>

<br/>

<details open>
<summary><b>English</b> - click to read in English</summary>

<br/>

<p align="center">
  A sleek, floating media control overlay for Windows inspired by the iOS Dynamic Island.<br/>
  It displays current media playback information, provides smooth playback controls, and includes a built-in screen recorder with system audio support.
</p>

<!-- Замени preview.gif на путь к твоей гифке или скриншоту -->
<p align="center">
  <img src="preview.gif" width="800" alt="Aqua Wave Preview" />
</p>

</details>

---

## Features

- Dynamic Island-style UI with smooth expand and collapse animations.
- Real-time media tracking (Title, Artist, Album Art, Progress).
- Playback controls: Play, Pause, Next, Previous, and Seek.
- Dynamic audio visualizer that extracts and adapts to the dominant color of the album art.
- Built-in screen recorder (H.264 High Profile 45 Mbps + AAC 192 kbps).
- System audio capture via WASAPI Loopback.
- Customizable appearance: HSL color picker, opacity slider, and preset themes.
- Click-through mode and position locking.
- System tray integration for quick settings access.
- Custom font support (SF Pro Display, Inter, or Segoe UI fallback).

## Hotkeys

- `Alt + Q`: Start or stop screen recording with system audio.
- `Ctrl + Shift + O`: Show or hide the overlay.
- `Ctrl + Shift + T`: Toggle click-through mode (makes the overlay ignore mouse clicks).

## Screen Recording

When recording is active, the app captures the entire screen along with the system audio. 
Recordings are saved in MP4 format at 60 FPS in the following directory:

`C:\Users\<YourUsername>\Videos\Aqua Wave\`

A red indicator dot will appear on the right side of the compact pill to indicate that recording is in progress.

## Requirements

- Windows 10 or Windows 11 (x64).
- Visual Studio 2022 with the "Desktop development with C++" workload.
- Windows SDK.

## Build Instructions

1. Clone the repository.
2. Open the solution or `main.cpp` in Visual Studio.
3. Set the build configuration to `Release` and `x64`.
4. Compile the project.
5. Ensure the required font files are placed in the same directory as the executable:
   - `SF-Pro-Display-Semibold.otf`
   - `SF-Pro-Display-Regular.otf`
   - (Alternatively, `Inter_18pt-SemiBold.ttf` and `Inter_18pt-Regular.ttf` or `Segoe UI` will be used as fallbacks).

## Configuration

The application saves your settings (position, color, opacity, and lock state) in an `overlay_config.ini` file located in the same directory as the executable. 

Right-click the tray icon or right-click the overlay to open the settings menu.

## Technologies Used

- Win32 API & GDI+ (Custom UI rendering, Squircle geometry, GDI+ text rendering).
- Windows.Media.Control (WinRT API for media session management).
- Media Foundation (MP4 container, H.264 video encoding, AAC audio encoding).
- WASAPI Loopback (System audio capture).
- Windows Imaging Component (WIC) for image decoding and high-quality album art scaling.
