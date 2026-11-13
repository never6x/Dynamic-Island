<div align="center">
  <!-- Замени app-icon.png на путь к твоей иконке (или удали строку, если иконки нет) -->
<img width="573" height="200" alt="изображение" src="https://github.com/user-attachments/assets/76f45f75-e84c-46be-a3fb-cfae14e14386" />

  <h1>Aqua Wave</h1>
  <p><b>Dynamic Island for Windows</b></p>

  <!-- Бейджи -->
  <a href="#"><img src="https://img.shields.io/badge/Windows-10%20%7C%2011-blue?style=flat-square" alt="Windows"></a>
  <a href="#"><img src="https://img.shields.io/badge/release-v1.0.0-green?style=flat-square" alt="Release"></a>
  <a href="#"><img src="https://img.shields.io/badge/interface-English%20%7C%20Russian-purple?style=flat-square" alt="Interface"></a>
</div>

<br/>

<!-- Английский язык (открыт по умолчанию) -->
<details open>
<summary><b>English</b> · click to read in English</summary>

<br/>

<p align="center">
  A sleek, floating media control overlay for Windows inspired by the iOS Dynamic Island.<br/>
  It displays current media playback information, provides smooth playback controls, and includes a built-in screen recorder with system audio support.
</p>

<!-- Замени preview.gif на путь к твоей гифке или скриншоту -->
<p align="center">
  <img src="preview.gif" width="800" alt="Aqua Wave Preview" />
</p>

### Features

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

### Hotkeys

- `Alt + Q`: Start or stop screen recording with system audio.
- `Ctrl + Shift + O`: Show or hide the overlay.
- `Ctrl + Shift + T`: Toggle click-through mode (makes the overlay ignore mouse clicks).

### Screen Recording

When recording is active, the app captures the entire screen along with the system audio. 
Recordings are saved in MP4 format at 60 FPS in the following directory:

`C:\Users\<YourUsername>\Videos\Aqua Wave\`

A red indicator dot will appear on the right side of the compact pill to indicate that recording is in progress.

### Requirements

- Windows 10 or Windows 11 (x64).
- Visual Studio 2022 with the "Desktop development with C++" workload.
- Windows SDK.

### Build Instructions

1. Clone the repository.
2. Open the solution or `main.cpp` in Visual Studio.
3. Set the build configuration to `Release` and `x64`.
4. Compile the project.
5. Ensure the required font files are placed in the same directory as the executable:
   - `SF-Pro-Display-Semibold.otf`
   - `SF-Pro-Display-Regular.otf`
   - (Alternatively, `Inter_18pt-SemiBold.ttf` and `Inter_18pt-Regular.ttf` or `Segoe UI` will be used as fallbacks).

### Configuration

The application saves your settings (position, color, opacity, and lock state) in an `overlay_config.ini` file located in the same directory as the executable. 

Right-click the tray icon or right-click the overlay to open the settings menu.

### Technologies Used

- Win32 API & GDI+ (Custom UI rendering, Squircle geometry, GDI+ text rendering).
- Windows.Media.Control (WinRT API for media session management).
- Media Foundation (MP4 container, H.264 video encoding, AAC audio encoding).
- WASAPI Loopback (System audio capture).
- Windows Imaging Component (WIC) for image decoding and high-quality album art scaling.

</details>

<br/>

<!-- Русский язык (закрыт по умолчанию) -->
<details>
<summary><b>Русский</b> · нажмите, чтобы прочитать на русском</summary>

<br/>

<p align="center">
  Стильный плавающий оверлей для управления медиа в Windows, вдохновленный Dynamic Island из iOS.<br/>
  Он отображает информацию о текущем воспроизведении, предоставляет плавные элементы управления и включает встроенную запись экрана с системным звуком.
</p>

<!-- Замени preview.gif на путь к твоей гифке или скриншоту -->
<p align="center">
  <img src="preview.gif" width="800" alt="Aqua Wave Preview" />
</p>

### Особенности

- Интерфейс в стиле Dynamic Island с плавными анимациями раскрытия и сворачивания.
- Отслеживание медиа в реальном времени (Название, Исполнитель, Обложка, Прогресс).
- Элементы управления: Play, Пауза, Следующий, Предыдущий и Перемотка.
- Динамический аудиовизуализатор, который извлекает доминирующий цвет из обложки альбома.
- Встроенная запись экрана (H.264 High Profile 45 Мбит/с + AAC 192 Кбит/с).
- Захват системного звука через WASAPI Loopback.
- Настройка внешнего вида: выбор цвета HSL, ползунок прозрачности и предустановленные темы.
- Режим сквозных кликов (click-through) и блокировка позиции.
- Интеграция с системным треем для быстрого доступа к настройкам.
- Поддержка кастомных шрифтов (SF Pro Display, Inter или системный Segoe UI).

### Горячие клавиши

- `Alt + Q`: Начать или остановить запись экрана с системным звуком.
- `Ctrl + Shift + O`: Показать или скрыть оверлей.
- `Ctrl + Shift + T`: Включить режим сквозных кликов (оверлей перестает реагировать на мышь).

### Запись экрана

Во время записи приложение захватывает весь экран вместе с системным звуком. 
Видео сохраняются в формате MP4 с частотой 60 FPS в следующую директорию:

`C:\Users\<ВашеИмя>\Videos\Aqua Wave\`

Красный индикатор появится в правой части свернутой капсулы, сигнализируя о том, что идет запись.

### Требования

- Windows 10 или Windows 11 (x64).
- Visual Studio 2022 с рабочей нагрузкой "Разработка классических приложений на C++".
- Windows SDK.

### Инструкция по сборке

1. Склонируйте репозиторий.
2. Откройте решение или `main.cpp` в Visual Studio.
3. Установите конфигурацию сборки `Release` и платформу `x64`.
4. Скомпилируйте проект.
5. Убедитесь, что необходимые файлы шрифтов находятся в одной папке с исполняемым файлом:
   - `SF-Pro-Display-Semibold.otf`
   - `SF-Pro-Display-Regular.otf`
   - (В качестве запасного варианта будут использоваться `Inter_18pt-SemiBold.ttf` и `Inter_18pt-Regular.ttf` или `Segoe UI`).

### Конфигурация

Приложение сохраняет ваши настройки (позицию, цвет, прозрачность и состояние блокировки) в файле `overlay_config.ini`, расположенном рядом с исполняемым файлом. 

Нажмите правой кнопкой мыши на иконку в трее или на сам оверлей, чтобы открыть меню настроек.

### Используемые технологии

- Win32 API & GDI+ (Кастомный рендеринг UI, геометрия Squircle, отрисовка текста GDI+).
- Windows.Media.Control (WinRT API для управления медиа-сессиями).
- Media Foundation (Контейнер MP4, кодирование видео H.264, кодирование аудио AAC).
- WASAPI Loopback (Захват системного звука).
- Windows Imaging Component (WIC) для декодирования изображений и масштабирования обложек в высоком качестве.

</details>
