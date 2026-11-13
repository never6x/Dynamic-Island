#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

// Размеры компактной пилюли и раскрытой карточки (по эталону)
static const float kCompactHeight = 32.0f;
static const float kExpandedWidth = 256.0f;
static const float kExpandedHeight = 128.0f;

static const float kMinCompactWidth = 98.0f;
static const float kMaxCompactWidth = 225.0f;
static const float kCompactTextHorizPad = 69.5f;

// Кружок записи справа (виден только в свернутом виде)
static const float kRecIslandSize = 32.0f;
static const float kRecIslandGap = 5.5f;

// Размер буфера окна (с запасом 12px под мягкий Glow вокруг капсулы)
static const int kCanvasW = 420;
static const int kCanvasH = 190;
static const int kCanvasPadY = 26;
static const float kMainAnchorX = 170.0f;

// Геометрия и раздельные каналы анимации (как в эталоне)
static float g_targetCompactW = 152.0f;
static float g_curCompactW = 152.0f;

static bool g_isExpanded = false;
static float g_shapeAnim = 0.0f;        // 0.0 = пилюля, 1.0 = раскрытая карточка
static float g_compContentAlpha = 1.0f; // Прозрачность мини-контента
static float g_expContentAlpha = 0.0f;  // Прозрачность контента раскрытой карточки
static float g_playAnim = 0.0f;         // 0.0 = пауза, 1.0 = играет
static float g_recAnim = 0.0f;          // 0.0 = нет записи, 1.0 = запись активна
static float g_playBtnPulse = 0.0f;
static float g_prevBtnPulse = 0.0f;
static float g_nextBtnPulse = 0.0f;
static float g_trackFadeAnim = 1.0f;

static bool g_seekingTimeline = false;

// События, таймеры и хоткеи
static const UINT WM_APP_MEDIA_CHANGED = WM_APP + 101;
static const UINT WM_APP_TRAYMSG = WM_APP + 102;
static const UINT TIMER_ANIM_ID = 1001;
static const UINT IDM_TRAY_EXIT = 3026;

static const int HOTKEY_TOGGLE_SHOW = 1;
static const int HOTKEY_TOGGLE_CLICKTHROUGH = 2;
static const int HOTKEY_TOGGLE_RECORD = 3; // Alt + Q

static IWICImagingFactory* g_pWicFactory = nullptr;

// Тёмно-синий полупрозрачный цвет фона из эталона (#20273C, Alpha = 216)
static uint8_t g_bgR = 32;
static uint8_t g_bgG = 39;
static uint8_t g_bgB = 60;
static uint8_t g_bgAlpha = 216;
static float g_pickerH = 225.0f;
static float g_pickerS = 0.47f;
static float g_pickerV = 0.24f;

struct ColorPreset {
    uint8_t r, g, b;
};

static const ColorPreset kPresets[] = {
    { 32, 39, 60 },  // Эталон (тёмно-синий полупрозрачный)
    { 18, 24, 48 },  // Глубокий синий
    { 10, 12, 18 },  // OLED Чёрный
    { 30, 33, 42 },  // Графит
    { 34, 24, 54 },  // Фиолетовый
    { 16, 38, 33 }   // Изумруд
};
static const int kNumPresets = 6;

static HWND g_hwndOverlay = nullptr;
static HWND g_hwndFlyout = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HICON g_hTrayIcon = nullptr;

struct MediaData {
    std::wstring title = L"AQUA WAVE";
    std::wstring artist = L"never6x";
    std::wstring displayText = L"AQUA WAVE";
    std::vector<uint8_t> thumbBytes;
    bool isPlaying = false;
    bool hasSession = false;
    double positionSec = 0.0;
    double durationSec = 0.0;
    uint64_t lastQueryTick = 0;
    uint64_t version = 0;
};

static std::atomic<bool> g_running{ true };
static std::mutex g_mediaMutex;
static MediaData g_sharedMedia;
static MediaData g_localMedia;
static uint64_t g_ignoreWorkerPlayUntil = 0;
static uint64_t g_ignoreWorkerSeekUntil = 0;

static double g_manualSeekBaseSec = -1.0;
static uint64_t g_manualSeekTick = 0;

// Пред-сглаженные текстуры обложек высокого разрешения
static Gdiplus::Bitmap* g_pTrackCircleHi = nullptr;  // 128x128 круг для мини-капсулы
static Gdiplus::Bitmap* g_pTrackSquareHi = nullptr;  // 160x160 скругленный квадрат для карточки
static Gdiplus::Bitmap* g_pDefCircleHi = nullptr;    // lol.jpg круг
static Gdiplus::Bitmap* g_pDefSquareHi = nullptr;    // lol.jpg скругленный квадрат

static bool g_isPositionLocked = false;
static bool g_isClickThrough = false;
static bool g_isVisible = true;

// Встроенная запись (Alt + Q) и автоопределение OBS
static std::atomic<bool> g_isBuiltinRecording{ false };
static std::atomic<bool> g_isExternalRecording{ false };
static std::thread g_recordThread;

// Перетаскивание окна
static bool g_isDragging = false;
static POINT g_dragClickOffset = { 0, 0 };
static POINT g_dragScreenStart = { 0, 0 };
static bool g_hasDraggedDistance = false;

static int g_posX = 0;
static int g_posY = 12;

// Буфер рендеринга основного окна
static HBITMAP g_hBmp = nullptr;
static HBITMAP g_hDefaultBmp = nullptr;
static void* g_pBits = nullptr;
static HDC g_hdcMem = nullptr;

// Шрифты
static Gdiplus::PrivateFontCollection* g_pPrivateFonts = nullptr;
static std::wstring g_loadedCustomFontPath;
static Gdiplus::FontFamily* g_pFontFamily = nullptr;
static Gdiplus::Font* g_pFontCompact = nullptr;
static Gdiplus::Font* g_pFontExpTitle = nullptr;
static Gdiplus::Font* g_pFontExpArtist = nullptr;
static Gdiplus::Font* g_pFontExpTime = nullptr;

// Тайминг и анимация
static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_lastFrameTime = {};
static float g_animTime = 0.0f;
static float g_barHeights[4] = { 8.0f, 5.0f, 10.0f, 6.0f };
static float g_totalTextWidth = 80.0f;
static float g_marqueeOffset = 0.0f;
static float g_marqueePauseTimer = 1.5f;
static bool g_wasMouseDown = false;
static int g_marqueeDirection = 1;

static void RenderOverlay(HWND hwnd);
static void StepAnimationsAndRender(HWND hwnd);
static void ToggleFlyoutWindow(HWND hwndOwner);
static void RenderFlyout(HWND hwnd);

static float SmoothDamp(float current, float target, float speed, float dt) {
    return current + (target - current) * (1.0f - expf(-speed * dt));
}

static float Smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void HsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    h = fmodf(std::max(0.0f, h), 360.0f);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0, gf = 0, bf = 0;
    if (h < 60) { rf = c; gf = x; }
    else if (h < 120) { rf = x; gf = c; }
    else if (h < 180) { gf = c; bf = x; }
    else if (h < 240) { gf = x; bf = c; }
    else if (h < 300) { rf = x; bf = c; }
    else { rf = c; bf = x; }
    r = (uint8_t)std::clamp((rf + m) * 255.0f, 0.0f, 255.0f);
    g = (uint8_t)std::clamp((gf + m) * 255.0f, 0.0f, 255.0f);
    b = (uint8_t)std::clamp((bf + m) * 255.0f, 0.0f, 255.0f);
}

static void RgbToHsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxC = (std::max)({ rf, gf, bf });
    float minC = (std::min)({ rf, gf, bf });
    float delta = maxC - minC;
    v = maxC;
    s = (maxC > 0.001f) ? (delta / maxC) : 0.0f;
    if (delta < 0.001f) h = 0.0f;
    else if (maxC == rf) h = 60.0f * fmodf(((gf - bf) / delta) + 6.0f, 6.0f);
    else if (maxC == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
    else h = 60.0f * (((rf - gf) / delta) + 4.0f);
}

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ws(path);
    size_t pos = ws.find_last_of(L"\\/");
    if (pos != std::wstring::npos) return ws.substr(0, pos + 1);
    return L"";
}

static std::wstring GetConfigPath() {
    return GetExeDir() + L"overlay_config.ini";
}

static Gdiplus::Font* CreateFontSafe(Gdiplus::FontFamily* fam, float sizePx, INT preferredStyle) {
    if (!fam) return nullptr;
    INT style = preferredStyle;
    if (!fam->IsStyleAvailable(style)) {
        if (fam->IsStyleAvailable(Gdiplus::FontStyleRegular)) style = Gdiplus::FontStyleRegular;
        else if (fam->IsStyleAvailable(Gdiplus::FontStyleBold)) style = Gdiplus::FontStyleBold;
        else if (fam->IsStyleAvailable(Gdiplus::FontStyleItalic)) style = Gdiplus::FontStyleItalic;
    }
    return new Gdiplus::Font(fam, sizePx, style, Gdiplus::UnitPixel);
}

static void InitCustomFonts() {
    std::wstring dir = GetExeDir();
    std::wstring subDir = dir + L"san-francisco-pro-display\\";

    // Ищем файлы в подпапке, а если их там нет — в корневой папке
    const std::wstring candidates[] = {
        subDir + L"SF-Pro-Display-Semibold.otf",
        subDir + L"SF-Pro-Display-Regular.otf",
        dir + L"SF-Pro-Display-Semibold.otf", // запасной вариант
        dir + L"SF-Pro-Display-Regular.otf"
    };

    g_pPrivateFonts = new Gdiplus::PrivateFontCollection();
    bool customLoaded = false;

    for (const auto& fullPath : candidates) {
        if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            AddFontResourceExW(fullPath.c_str(), FR_PRIVATE, 0);
            if (g_pPrivateFonts->AddFontFile(fullPath.c_str()) == Gdiplus::Ok) {
                customLoaded = true;
            }
        }
    }

    if (customLoaded) {
        int count = g_pPrivateFonts->GetFamilyCount();
        if (count > 0) {
            Gdiplus::FontFamily* pFamList = new Gdiplus::FontFamily[count];
            int found = 0;
            if (g_pPrivateFonts->GetFamilies(count, pFamList, &found) == Gdiplus::Ok && found > 0) {
                // Берем семейство SF Pro Display
                g_pFontFamily = pFamList[0].Clone();
            }
            delete[] pFamList;
        }
    }

    // Если ничего не нашли, используем Segoe UI
    if (!g_pFontFamily || !g_pFontFamily->IsAvailable()) {
        if (g_pFontFamily) delete g_pFontFamily;
        g_pFontFamily = new Gdiplus::FontFamily(L"Segoe UI");
    }

       // Уменьшенные размеры для идеального баланса
    g_pFontCompact   = CreateFontSafe(g_pFontFamily, 13.0f, Gdiplus::FontStyleBold);    // Свернутая пилюля
    g_pFontExpTitle  = CreateFontSafe(g_pFontFamily, 13.5f, Gdiplus::FontStyleBold);    // Название трека
    g_pFontExpArtist = CreateFontSafe(g_pFontFamily, 11.5f, Gdiplus::FontStyleRegular); // Имя артиста
    g_pFontExpTime   = CreateFontSafe(g_pFontFamily, 10.0f, Gdiplus::FontStyleRegular); // Время
}

static bool CheckExternalRecorderActive() {
    bool active = false;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"obs-ffmpeg-mux.exe") == 0) {
                    active = true;
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    return active;
}

// -------------------------------------------------------------
// Встроенная запись экрана со системным звуком (WASAPI Loopback + H.264 High 45 Mbps, 60 FPS) на Alt + Q
// -------------------------------------------------------------
static void ScreenRecorderWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    int screenW = GetSystemMetrics(SM_CXSCREEN) & ~1;
    int screenH = GetSystemMetrics(SM_CYSCREEN) & ~1;
    const UINT32 fps = 60;
    const UINT32 videoBitrate = 45000000; // 45 Mbps High Profile для максимальной четкости

      // Получаем путь к системной папке "Видео" (C:\Users\Имя\Videos)
    std::wstring recDir = L"";
    PWSTR pVideosPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &pVideosPath))) {
        recDir = pVideosPath;
        recDir += L"\\Aqua Wave"; // Создаем подпапку Aqua Wave
        CoTaskMemFree(pVideosPath); // Освобождаем память
    } else {
        // Если вдруг не удалось получить путь, сохраняем рядом с exe
        recDir = GetExeDir() + L"Aqua Wave";
    }
    
    // Создаем папку, если её еще нет
    CreateDirectoryW(recDir.c_str(), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fileName[MAX_PATH];
    swprintf_s(fileName, L"%s\\Record_%04d-%02d-%02d_%02d-%02d-%02d.mp4",
        recDir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // 1. Инициализируем захват всего системного звука Windows (WASAPI Loopback в 48 кГц, 16-бит, стерео)
    IMMDeviceEnumerator* pDeviceEnum = nullptr;
    IMMDevice* pAudioDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    bool audioReady = false;

    const UINT32 audioSampleRate = 48000;
    const UINT32 audioChannels = 2;
    const UINT32 audioBits = 16;
    const UINT32 audioBlockAlign = audioChannels * (audioBits / 8); // 4 байта на сэмпл

    WAVEFORMATEX wfxLoopback = {};
    wfxLoopback.wFormatTag = WAVE_FORMAT_PCM;
    wfxLoopback.nChannels = (WORD)audioChannels;
    wfxLoopback.nSamplesPerSec = audioSampleRate;
    wfxLoopback.wBitsPerSample = (WORD)audioBits;
    wfxLoopback.nBlockAlign = (WORD)audioBlockAlign;
    wfxLoopback.nAvgBytesPerSec = audioSampleRate * audioBlockAlign;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pDeviceEnum))) && pDeviceEnum) {
        if (SUCCEEDED(pDeviceEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pAudioDevice)) && pAudioDevice) {
            if (SUCCEEDED(pAudioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient)) && pAudioClient) {
                DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                if (SUCCEEDED(pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 1000000, 0, &wfxLoopback, nullptr))) {
                    if (SUCCEEDED(pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient)) && pCaptureClient) {
                        audioReady = true;
                    }
                }
            }
        }
    }

    // 2. Настраиваем аппаратный MP4 SinkWriter (Видео H.264 High + Аудио AAC 192 kbps)
    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 3);
    if (pAttr) {
        pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        pAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    IMFSinkWriter* pSinkWriter = nullptr;
    DWORD videoStreamIndex = 0;
    DWORD audioStreamIndex = 0;
    bool writerReady = false;
    bool audioStreamAdded = false;

    if (SUCCEEDED(MFCreateSinkWriterFromURL(fileName, nullptr, pAttr, &pSinkWriter)) && pSinkWriter) {
        // Видео поток (H.264 High Profile, 45 Mbps, 60 FPS)
        IMFMediaType* pVideoOut = nullptr;
        MFCreateMediaType(&pVideoOut);
        pVideoOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pVideoOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        pVideoOut->SetUINT32(MF_MT_AVG_BITRATE, videoBitrate);
        pVideoOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        pVideoOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pVideoOut, MF_MT_FRAME_SIZE, screenW, screenH);
        MFSetAttributeRatio(pVideoOut, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(pVideoOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (SUCCEEDED(pSinkWriter->AddStream(pVideoOut, &videoStreamIndex))) {
            IMFMediaType* pVideoIn = nullptr;
            MFCreateMediaType(&pVideoIn);
            pVideoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            pVideoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            pVideoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(pVideoIn, MF_MT_FRAME_SIZE, screenW, screenH);
            MFSetAttributeRatio(pVideoIn, MF_MT_FRAME_RATE, fps, 1);
            MFSetAttributeRatio(pVideoIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            if (SUCCEEDED(pSinkWriter->SetInputMediaType(videoStreamIndex, pVideoIn, nullptr))) {
                writerReady = true;
            }
            pVideoIn->Release();
        }
        pVideoOut->Release();

        // Аудио поток (AAC 192 kbps, 48000 Hz, стерео)
        if (writerReady && audioReady) {
            IMFMediaType* pAudioOut = nullptr;
            MFCreateMediaType(&pAudioOut);
            pAudioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            pAudioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            pAudioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            pAudioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioSampleRate);
            pAudioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audioChannels);
            pAudioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbps

            if (SUCCEEDED(pSinkWriter->AddStream(pAudioOut, &audioStreamIndex))) {
                IMFMediaType* pAudioIn = nullptr;
                MFCreateMediaType(&pAudioIn);
                pAudioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pAudioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                pAudioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audioChannels);
                pAudioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioSampleRate);
                pAudioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, audioBits);
                pAudioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audioBlockAlign);
                pAudioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioSampleRate * audioBlockAlign);
                pAudioIn->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

                if (SUCCEEDED(pSinkWriter->SetInputMediaType(audioStreamIndex, pAudioIn, nullptr))) {
                    audioStreamAdded = true;
                }
                pAudioIn->Release();
            }
            pAudioOut->Release();
        }

        if (writerReady && FAILED(pSinkWriter->BeginWriting())) {
            writerReady = false;
        }
    }
    if (pAttr) pAttr->Release();

    if (writerReady) {
        if (audioStreamAdded && pAudioClient) {
            pAudioClient->Start();
        }

        HDC hdcScreen = GetDC(nullptr);
        HDC hdcCapture = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = screenW;
        bmi.bmiHeader.biHeight = screenH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pFrameBits = nullptr;
        HBITMAP hCapBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pFrameBits, nullptr, 0);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcCapture, hCapBmp);
        const DWORD cbVideoBuffer = screenW * screenH * 4;

        LARGE_INTEGER freq, startCounter, curCounter;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&startCounter);
        LONGLONG nextFrameTime100ns = 0;
        const LONGLONG frameDuration100ns = 10000000LL / fps;
        LONGLONG audioTime100ns = 0;

        auto writeAudioChunk = [&](const BYTE* pPcmData, UINT32 numFrames, bool isSilent) {
            if (!audioStreamAdded || numFrames == 0) return;
            DWORD cbAudio = numFrames * audioBlockAlign;
            LONGLONG dur100ns = ((LONGLONG)numFrames * 10000000LL) / audioSampleRate;

            IMFMediaBuffer* pAudBuf = nullptr;
            if (SUCCEEDED(MFCreateMemoryBuffer(cbAudio, &pAudBuf))) {
                BYTE* pDst = nullptr;
                if (SUCCEEDED(pAudBuf->Lock(&pDst, nullptr, nullptr))) {
                    if (isSilent || !pPcmData) memset(pDst, 0, cbAudio);
                    else memcpy(pDst, pPcmData, cbAudio);
                    pAudBuf->Unlock();
                    pAudBuf->SetCurrentLength(cbAudio);

                    IMFSample* pAudSample = nullptr;
                    if (SUCCEEDED(MFCreateSample(&pAudSample))) {
                        pAudSample->AddBuffer(pAudBuf);
                        pAudSample->SetSampleTime(audioTime100ns);
                        pAudSample->SetSampleDuration(dur100ns);
                        pSinkWriter->WriteSample(audioStreamIndex, pAudSample);
                        pAudSample->Release();
                    }
                }
                pAudBuf->Release();
            }
            audioTime100ns += dur100ns;
        };

        while (g_isBuiltinRecording && g_running) {
            QueryPerformanceCounter(&curCounter);
            LONGLONG elapsed100ns = ((curCounter.QuadPart - startCounter.QuadPart) * 10000000LL) / freq.QuadPart;

            // 1. Захватываем кадр экрана + курсор
            BitBlt(hdcCapture, 0, 0, screenW, screenH, hdcScreen, 0, 0, SRCCOPY);

            CURSORINFO ci = { sizeof(CURSORINFO) };
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii = {};
                if (GetIconInfo(ci.hCursor, &ii)) {
                    DrawIconEx(hdcCapture, ci.ptScreenPos.x - (int)ii.xHotspot, ci.ptScreenPos.y - (int)ii.yHotspot,
                               ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                }
            }
            GdiFlush();

            IMFMediaBuffer* pBuffer = nullptr;
            if (SUCCEEDED(MFCreateMemoryBuffer(cbVideoBuffer, &pBuffer))) {
                BYTE* pData = nullptr;
                if (SUCCEEDED(pBuffer->Lock(&pData, nullptr, nullptr))) {
                    memcpy(pData, pFrameBits, cbVideoBuffer);
                    pBuffer->Unlock();
                    pBuffer->SetCurrentLength(cbVideoBuffer);

                    IMFSample* pSample = nullptr;
                    if (SUCCEEDED(MFCreateSample(&pSample))) {
                        pSample->AddBuffer(pBuffer);
                        pSample->SetSampleTime(elapsed100ns);
                        pSample->SetSampleDuration(frameDuration100ns);
                        pSinkWriter->WriteSample(videoStreamIndex, pSample);
                        pSample->Release();
                    }
                }
                pBuffer->Release();
            }

            // 2. Считываем все накопленные пакеты системного звука из WASAPI Loopback
            if (audioStreamAdded && pCaptureClient) {
                UINT32 packetLength = 0;
                while (SUCCEEDED(pCaptureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
                    BYTE* pAudioData = nullptr;
                    UINT32 numFramesAvailable = 0;
                    DWORD flags = 0;
                    if (SUCCEEDED(pCaptureClient->GetBuffer(&pAudioData, &numFramesAvailable, &flags, nullptr, nullptr))) {
                        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                        writeAudioChunk(pAudioData, numFramesAvailable, silent);
                        pCaptureClient->ReleaseBuffer(numFramesAvailable);
                    } else {
                        break;
                    }
                }

                // Если в системе полная тишина и WASAPI не шлёт пакеты, дописываем тишину для идеальной синхронизации звука с видео
                while (elapsed100ns - audioTime100ns >= 200000LL) { // 20 мс = 960 сэмплов
                    writeAudioChunk(nullptr, 960, true);
                }
            }

            nextFrameTime100ns += frameDuration100ns;
            QueryPerformanceCounter(&curCounter);
            LONGLONG now100ns = ((curCounter.QuadPart - startCounter.QuadPart) * 10000000LL) / freq.QuadPart;
            if (nextFrameTime100ns > now100ns) {
                DWORD sleepMs = (DWORD)((nextFrameTime100ns - now100ns) / 10000LL);
                if (sleepMs > 0) Sleep(sleepMs);
            } else {
                nextFrameTime100ns = now100ns;
            }
        }

        if (audioStreamAdded && pAudioClient) {
            pAudioClient->Stop();
        }
        pSinkWriter->Finalize();

        SelectObject(hdcCapture, hOldBmp);
        DeleteObject(hCapBmp);
        DeleteDC(hdcCapture);
        ReleaseDC(nullptr, hdcScreen);
    }

    if (pCaptureClient) pCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pAudioDevice) pAudioDevice->Release();
    if (pDeviceEnum) pDeviceEnum->Release();
    if (pSinkWriter) pSinkWriter->Release();
    MFShutdown();
    CoUninitialize();
}

static void ToggleBuiltinRecording() {
    if (g_isBuiltinRecording) {
        g_isBuiltinRecording = false;
        if (g_recordThread.joinable()) g_recordThread.join();
    } else {
        if (g_recordThread.joinable()) g_recordThread.join();
        g_isBuiltinRecording = true;
        g_recordThread = std::thread(ScreenRecorderWorker);
    }
}

static std::wstring NormalizeSpacing(const std::wstring& str) {
    std::wstring res;
    bool lastSpace = false;
    for (wchar_t ch : str) {
        if (ch == L' ' || ch == L'\t' || ch == 0x00A0) {
            if (!lastSpace && !res.empty()) {
                res += L' ';
                lastSpace = true;
            }
        } else {
            res += ch;
            lastSpace = false;
        }
    }
    while (!res.empty() && res.back() == L' ') res.pop_back();
    return res;
}

static std::wstring CleanTitle(const std::wstring& raw) {
    std::wstring s = NormalizeSpacing(raw);
    const std::wstring suffixes[] = {
        L" - YouTube Music", L" - YouTube", L" | VK \u041C\u0443\u0437\u044B\u043A\u0430", L" - SoundCloud", L" - Spotify"
    };
    for (const auto& suf : suffixes) {
        if (s.size() > suf.size() && s.rfind(suf) == (s.size() - suf.size())) {
            s = s.substr(0, s.size() - suf.size());
            s = NormalizeSpacing(s);
            break;
        }
    }
    return s;
}

static std::wstring FormatTimeSec(double totalSec) {
    if (totalSec < 0.0 || std::isnan(totalSec) || std::isinf(totalSec)) totalSec = 0.0;
    int s = (int)totalSec;
    int mins = s / 60;
    int secs = s % 60;
    wchar_t buf[32];
    swprintf_s(buf, L"%d:%02d", mins, secs);
    return buf;
}

static void LoadConfig() {
    std::wstring cfg = GetConfigPath();
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    g_posX = GetPrivateProfileIntW(L"Position", L"X", screenW / 2, cfg.c_str());
    g_posY = GetPrivateProfileIntW(L"Position", L"Y", 12, cfg.c_str());
    g_isPositionLocked = GetPrivateProfileIntW(L"Settings", L"Locked", 0, cfg.c_str()) != 0;

    g_bgR = (uint8_t)GetPrivateProfileIntW(L"Appearance_v2", L"ColorR", 32, cfg.c_str());
    g_bgG = (uint8_t)GetPrivateProfileIntW(L"Appearance_v2", L"ColorG", 39, cfg.c_str());
    g_bgB = (uint8_t)GetPrivateProfileIntW(L"Appearance_v2", L"ColorB", 60, cfg.c_str());
    g_bgAlpha = (uint8_t)GetPrivateProfileIntW(L"Appearance_v2", L"Alpha", 216, cfg.c_str());

    RgbToHsv(g_bgR, g_bgG, g_bgB, g_pickerH, g_pickerS, g_pickerV);

    if (g_posX < 150 || g_posX > screenW - 150) g_posX = screenW / 2;
    if (g_posY < 0 || g_posY > 300) g_posY = 12;
}

static void SaveConfig() {
    std::wstring cfg = GetConfigPath();
    WritePrivateProfileStringW(L"Position", L"X", std::to_wstring(g_posX).c_str(), cfg.c_str());
    WritePrivateProfileStringW(L"Position", L"Y", std::to_wstring(g_posY).c_str(), cfg.c_str());
    WritePrivateProfileStringW(L"Settings", L"Locked", g_isPositionLocked ? L"1" : L"0", cfg.c_str());
    WritePrivateProfileStringW(L"Appearance_v2", L"ColorR", std::to_wstring(g_bgR).c_str(), cfg.c_str());
    WritePrivateProfileStringW(L"Appearance_v2", L"ColorG", std::to_wstring(g_bgG).c_str(), cfg.c_str());
    WritePrivateProfileStringW(L"Appearance_v2", L"ColorB", std::to_wstring(g_bgB).c_str(), cfg.c_str());
    WritePrivateProfileStringW(L"Appearance_v2", L"Alpha", std::to_wstring(g_bgAlpha).c_str(), cfg.c_str());
}

static GlobalSystemMediaTransportControlsSession PickActiveSession(const GlobalSystemMediaTransportControlsSessionManager& mgr) {
    if (!mgr) return nullptr;
    auto sessions = mgr.GetSessions();
    for (auto s : sessions) {
        try {
            auto pb = s.GetPlaybackInfo();
            if (pb && pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                return s;
            }
        } catch (...) {}
    }
    try {
        auto cur = mgr.GetCurrentSession();
        if (cur) return cur;
    } catch (...) {}
    if (sessions.Size() > 0) return sessions.GetAt(0);
    return nullptr;
}

static void SendMediaCommand(int cmd, double seekSec = 0.0) {
    std::thread([cmd, seekSec]() {
        try {
            init_apartment(apartment_type::multi_threaded);
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = PickActiveSession(mgr);
            if (session) {
                if (cmd == 1) session.TryTogglePlayPauseAsync().get();
                else if (cmd == 2) session.TrySkipNextAsync().get();
                else if (cmd == 3) session.TrySkipPreviousAsync().get();
                else if (cmd == 4) {
                    int64_t startTicks = 0;
                    try {
                        auto tl = session.GetTimelineProperties();
                        if (tl) startTicks = tl.StartTime().count();
                    } catch (...) {}
                    int64_t targetTicks = startTicks + static_cast<int64_t>(seekSec * 10000000.0);
                    session.TryChangePlaybackPositionAsync(targetTicks).get();
                }
            }
        } catch (...) {}
    }).detach();
}

static void BuildRoundedPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    if (r * 2.0f > h) r = h * 0.5f;
    if (r * 2.0f > w) r = w * 0.5f;
    float d = r * 2.0f;
    if (d <= 0.1f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void DrawRoundedPill(Gdiplus::Graphics& g, Gdiplus::Brush* brush, float x, float y, float w, float h, float r) {
    if (w <= 0.0f || h <= 0.0f || !brush) return;
    Gdiplus::GraphicsPath path;
    BuildRoundedPath(path, x, y, w, h, r);
    g.FillPath(brush, &path);
}

static void DrawSquircleDropletSDF(uint32_t* pBuf, int bufW, int bufH,
                                   float x, float y, float w, float h,
                                   float rxCap, float ryCap, float exponent,
                                   uint8_t rCol, uint8_t gCol, uint8_t bCol, uint8_t aCol) {
    if (!pBuf || w <= 1.0f || h <= 1.0f || aCol == 0) return;

    const float kGlowSpread     = 18.0f; 
    const float kGlowIntensity  = 0.65f; 

    float cx = x + w * 0.5f;
    float cy = y + h * 0.5f;
    float hx = w * 0.5f;
    float hy = h * 0.5f;

    float R = std::clamp((std::min)(rxCap, ryCap), 2.0f, (std::min)(hx, hy));
    float flatX = hx - R;
    float flatY = hy - R;
    float invExp = 1.0f / exponent;

    int minX = (std::max)(0, (int)floorf(x - kGlowSpread - 2.0f));
    int maxX = (std::min)(bufW - 1, (int)ceilf(x + w + kGlowSpread + 2.0f));
    int minY = (std::max)(0, (int)floorf(y - kGlowSpread - 2.0f));
    int maxY = (std::min)(bufH - 1, (int)ceilf(y + h + kGlowSpread + 2.0f));

    float baseAlpha = aCol / 255.0f;

    for (int py = minY; py <= maxY; ++py) {
        uint32_t* row = pBuf + py * bufW;
        float qy = fabsf((py + 0.5f) - cy) - flatY;
        float pyPos = (std::max)(qy, 0.0f);
        float pyPow = (pyPos > 0.0f) ? powf(pyPos, exponent) : 0.0f;

        for (int px = minX; px <= maxX; ++px) {
            float qx = fabsf((px + 0.5f) - cx) - flatX;
            float pxPos = (std::max)(qx, 0.0f);

            float outDist = 0.0f;
            if (pxPos > 0.0f && pyPos > 0.0f) {
                outDist = powf(powf(pxPos, exponent) + pyPow, invExp);
            } else {
                outDist = pxPos + pyPos;
            }
            float inDist = (std::min)((std::max)(qx, qy), 0.0f);
            float dist = outDist + inDist - R;

            if (dist < kGlowSpread) {
                float bodyCov = std::clamp((0.5f - dist) / 1.2f, 0.0f, 1.0f);
                bodyCov = bodyCov * bodyCov * (3.0f - 2.0f * bodyCov);

                float glowDist = (std::max)(0.0f, dist);
                float t = std::clamp(1.0f - (glowDist / kGlowSpread), 0.0f, 1.0f);
                float glowCurve = t * t * t;
                float glowAlpha = baseAlpha * kGlowIntensity * glowCurve;

                float finalA = bodyCov * baseAlpha + (1.0f - bodyCov) * glowAlpha;
                uint32_t a8 = (uint32_t)(finalA * 255.0f + 0.5f);

                if (a8 > 0) {
                    uint32_t r8 = (rCol * a8) / 255;
                    uint32_t g8 = (gCol * a8) / 255;
                    uint32_t b8 = (bCol * a8) / 255;

                    // ПРАВИЛЬНОЕ СМЕШИВАНИЕ ЦВЕТОВ (никаких черных дыр)
                    uint32_t dst = row[px];
                    uint32_t dstA = (dst >> 24) & 0xFF;

                    if (dstA == 0) {
                        row[px] = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
                    } else {
                        uint32_t dstR = (dst >> 16) & 0xFF;
                        uint32_t dstG = (dst >> 8) & 0xFF;
                        uint32_t dstB = dst & 0xFF;

                        uint32_t invA = 255 - a8;
                        uint32_t outA = a8 + (dstA * invA) / 255;
                        if (outA > 0) {
                            uint32_t outR = r8 + (dstR * invA) / 255;
                            uint32_t outG = g8 + (dstG * invA) / 255;
                            uint32_t outB = b8 + (dstB * invA) / 255;
                            row[px] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
                        }
                    }
                }
            }
        }
    }
}

static Gdiplus::Bitmap* CreateMaskedHiResBitmap(Gdiplus::Bitmap* rawBmp, int targetSize, float radius) {
    if (!rawBmp || targetSize <= 4) return nullptr;
    int sw = rawBmp->GetWidth();
    int sh = rawBmp->GetHeight();
    if (sw <= 0 || sh <= 0) return nullptr;

    Gdiplus::Bitmap* resBmp = new Gdiplus::Bitmap(targetSize, targetSize, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics gRes(resBmp);
        gRes.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gRes.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        gRes.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        float scale = (std::max)((float)targetSize / sw, (float)targetSize / sh);
        float dw = sw * scale;
        float dh = sh * scale;
        float dx = (targetSize - dw) * 0.5f;
        float dy = (targetSize - dh) * 0.5f;
        gRes.DrawImage(rawBmp, Gdiplus::RectF(dx, dy, dw, dh));
    }

    Gdiplus::BitmapData data;
    Gdiplus::Rect rect(0, 0, targetSize, targetSize);
    if (resBmp->LockBits(&rect, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
        const float half = targetSize * 0.5f;
        const float R = std::clamp(radius, 1.0f, half);

        for (int y = 0; y < targetSize; ++y) {
            uint8_t* row = (uint8_t*)data.Scan0 + y * data.Stride;
            for (int x = 0; x < targetSize; ++x) {
                uint8_t* px = row + x * 4;
                float qx = fabsf((x + 0.5f) - half) - (half - R);
                float qy = fabsf((y + 0.5f) - half) - (half - R);
                float outDist = sqrtf((std::max)(qx, 0.0f) * (std::max)(qx, 0.0f) + (std::max)(qy, 0.0f) * (std::max)(qy, 0.0f));
                float inDist = (std::min)((std::max)(qx, qy), 0.0f);
                float dist = outDist + inDist - R;

                float edgeAlpha = std::clamp(0.5f - dist, 0.0f, 1.0f);
                px[3] = (uint8_t)((px[3] / 255.0f) * edgeAlpha * 255.0f);
            }
        }
        resBmp->UnlockBits(&data);
    }
    return resBmp;
}

static Gdiplus::Bitmap* CreateBitmapWithWIC(const uint8_t* data, size_t size) {
    if (!data || size == 0 || !g_pWicFactory) return nullptr;

    IWICStream* pStream = nullptr;
    if (FAILED(g_pWicFactory->CreateStream(&pStream))) return nullptr;
    if (FAILED(pStream->InitializeFromMemory((BYTE*)data, (DWORD)size))) { pStream->Release(); return nullptr; }

    IWICBitmapDecoder* pDecoder = nullptr;
    if (FAILED(g_pWicFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
        pStream->Release(); return nullptr;
    }

    IWICBitmapFrameDecode* pFrame = nullptr;
    if (FAILED(pDecoder->GetFrame(0, &pFrame))) { pDecoder->Release(); pStream->Release(); return nullptr; }

    IWICFormatConverter* pConverter = nullptr;
    if (FAILED(g_pWicFactory->CreateFormatConverter(&pConverter))) {
        pFrame->Release(); pDecoder->Release(); pStream->Release(); return nullptr;
    }

    if (FAILED(pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        pConverter->Release(); pFrame->Release(); pDecoder->Release(); pStream->Release(); return nullptr;
    }

    UINT width = 0, height = 0;
    pConverter->GetSize(&width, &height);
    if (width == 0 || height == 0) {
        pConverter->Release(); pFrame->Release(); pDecoder->Release(); pStream->Release(); return nullptr;
    }

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(width, height, PixelFormat32bppARGB);
    Gdiplus::BitmapData bmpData;
    Gdiplus::Rect rect(0, 0, width, height);
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
        pConverter->CopyPixels(nullptr, bmpData.Stride, bmpData.Stride * height, (BYTE*)bmpData.Scan0);
        bmp->UnlockBits(&bmpData);
    } else {
        delete bmp; bmp = nullptr;
    }

    pConverter->Release(); pFrame->Release(); pDecoder->Release(); pStream->Release();
    return bmp;
}

static void BuildArtPairFromRaw(Gdiplus::Bitmap* rawBmp, Gdiplus::Bitmap** outCircle, Gdiplus::Bitmap** outSquare) {
    if (*outCircle) { delete *outCircle; *outCircle = nullptr; }
    if (*outSquare) { delete *outSquare; *outSquare = nullptr; }
    if (!rawBmp) return;
    *outCircle = CreateMaskedHiResBitmap(rawBmp, 128, 64.0f);
    *outSquare = CreateMaskedHiResBitmap(rawBmp, 160, 38.0f);
}

static void LoadDefaultLolPair() {
    std::wstring path = GetExeDir() + L"lol.jpg";
    Gdiplus::Bitmap* rawBmp = nullptr;

    if (g_pWicFactory) {
        IWICBitmapDecoder* pDecoder = nullptr;
        if (SUCCEEDED(g_pWicFactory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
            IWICBitmapFrameDecode* pFrame = nullptr;
            if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame))) {
                IWICFormatConverter* pConverter = nullptr;
                if (SUCCEEDED(g_pWicFactory->CreateFormatConverter(&pConverter))) {
                    if (SUCCEEDED(pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                        UINT w = 0, h = 0;
                        pConverter->GetSize(&w, &h);
                        if (w > 0 && h > 0) {
                            rawBmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
                            Gdiplus::BitmapData bd;
                            Gdiplus::Rect rc(0, 0, w, h);
                            if (rawBmp->LockBits(&rc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                                pConverter->CopyPixels(nullptr, bd.Stride, bd.Stride * h, (BYTE*)bd.Scan0);
                                rawBmp->UnlockBits(&bd);
                            } else {
                                delete rawBmp; rawBmp = nullptr;
                            }
                        }
                    }
                    pConverter->Release();
                }
                pFrame->Release();
            }
            pDecoder->Release();
        }
    }

    if (!rawBmp) {
        Gdiplus::Bitmap* gdipBmp = Gdiplus::Bitmap::FromFile(path.c_str());
        if (gdipBmp && gdipBmp->GetLastStatus() == Gdiplus::Ok) rawBmp = gdipBmp;
        else delete gdipBmp;
    }

    if (rawBmp) {
        BuildArtPairFromRaw(rawBmp, &g_pDefCircleHi, &g_pDefSquareHi);
        delete rawBmp;
    }
}

static void DrawAlbumArtLayer(Gdiplus::Graphics& g, bool isSquareMode, float cx, float cy, float size, float brightness, float alpha) {
    if (size <= 2.0f || alpha <= 0.005f) return;

    Gdiplus::Bitmap* bmp = isSquareMode
        ? (g_pTrackSquareHi ? g_pTrackSquareHi : g_pDefSquareHi)
        : (g_pTrackCircleHi ? g_pTrackCircleHi : g_pDefCircleHi);

    float x = cx - size * 0.5f;
    float y = cy - size * 0.5f;
    Gdiplus::RectF dstRect(x, y, size, size);

    if (bmp) {
        Gdiplus::ImageAttributes imgAttr;
        Gdiplus::ColorMatrix cm = {
            brightness, 0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, brightness, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, brightness, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, alpha, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f
        };
        imgAttr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
        g.DrawImage(bmp, dstRect, 0.0f, 0.0f, (float)bmp->GetWidth(), (float)bmp->GetHeight(), Gdiplus::UnitPixel, &imgAttr);
        return;
    }

    float radius = isSquareMode ? (size * 0.24f) : (size * 0.5f);
    Gdiplus::GraphicsPath path;
    BuildRoundedPath(path, x, y, size, size, radius);
    BYTE a = (BYTE)(255 * alpha);
    BYTE c1 = (BYTE)(24 * brightness);
    Gdiplus::SolidBrush discBg(Gdiplus::Color(a, c1, c1 + 4, c1 + 18));
    g.FillPath(&discBg, &path);
}

static void DrawCrispString(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::Font* font,
                            const Gdiplus::RectF& rect, Gdiplus::StringFormat* fmt, Gdiplus::Color color) {
    Gdiplus::SolidBrush br(color);
    
    // Округляем координаты до целых пикселей, чтобы убрать "мыло"
    Gdiplus::RectF snapRect(
        std::round(rect.X), 
        std::round(rect.Y), 
        std::round(rect.Width), 
        std::round(rect.Height)
    );
    
    g.DrawString(text.c_str(), -1, font, snapRect, fmt, &br);
}

static void DrawRoundedTriangle(Gdiplus::Graphics& g, Gdiplus::PointF p1, Gdiplus::PointF p2, Gdiplus::PointF p3, float cornerWeight, Gdiplus::Color color) {
    Gdiplus::PointF pts[3] = { p1, p2, p3 };
    Gdiplus::SolidBrush br(color);
    Gdiplus::Pen pen(color, cornerWeight);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.FillPolygon(&br, pts, 3);
    g.DrawPolygon(&pen, pts, 3);
}

static void DrawRewindIcon(Gdiplus::Graphics& g, float cx, float cy, float scale, float alpha) {
    if (alpha <= 0.01f) return;
    Gdiplus::Color col((BYTE)(255 * alpha), 205, 214, 232);
    float s = scale;
    float w = 5.8f * s;
    float h = 4.3f * s;
    float gap = 6.8f * s;

    float c1x = cx - gap * 0.5f;
    DrawRoundedTriangle(g,
        Gdiplus::PointF(c1x - w * 0.5f, cy),
        Gdiplus::PointF(c1x + w * 0.5f, cy - h),
        Gdiplus::PointF(c1x + w * 0.5f, cy + h),
        2.2f * s, col);

    float c2x = cx + gap * 0.5f;
    DrawRoundedTriangle(g,
        Gdiplus::PointF(c2x - w * 0.5f, cy),
        Gdiplus::PointF(c2x + w * 0.5f, cy - h),
        Gdiplus::PointF(c2x + w * 0.5f, cy + h),
        2.2f * s, col);
}

static void DrawFastForwardIcon(Gdiplus::Graphics& g, float cx, float cy, float scale, float alpha) {
    if (alpha <= 0.01f) return;
    Gdiplus::Color col((BYTE)(255 * alpha), 205, 214, 232);
    float s = scale;
    float w = 5.8f * s;
    float h = 4.3f * s;
    float gap = 6.8f * s;

    float c1x = cx - gap * 0.5f;
    DrawRoundedTriangle(g,
        Gdiplus::PointF(c1x + w * 0.5f, cy),
        Gdiplus::PointF(c1x - w * 0.5f, cy - h),
        Gdiplus::PointF(c1x - w * 0.5f, cy + h),
        2.2f * s, col);

    float c2x = cx + gap * 0.5f;
    DrawRoundedTriangle(g,
        Gdiplus::PointF(c2x + w * 0.5f, cy),
        Gdiplus::PointF(c2x - w * 0.5f, cy - h),
        Gdiplus::PointF(c2x - w * 0.5f, cy + h),
        2.2f * s, col);
}

static void DrawCenterPlayPause(Gdiplus::Graphics& g, float cx, float cy, float playAnim, float pulseScale, float alpha) {
    if (alpha <= 0.01f) return;

    float triVis = std::clamp(1.0f - playAnim, 0.0f, 1.0f);
    if (triVis > 0.01f) {
        float s = pulseScale * (0.65f + 0.35f * triVis);
        BYTE a = (BYTE)(255 * alpha * triVis);
        Gdiplus::Color col(a, 255, 255, 255);
        float w = 12.5f * s;
        float h = 7.6f * s;
        DrawRoundedTriangle(g,
            Gdiplus::PointF(cx + w * 0.58f, cy),
            Gdiplus::PointF(cx - w * 0.42f, cy - h),
            Gdiplus::PointF(cx - w * 0.42f, cy + h),
            3.8f * s, col);
    }

    float barVis = std::clamp(playAnim, 0.0f, 1.0f);
    if (barVis > 0.01f) {
        float s = pulseScale * (0.65f + 0.35f * barVis);
        BYTE a = (BYTE)(255 * alpha * barVis);
        Gdiplus::SolidBrush br(Gdiplus::Color(a, 255, 255, 255));
        float bw = 4.6f * s;
        float bh = 18.5f * s;
        float gap = 4.4f * s;
        float r = bw * 0.48f;
        DrawRoundedPill(g, &br, cx - gap * 0.5f - bw, cy - bh * 0.5f, bw, bh, r);
        DrawRoundedPill(g, &br, cx + gap * 0.5f, cy - bh * 0.5f, bw, bh, r);
    }
}

static float GetShapeMorphProgress() {
    return std::clamp(g_shapeAnim, 0.0f, 1.0f);
}

static void GetCurrentPillRect(float& outX, float& outY, float& outW, float& outH) {
    float shapeM = GetShapeMorphProgress();
    outW = g_curCompactW + (kExpandedWidth - g_curCompactW) * shapeM;
    outH = kCompactHeight + (kExpandedHeight - kCompactHeight) * shapeM;
    outX = kMainAnchorX - outW * 0.5f;
    outY = (float)kCanvasPadY;
}

static void UpdateCompactWidthForText(const std::wstring& text) {
    Gdiplus::Bitmap measureBmp(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics measureG(&measureBmp);

    Gdiplus::StringFormat format; // Убрали GenericTypographic для четкости
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetAlignment(Gdiplus::StringAlignmentNear);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::RectF bound;
    measureG.MeasureString(text.c_str(), -1, g_pFontCompact, Gdiplus::PointF(0, 0), &format, &bound);
    g_totalTextWidth = bound.Width + 5.0f;
    float neededW = g_totalTextWidth + kCompactTextHorizPad;
    g_targetCompactW = std::clamp(neededW, kMinCompactWidth, kMaxCompactWidth);
}

static void MediaWorker(HWND hwnd) {
    init_apartment(apartment_type::multi_threaded);

    uint64_t ver = 0;
    std::wstring currentTrackKey = L"";
    bool lastPlaying = false;
    std::vector<uint8_t> currentThumbBytes;
    std::vector<uint8_t> notifiedThumbBytes;
    std::vector<uint8_t> prevTrackThumbBytes;
    int thumbPollRemaining = 0;
    double cachedDur = 0.0;
    int obsCheckCounter = 0;

    while (g_running) {
        if (++obsCheckCounter >= 5) {
            obsCheckCounter = 0;
            g_isExternalRecording = CheckExternalRecorderActive();
        }

        try {
            auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            GlobalSystemMediaTransportControlsSession activeSession = PickActiveSession(mgr);

            if (activeSession) {
                auto pb = activeSession.GetPlaybackInfo();
                bool isPlaying = pb ? (pb.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) : false;
                auto props = activeSession.TryGetMediaPropertiesAsync().get();

                if (props) {
                    std::wstring title = CleanTitle(props.Title().c_str());
                    std::wstring artist = CleanTitle(props.Artist().c_str());
                    if (title.empty()) title = L"AQUA WAVE";

                    std::wstring trackKey = title + L"|" + artist;
                    bool trackChanged = (trackKey != currentTrackKey);

                    if (trackChanged) {
                        prevTrackThumbBytes = currentThumbBytes;
                        currentTrackKey = trackKey;
                        currentThumbBytes.clear();
                        notifiedThumbBytes.clear();
                        thumbPollRemaining = 24;
                        cachedDur = 0.0;
                        {
                            std::lock_guard<std::mutex> lock(g_mediaMutex);
                            g_manualSeekBaseSec = -1.0;
                        }
                    }

                    double posSec = 0.0;
                    double durSec = 0.0;
                    double rawPosS = 0.0;
                    try {
                        auto tl = activeSession.GetTimelineProperties();
                        if (tl) {
                            double startS = tl.StartTime().count() / 10000000.0;
                            double endS = tl.EndTime().count() / 10000000.0;
                            double maxSeekS = tl.MaxSeekTime().count() / 10000000.0;
                            rawPosS = (tl.Position().count() / 10000000.0) - startS;
                            if (rawPosS < 0.0) rawPosS = tl.Position().count() / 10000000.0;

                            durSec = endS - startS;
                            if (durSec <= 1.0 && maxSeekS > 1.0) durSec = maxSeekS - startS;
                            if (durSec <= 1.0 && endS > 1.0) durSec = endS;

                            posSec = rawPosS;
                            if (isPlaying) {
                                auto now = winrt::clock::now();
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - tl.LastUpdatedTime()).count() / 1000.0;
                                if (elapsed > 0.0 && elapsed < 3600.0) posSec += elapsed;
                            }
                            if (posSec < 0.0) posSec = 0.0;
                        }
                    } catch (...) {}

                    if (durSec > 1.0) cachedDur = durSec;
                    else if (cachedDur > 1.0) durSec = cachedDur;
                    else durSec = 180.0;

                    if (thumbPollRemaining > 0) {
                        thumbPollRemaining--;
                        try {
                            auto thumbRef = props.Thumbnail();
                            if (thumbRef) {
                                auto stream = thumbRef.OpenReadAsync().get();
                                if (stream) {
                                    auto size = stream.Size();
                                    if (size > 0 && size < 15 * 1024 * 1024) {
                                        DataReader reader(stream);
                                        reader.LoadAsync(static_cast<uint32_t>(size)).get();
                                        std::vector<uint8_t> fetched(static_cast<size_t>(size));
                                        reader.ReadBytes(winrt::array_view<uint8_t>(fetched));
                                        if (!fetched.empty()) {
                                            bool isStaleFromPrev = (!prevTrackThumbBytes.empty() && fetched == prevTrackThumbBytes);
                                            if (!isStaleFromPrev || thumbPollRemaining < 12) {
                                                currentThumbBytes = std::move(fetched);
                                                if (!isStaleFromPrev) thumbPollRemaining = 0;
                                            }
                                        }
                                    }
                                }
                            }
                        } catch (...) {}
                    }

                    bool thumbUpdated = (currentThumbBytes != notifiedThumbBytes);
                    bool playbackChanged = (isPlaying != lastPlaying);
                    uint64_t nowTick = GetTickCount64();

                    {
                        std::lock_guard<std::mutex> lock(g_mediaMutex);
                        if (!g_seekingTimeline && nowTick >= g_ignoreWorkerSeekUntil) {
                            if (g_manualSeekBaseSec >= 0.0 && rawPosS < 0.5) {
                                if (isPlaying) posSec = g_manualSeekBaseSec + (nowTick - g_manualSeekTick) / 1000.0;
                                else { posSec = g_manualSeekBaseSec; g_manualSeekTick = nowTick; }
                            } else if (rawPosS >= 0.5) {
                                g_manualSeekBaseSec = -1.0;
                            }

                            if (posSec > durSec) durSec = posSec + 15.0;
                            g_sharedMedia.positionSec = posSec;
                            g_sharedMedia.durationSec = durSec;
                            g_sharedMedia.lastQueryTick = nowTick;
                        }
                        if (nowTick >= g_ignoreWorkerPlayUntil) {
                            g_sharedMedia.isPlaying = isPlaying;
                        }
                    }

                    if (trackChanged || playbackChanged || thumbUpdated) {
                        lastPlaying = isPlaying;
                        notifiedThumbBytes = currentThumbBytes;
                        ver++;

                        {
                            std::lock_guard<std::mutex> lock(g_mediaMutex);
                            g_sharedMedia.title = title;
                            g_sharedMedia.artist = artist;
                            g_sharedMedia.thumbBytes = notifiedThumbBytes;
                            if (nowTick >= g_ignoreWorkerPlayUntil) {
                                g_sharedMedia.isPlaying = isPlaying;
                            }
                            g_sharedMedia.hasSession = true;
                            g_sharedMedia.version = ver;
                        }
                        PostMessageW(hwnd, WM_APP_MEDIA_CHANGED, trackChanged ? 1 : 0, 0);
                    }
                }
            } else {
                if (!currentTrackKey.empty()) {
                    currentTrackKey = L"";
                    lastPlaying = false;
                    currentThumbBytes.clear();
                    notifiedThumbBytes.clear();
                    prevTrackThumbBytes.clear();
                    thumbPollRemaining = 0;
                    cachedDur = 0.0;
                    ver++;
                    {
                        std::lock_guard<std::mutex> lock(g_mediaMutex);
                        g_sharedMedia = MediaData{};
                        g_sharedMedia.version = ver;
                        g_manualSeekBaseSec = -1.0;
                    }
                    PostMessageW(hwnd, WM_APP_MEDIA_CHANGED, 1, 0);
                }
            }
        } catch (...) {}

        Sleep(110);
    }
}

static void RenderOverlay(HWND hwnd) {
    if (!g_pBits || !g_hdcMem) return;

    memset(g_pBits, 0, kCanvasW * kCanvasH * 4);

    float pillX, pillY, pillW, pillH;
    GetCurrentPillRect(pillX, pillY, pillW, pillH);
    float shapeM = GetShapeMorphProgress();

    // 1. Фон основной капсулы с мягким Glow и углами iOS Squircle (rx=ry=44 в раскрытом виде, степень 2.55)
    float cornerR = 16.0f + (44.0f - 16.0f) * shapeM;
    float superExp = 2.0f + 0.55f * shapeM;

    DrawSquircleDropletSDF((uint32_t*)g_pBits, kCanvasW, kCanvasH,
                           pillX, pillY, pillW, pillH,
                           cornerR, cornerR, superExp,
                           g_bgR, g_bgG, g_bgB, g_bgAlpha);

    // 2. Кружок записи справа (с таким же Glow и крупной красной точкой 12px, как в эталоне)
    float recCollapsedFactor = g_compContentAlpha;
    float recVis = g_recAnim * recCollapsedFactor;
    if (recVis > 0.005f) {
        float recSize = kRecIslandSize * (0.55f + 0.45f * recVis);
        float recX = pillX + pillW + kRecIslandGap * recCollapsedFactor + (kRecIslandSize - recSize) * 0.5f;
        float recY = pillY + (kCompactHeight - recSize) * 0.5f;

        DrawSquircleDropletSDF((uint32_t*)g_pBits, kCanvasW, kCanvasH,
                               recX, recY, recSize, recSize,
                               recSize * 0.5f, recSize * 0.5f, 2.0f,
                               g_bgR, g_bgG, g_bgB, (uint8_t)(g_bgAlpha * recVis));
    }

    Gdiplus::Bitmap bmp(kCanvasW, kCanvasH, kCanvasW * 4, PixelFormat32bppPARGB, (BYTE*)g_pBits);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);

    if (recVis > 0.005f) {
        float recSize = kRecIslandSize * (0.55f + 0.45f * recVis);
        float recX = pillX + pillW + kRecIslandGap * recCollapsedFactor + (kRecIslandSize - recSize) * 0.5f;
        float recY = pillY + (kCompactHeight - recSize) * 0.5f;
        float dotSize = 12.0f * recVis;
        float dotX = recX + (recSize - dotSize) * 0.5f;
        float dotY = recY + (recSize - dotSize) * 0.5f;
        Gdiplus::SolidBrush pureRedBrush(Gdiplus::Color((BYTE)(255 * recVis), 255, 0, 0));
        g.FillEllipse(&pureRedBrush, dotX, dotY, dotSize, dotSize);
    }

    float compactAlpha = g_compContentAlpha * g_trackFadeAnim;
    float expandedAlpha = g_expContentAlpha * g_trackFadeAnim;
    float curBrightness = 0.56f + 0.44f * g_playAnim;

    // 3. СВЕРНУТЫЙ РЕЖИМ
    if (compactAlpha > 0.005f) {
        BYTE aComp = (BYTE)(255 * compactAlpha);

        float compAvSize = (19.5f + 2.5f * g_playAnim) * (0.88f + 0.12f * compactAlpha);
        DrawAlbumArtLayer(g, false, pillX + 16.0f, pillY + 16.0f, compAvSize, curBrightness, compactAlpha);

        const float kRightMargin = 11.5f;
        const float kVisCenterY = pillY + kCompactHeight * 0.5f;

        if (g_playAnim > 0.02f) {
            Gdiplus::SolidBrush visBrush(Gdiplus::Color((BYTE)(aComp * g_playAnim), 59, 130, 246));
            const int kNumBars = 4;
            const float kBarW = 2.0f;
            const float kBarGap = 2.0f;
            const float kVisTotalW = kNumBars * kBarW + (kNumBars - 1) * kBarGap;
            const float kVisX = pillX + pillW - kRightMargin - kVisTotalW;

            for (int i = 0; i < kNumBars; ++i) {
                float h = g_barHeights[i] * g_playAnim;
                if (h < 2.0f) h = 2.0f;
                float bx = kVisX + i * (kBarW + kBarGap);
                float by = kVisCenterY - h * 0.5f;
                DrawRoundedPill(g, &visBrush, bx, by, kBarW, h, 1.0f);
            }
        }
        if (g_playAnim < 0.98f) {
            float pauseVis = 1.0f - g_playAnim;
            Gdiplus::SolidBrush pauseBrush(Gdiplus::Color((BYTE)(aComp * pauseVis), 59, 130, 246));
            const float kPauseBarW = 2.5f;
            const float kPauseGap = 2.5f;
            const float kPauseH = 10.0f * (0.7f + 0.3f * pauseVis);
            const float kPauseTotalW = 2.0f * kPauseBarW + kPauseGap;
            const float kPauseX = pillX + pillW - kRightMargin - kPauseTotalW;

            DrawRoundedPill(g, &pauseBrush, kPauseX, kVisCenterY - kPauseH * 0.5f, kPauseBarW, kPauseH, 1.2f);
            DrawRoundedPill(g, &pauseBrush, kPauseX + kPauseBarW + kPauseGap, kVisCenterY - kPauseH * 0.5f, kPauseBarW, kPauseH, 1.2f);
        }

        const float kTextX = pillX + 34.0f;
        const float kTextW = (std::max)(10.0f, pillW - kCompactTextHorizPad);

        Gdiplus::StringFormat format; // Убрали GenericTypographic
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::RectF textClipRect(kTextX, pillY, kTextW, kCompactHeight);
        g.SetClip(textClipRect);

        float drawStartX = kTextX + 1.5f - ((g_totalTextWidth > kTextW) ? g_marqueeOffset : 0.0f);
        std::wstring display = g_localMedia.hasSession ? g_localMedia.displayText : L"AQUA WAVE";
        if (display.empty()) display = L"AQUA WAVE";

        Gdiplus::RectF rectText(drawStartX, pillY, g_totalTextWidth + 60.0f, kCompactHeight);
        DrawCrispString(g, display, g_pFontCompact, rectText, &format, Gdiplus::Color(aComp, 255, 255, 255));

        g.ResetClip();
    }

    // 4. РАСКРЫТЫЙ РЕЖИМ
    if (expandedAlpha > 0.005f) {
        BYTE aMain = (BYTE)(255 * expandedAlpha);
        BYTE aSub = (BYTE)(190 * expandedAlpha);
        float settleY = (1.0f - expandedAlpha) * -3.5f;

     	 float expAvSize = 48.0f; // Жестко фиксируем размер в раскрытом виде
        DrawAlbumArtLayer(g, true, pillX + 46.0f, pillY + 36.0f + settleY, expAvSize, curBrightness, expandedAlpha);

        std::wstring tStr = g_localMedia.hasSession ? g_localMedia.title : L"AQUA WAVE";
        std::wstring aStr = g_localMedia.hasSession ? g_localMedia.artist : L"never6x";
        if (aStr.empty()) aStr = L"never6x";

        Gdiplus::StringFormat strFmt; // Убрали GenericTypographic
        strFmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        strFmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

        Gdiplus::RectF titleRect(pillX + 78.0f, pillY + 20.0f + settleY, pillW - 98.0f, 18.0f);
        Gdiplus::RectF artistRect(pillX + 78.0f, pillY + 37.5f + settleY, pillW - 98.0f, 16.0f);
        DrawCrispString(g, tStr, g_pFontExpTitle, titleRect, &strFmt, Gdiplus::Color(aMain, 255, 255, 255));
        DrawCrispString(g, aStr, g_pFontExpArtist, artistRect, &strFmt, Gdiplus::Color(aSub, 154, 166, 190));

        double dur = (g_localMedia.durationSec > 1.0) ? g_localMedia.durationSec : 180.0;
        double pos = std::clamp(g_localMedia.positionSec, 0.0, dur);
        float ratio = (float)std::clamp(pos / dur, 0.0, 1.0);

        std::wstring curTimeStr = FormatTimeSec(pos);
        std::wstring totTimeStr = FormatTimeSec(dur);

        float barY = pillY + 71.0f + settleY;
        float barX = pillX + 58.0f;
        float barW = pillW - 116.0f;
        float barH = 4.2f;

        Gdiplus::SolidBrush subBr(Gdiplus::Color(aSub, 154, 166, 190));

        g.DrawString(curTimeStr.c_str(), -1, g_pFontExpTime, Gdiplus::PointF(pillX + 23.0f, barY - 4.5f), &subBr);

        Gdiplus::StringFormat rightFmt(Gdiplus::StringFormat::GenericTypographic());
        rightFmt.SetAlignment(Gdiplus::StringAlignmentFar);
        g.DrawString(totTimeStr.c_str(), -1, g_pFontExpTime, Gdiplus::PointF(pillX + pillW - 23.0f, barY - 4.5f), &rightFmt, &subBr);

        // Фон полосы прогресса (внутренний glow удален)
        Gdiplus::SolidBrush trackBgBr(Gdiplus::Color((BYTE)(200 * expandedAlpha), 54, 63, 86));
        DrawRoundedPill(g, &trackBgBr, barX, barY, barW, barH, 2.1f);

        float fillW = barW * ratio;
        if (fillW > 2.0f) {
            Gdiplus::SolidBrush trackFillBr(Gdiplus::Color(aMain, 59, 130, 246));
            DrawRoundedPill(g, &trackFillBr, barX, barY, fillW, barH, 2.1f);
        }
        float ctrlY = pillY + 100.0f + settleY;
        float centerX = pillX + pillW * 0.5f;

        float prevScale = 1.0f - 0.15f * sinf(g_prevBtnPulse * 3.14159f);
        float nextScale = 1.0f - 0.15f * sinf(g_nextBtnPulse * 3.14159f);
        float playScale = 1.0f - 0.14f * sinf(g_playBtnPulse * 3.14159f);

        DrawRewindIcon(g, centerX - 44.0f, ctrlY, prevScale, expandedAlpha);
        DrawCenterPlayPause(g, centerX, ctrlY, g_playAnim, playScale, expandedAlpha);
        DrawFastForwardIcon(g, centerX + 44.0f, ctrlY, nextScale, expandedAlpha);
    }

    HDC hdcScreen = GetDC(nullptr);
    POINT ptSrc = { 0, 0 };
    SIZE sizeWnd = { kCanvasW, kCanvasH };
    POINT ptPos = { g_posX - (int)kMainAnchorX, g_posY - kCanvasPadY };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    GdiFlush();
    UpdateLayeredWindow(hwnd, hdcScreen, &ptPos, &sizeWnd, g_hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, hdcScreen);
}

static void StepAnimationsAndRender(HWND hwnd) {
    if (!hwnd || !g_isVisible) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_lastFrameTime.QuadPart) / (float)g_qpcFreq.QuadPart;
    g_lastFrameTime = now;
    if (dt <= 0.0001f || dt > 0.05f) dt = 0.007f;

    g_animTime += dt * 3.2f;

    const bool isMouseDown =
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (g_isExpanded && isMouseDown && !g_wasMouseDown &&
        !g_seekingTimeline && !g_isDragging) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);

        float pillX, pillY, pillW, pillH;
        GetCurrentPillRect(pillX, pillY, pillW, pillH);
        RECT rcPill = {
            (LONG)pillX, (LONG)pillY,
            (LONG)(pillX + pillW), (LONG)(pillY + pillH)
        };
        if (!PtInRect(&rcPill, pt)) {
            g_isExpanded = false;
        }
    }
    g_wasMouseDown = isMouseDown;

    uint64_t nowTick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_mediaMutex);
        if (nowTick >= g_ignoreWorkerPlayUntil) {
            g_localMedia.isPlaying = g_sharedMedia.isPlaying;
        }
        if (g_sharedMedia.durationSec > 1.0) {
            g_localMedia.durationSec = g_sharedMedia.durationSec;
        } else if (g_localMedia.durationSec <= 1.0) {
            g_localMedia.durationSec = 180.0;
        }

        if (!g_seekingTimeline) {
            double basePos = g_sharedMedia.positionSec;
            if (g_localMedia.isPlaying && g_sharedMedia.lastQueryTick > 0) {
                double elapsed = (nowTick - g_sharedMedia.lastQueryTick) / 1000.0;
                if (elapsed > 0.0 && elapsed < 5.0) basePos += elapsed;
            }
            if (g_localMedia.durationSec > 1.0 && basePos > g_localMedia.durationSec) {
                basePos = g_localMedia.durationSec;
            }
            g_localMedia.positionSec = basePos;
        }
    }

    // Точная асимметричная хореография эталона (EaseOut без подвисания пустого прямоугольника):
    if (g_isExpanded) {
        // ОТКРЫТИЕ:
        // 1. Мини-контент мгновенно гаснет в первые ~45мс
        g_compContentAlpha = SmoothDamp(g_compContentAlpha, 0.0f, 34.0f, dt);
        // 2. Форма стремительно вырастает (в середине кадр пустой) и мягко доводится в конце
        g_shapeAnim = SmoothDamp(g_shapeAnim, 1.0f, 14.5f, dt);
        if (1.0f - g_shapeAnim < 0.001f) g_shapeAnim = 1.0f;
        // 3. Когда форма выросла на ~60%, внутри плавно проявляются обложка, текст, прогресс и кнопки
        if (g_shapeAnim > 0.58f) {
            g_expContentAlpha = SmoothDamp(g_expContentAlpha, 1.0f, 15.5f, dt);
            if (1.0f - g_expContentAlpha < 0.001f) g_expContentAlpha = 1.0f;
        } else {
            g_expContentAlpha = SmoothDamp(g_expContentAlpha, 0.0f, 24.0f, dt);
        }
    } else {
        // ЗАКРЫТИЕ:
        // 1. Содержимое карточки быстро гаснет
        g_expContentAlpha = SmoothDamp(g_expContentAlpha, 0.0f, 32.0f, dt);
        if (g_expContentAlpha < 0.001f) g_expContentAlpha = 0.0f;
        // 2. Форма сразу начинает плавно сжиматься обратно в пилюлю
        g_shapeAnim = SmoothDamp(g_shapeAnim, 0.0f, 14.0f, dt);
        if (g_shapeAnim < 0.001f) g_shapeAnim = 0.0f;
        // 3. Когда пилюля почти собралась (< 32%), проявляется мини-контент
        if (g_shapeAnim < 0.32f) {
            g_compContentAlpha = SmoothDamp(g_compContentAlpha, 1.0f, 17.0f, dt);
            if (1.0f - g_compContentAlpha < 0.001f) g_compContentAlpha = 1.0f;
        } else {
            g_compContentAlpha = SmoothDamp(g_compContentAlpha, 0.0f, 25.0f, dt);
        }
    }

    g_curCompactW = SmoothDamp(g_curCompactW, g_targetCompactW, 12.0f, dt);

    // Плавное отдаление и затемнение обложки при паузе
    float targetPlay = g_localMedia.isPlaying ? 1.0f : 0.0f;
    g_playAnim = SmoothDamp(g_playAnim, targetPlay, 9.5f, dt);
    if (fabsf(g_playAnim - targetPlay) < 0.0005f) g_playAnim = targetPlay;

    bool anyRec = g_isBuiltinRecording || g_isExternalRecording;
    float targetRec = anyRec ? 1.0f : 0.0f;
    g_recAnim = SmoothDamp(g_recAnim, targetRec, 11.0f, dt);
    if (fabsf(g_recAnim - targetRec) < 0.0005f) g_recAnim = targetRec;

    g_trackFadeAnim = SmoothDamp(g_trackFadeAnim, 1.0f, 9.0f, dt);

    if (g_playBtnPulse > 0.0f) g_playBtnPulse = (std::max)(0.0f, g_playBtnPulse - dt * 3.8f);
    if (g_prevBtnPulse > 0.0f) g_prevBtnPulse = (std::max)(0.0f, g_prevBtnPulse - dt * 3.8f);
    if (g_nextBtnPulse > 0.0f) g_nextBtnPulse = (std::max)(0.0f, g_nextBtnPulse - dt * 3.8f);

    if (g_playAnim > 0.01f) {
        float t0 = 5.5f + 6.5f * (0.5f + 0.5f * sinf(g_animTime * 1.8f));
        float t1 = 4.0f + 8.5f * (0.5f + 0.5f * sinf(g_animTime * 2.4f + 1.2f));
        float t2 = 6.5f + 7.5f * (0.5f + 0.5f * sinf(g_animTime * 2.0f + 2.4f));
        float t3 = 4.5f + 8.5f * (0.5f + 0.5f * sinf(g_animTime * 2.2f + 3.6f));
        g_barHeights[0] = SmoothDamp(g_barHeights[0], t0, 14.0f, dt);
        g_barHeights[1] = SmoothDamp(g_barHeights[1], t1, 14.0f, dt);
        g_barHeights[2] = SmoothDamp(g_barHeights[2], t2, 14.0f, dt);
        g_barHeights[3] = SmoothDamp(g_barHeights[3], t3, 14.0f, dt);
    }

    const float kTextMaxW = g_curCompactW - kCompactTextHorizPad;
    float maxScroll = g_totalTextWidth - kTextMaxW;
    if (maxScroll > 1.5f && g_shapeAnim < 0.05f) {
        if (g_marqueePauseTimer > 0.0f) {
            g_marqueePauseTimer -= dt;
        } else {
            g_marqueeOffset += g_marqueeDirection * 24.0f * dt;
            if (g_marqueeOffset >= maxScroll) {
                g_marqueeOffset = maxScroll;
                g_marqueeDirection = -1;
                g_marqueePauseTimer = 1.6f;
            } else if (g_marqueeOffset <= 0.0f) {
                g_marqueeOffset = 0.0f;
                g_marqueeDirection = 1;
                g_marqueePauseTimer = 1.6f;
            }
        }
    } else {
        g_marqueeOffset = SmoothDamp(g_marqueeOffset, 0.0f, 10.0f, dt);
    }

    RenderOverlay(hwnd);
}

// -------------------------------------------------------------
// Меню с интерактивной Live-палитрой цветов
// -------------------------------------------------------------
static const int kFlyW = 264;
static const int kFlyH = 266;
static const float kPalX = 18.0f;
static const float kPalY = 44.0f;
static const float kPalW = 228.0f;
static const float kPalH = 60.0f;

static bool g_draggingHueSat = false;
static bool g_draggingVal = false;
static bool g_draggingOpacity = false;

static void ApplyLiveHsvColor() {
    HsvToRgb(g_pickerH, g_pickerS, g_pickerV, g_bgR, g_bgG, g_bgB);
    if (g_hwndOverlay) RenderOverlay(g_hwndOverlay);
    if (g_hwndFlyout && IsWindow(g_hwndFlyout)) RenderFlyout(g_hwndFlyout);
}

static void RenderFlyout(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kFlyW;
    bmi.bmiHeader.biHeight = -kFlyH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    if (pBits) {
        memset(pBits, 0, kFlyW * kFlyH * 4);
        Gdiplus::Bitmap bmp(kFlyW, kFlyH, kFlyW * 4, PixelFormat32bppPARGB, (BYTE*)pBits);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        Gdiplus::SolidBrush cardBg(Gdiplus::Color(248, 18, 22, 34));
        DrawRoundedPill(g, &cardBg, 1.0f, 1.0f, kFlyW - 2.0f, kFlyH - 2.0f, 24.0f);

        Gdiplus::GraphicsPath borderPath;
        BuildRoundedPath(borderPath, 1.0f, 1.0f, kFlyW - 2.0f, kFlyH - 2.0f, 24.0f);
        Gdiplus::Pen borderPen(Gdiplus::Color(75, 255, 255, 255), 1.0f);
        g.DrawPath(&borderPen, &borderPath);

        Gdiplus::Font* pTitleFont = CreateFontSafe(g_pFontFamily, 11.5f, Gdiplus::FontStyleBold);
        Gdiplus::Font* pSubFont = CreateFontSafe(g_pFontFamily, 10.0f, Gdiplus::FontStyleBold);
        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 242, 245, 252));
        Gdiplus::SolidBrush mutedBr(Gdiplus::Color(255, 142, 154, 178));

        g.DrawString(L"AQUA WAVE", -1, pTitleFont, Gdiplus::PointF(18.0f, 15.0f), &whiteBr);

        bool recNow = g_isBuiltinRecording || g_isExternalRecording;
        Gdiplus::SolidBrush recBtnBg(recNow ? Gdiplus::Color(255, 65, 22, 28) : Gdiplus::Color(255, 30, 36, 54));
        DrawRoundedPill(g, &recBtnBg, kFlyW - 102.0f, 12.0f, 84.0f, 22.0f, 11.0f);
        Gdiplus::SolidBrush redDotBr(recNow ? Gdiplus::Color(255, 255, 0, 0) : Gdiplus::Color(255, 130, 140, 165));
        g.FillEllipse(&redDotBr, kFlyW - 93.0f, 19.0f, 8.0f, 8.0f);
        g.DrawString(L"Alt+Q REC", -1, pSubFont, Gdiplus::PointF(kFlyW - 80.0f, 16.0f), &whiteBr);

        int pw = (int)kPalW, ph = (int)kPalH;
        Gdiplus::Bitmap palBmp(pw, ph, PixelFormat32bppARGB);
        Gdiplus::BitmapData palData;
        Gdiplus::Rect palRc(0, 0, pw, ph);
        if (palBmp.LockBits(&palRc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &palData) == Gdiplus::Ok) {
            float previewV = (std::max)(0.28f, g_pickerV);
            for (int py = 0; py < ph; ++py) {
                uint8_t* row = (uint8_t*)palData.Scan0 + py * palData.Stride;
                float sat = 1.0f - (float)py / (float)(ph - 1);
                for (int px = 0; px < pw; ++px) {
                    float hue = ((float)px / (float)(pw - 1)) * 360.0f;
                    uint8_t r, gr, b;
                    HsvToRgb(hue, sat, previewV, r, gr, b);
                    row[px * 4 + 0] = b;
                    row[px * 4 + 1] = gr;
                    row[px * 4 + 2] = r;
                    row[px * 4 + 3] = 255;
                }
            }
            palBmp.UnlockBits(&palData);
        }

        Gdiplus::GraphicsPath palClip;
        BuildRoundedPath(palClip, kPalX, kPalY, kPalW, kPalH, 10.0f);
        g.SetClip(&palClip);
        g.DrawImage(&palBmp, Gdiplus::RectF(kPalX, kPalY, kPalW, kPalH));
        g.ResetClip();
        g.DrawPath(&borderPen, &palClip);

        float curPx = kPalX + (g_pickerH / 360.0f) * kPalW;
        float curPy = kPalY + (1.0f - g_pickerS) * kPalH;
        Gdiplus::Pen curOuter(Gdiplus::Color(255, 255, 255, 255), 2.0f);
        g.DrawEllipse(&curOuter, curPx - 6.0f, curPy - 6.0f, 12.0f, 12.0f);

        g.DrawString(L"\u042F\u0440\u043A\u043E\u0441\u0442\u044C", -1, pSubFont, Gdiplus::PointF(18.0f, 112.0f), &mutedBr);
        float valBarX = 88.0f, valBarY = 117.0f, valBarW = kFlyW - 106.0f, valBarH = 6.0f;
        Gdiplus::SolidBrush trackBg(Gdiplus::Color(255, 40, 48, 68));
        DrawRoundedPill(g, &trackBg, valBarX, valBarY, valBarW, valBarH, 3.0f);
        float valFill = valBarW * std::clamp(g_pickerV, 0.0f, 1.0f);
        if (valFill > 3.0f) DrawRoundedPill(g, &whiteBr, valBarX, valBarY, valFill, valBarH, 3.0f);
        g.FillEllipse(&whiteBr, valBarX + valFill - 5.5f, valBarY - 2.5f, 11.0f, 11.0f);

        g.DrawString(L"\u041F\u0440\u043E\u0437\u0440\u0430\u0447\u043D.", -1, pSubFont, Gdiplus::PointF(18.0f, 136.0f), &mutedBr);
        float alBarX = 88.0f, alBarY = 141.0f, alBarW = kFlyW - 106.0f, alBarH = 6.0f;
        DrawRoundedPill(g, &trackBg, alBarX, alBarY, alBarW, alBarH, 3.0f);
        float alRatio = std::clamp(((float)g_bgAlpha - 38.0f) / 217.0f, 0.0f, 1.0f);
        float alFill = alBarW * alRatio;
        if (alFill > 3.0f) DrawRoundedPill(g, &whiteBr, alBarX, alBarY, alFill, alBarH, 3.0f);
        g.FillEllipse(&whiteBr, alBarX + alFill - 5.5f, alBarY - 2.5f, 11.0f, 11.0f);

        float swY = 162.0f;
        for (int i = 0; i < kNumPresets; ++i) {
            float sx = 18.0f + i * 28.0f;
            Gdiplus::SolidBrush swBr(Gdiplus::Color(255, kPresets[i].r, kPresets[i].g, kPresets[i].b));
            g.FillEllipse(&swBr, sx, swY, 18.0f, 18.0f);
            bool sel = (g_bgR == kPresets[i].r && g_bgG == kPresets[i].g && g_bgB == kPresets[i].b);
            Gdiplus::Pen ringPen(sel ? Gdiplus::Color(255, 59, 130, 246) : Gdiplus::Color(85, 255, 255, 255), sel ? 2.0f : 1.0f);
            g.DrawEllipse(&ringPen, sx, swY, 18.0f, 18.0f);
        }

        Gdiplus::SolidBrush livePrevBr(Gdiplus::Color(g_bgAlpha, g_bgR, g_bgG, g_bgB));
        DrawRoundedPill(g, &livePrevBr, kFlyW - 60.0f, swY - 1.0f, 42.0f, 20.0f, 10.0f);
        Gdiplus::GraphicsPath prevBorder;
        BuildRoundedPath(prevBorder, kFlyW - 60.0f, swY - 1.0f, 42.0f, 20.0f, 10.0f);
        g.DrawPath(&borderPen, &prevBorder);

        Gdiplus::SolidBrush rowBg(Gdiplus::Color(255, 26, 32, 48));
        DrawRoundedPill(g, &rowBg, 16.0f, 190.0f, kFlyW - 32.0f, 30.0f, 10.0f);
        g.DrawString(L"\u0417\u0430\u043A\u0440\u0435\u043F\u0438\u0442\u044C", -1, pSubFont, Gdiplus::PointF(26.0f, 198.0f), &whiteBr);

        float swX = kFlyW - 54.0f, swTop = 197.0f, swW = 28.0f, swH = 16.0f;
        Gdiplus::SolidBrush toggleBg(g_isPositionLocked ? Gdiplus::Color(255, 59, 130, 246) : Gdiplus::Color(255, 52, 60, 82));
        DrawRoundedPill(g, &toggleBg, swX, swTop, swW, swH, 8.0f);
        float knobX = g_isPositionLocked ? (swX + swW - 13.0f) : (swX + 3.0f);
        g.FillEllipse(&whiteBr, knobX, swTop + 3.0f, 10.0f, 10.0f);

        Gdiplus::StringFormat centerFmt;
        centerFmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        centerFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        DrawRoundedPill(g, &rowBg, 16.0f, 226.0f, 110.0f, 26.0f, 9.0f);
        g.DrawString(L"\u0421\u0431\u0440\u043E\u0441", -1, pSubFont, Gdiplus::RectF(16.0f, 226.0f, 110.0f, 26.0f), &centerFmt, &mutedBr);

        Gdiplus::SolidBrush exitBg(Gdiplus::Color(255, 48, 26, 36));
        DrawRoundedPill(g, &exitBg, 134.0f, 226.0f, 114.0f, 26.0f, 9.0f);
        Gdiplus::SolidBrush exitTxt(Gdiplus::Color(255, 252, 130, 130));
        g.DrawString(L"\u0412\u044B\u0445\u043E\u0434", -1, pSubFont, Gdiplus::RectF(134.0f, 226.0f, 114.0f, 26.0f), &centerFmt, &exitTxt);

        delete pSubFont;
        delete pTitleFont;

        RECT rcWnd;
        GetWindowRect(hwnd, &rcWnd);
        POINT ptPos = { rcWnd.left, rcWnd.top };
        POINT ptSrc = { 0, 0 };
        SIZE sizeWnd = { kFlyW, kFlyH };

        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        GdiFlush();
        UpdateLayeredWindow(hwnd, hdcScreen, &ptPos, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    SelectObject(hdcMem, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

static LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);

        if (y >= 10 && y <= 36 && x >= kFlyW - 104 && x <= kFlyW - 14) {
            ToggleBuiltinRecording();
            RenderFlyout(hwnd);
            return 0;
        }

        if (x >= (int)kPalX && x <= (int)(kPalX + kPalW) && y >= (int)kPalY && y <= (int)(kPalY + kPalH)) {
            g_draggingHueSat = true;
            SetCapture(hwnd);
            g_pickerH = std::clamp(((float)x - kPalX) / kPalW, 0.0f, 1.0f) * 360.0f;
            g_pickerS = 1.0f - std::clamp(((float)y - kPalY) / kPalH, 0.0f, 1.0f);
            if (g_pickerV < 0.08f) g_pickerV = 0.24f;
            ApplyLiveHsvColor();
            return 0;
        }

        float BarX = 88.0f, BarW = kFlyW - 106.0f;
        if (y >= 109 && y <= 129 && x >= (int)(BarX - 8) && x <= (int)(BarX + BarW + 8)) {
            g_draggingVal = true;
            SetCapture(hwnd);
            g_pickerV = std::clamp(((float)x - BarX) / BarW, 0.02f, 1.0f);
            ApplyLiveHsvColor();
            return 0;
        }

        if (y >= 133 && y <= 153 && x >= (int)(BarX - 8) && x <= (int)(BarX + BarW + 8)) {
            g_draggingOpacity = true;
            SetCapture(hwnd);
            float t = std::clamp(((float)x - BarX) / BarW, 0.0f, 1.0f);
            g_bgAlpha = (uint8_t)(38.0f + t * 217.0f);
            if (g_hwndOverlay) RenderOverlay(g_hwndOverlay);
            RenderFlyout(hwnd);
            return 0;
        }

        if (y >= 158 && y <= 184) {
            for (int i = 0; i < kNumPresets; ++i) {
                float sx = 18.0f + i * 28.0f;
                float dx = x - (sx + 9.0f);
                float dy = y - (162.0f + 9.0f);
                if (dx * dx + dy * dy <= 120.0f) {
                    g_bgR = kPresets[i].r; g_bgG = kPresets[i].g; g_bgB = kPresets[i].b;
                    RgbToHsv(g_bgR, g_bgG, g_bgB, g_pickerH, g_pickerS, g_pickerV);
                    SaveConfig();
                    if (g_hwndOverlay) RenderOverlay(g_hwndOverlay);
                    RenderFlyout(hwnd);
                    return 0;
                }
            }
        }

        if (y >= 190 && y <= 220 && x >= 16 && x <= kFlyW - 16) {
            g_isPositionLocked = !g_isPositionLocked;
            SaveConfig();
            RenderFlyout(hwnd);
            return 0;
        }

        if (y >= 226 && y <= 254) {
            if (x >= 16 && x <= 126) {
                g_posX = GetSystemMetrics(SM_CXSCREEN) / 2;
                g_posY = 12;
                g_bgR = 32; g_bgG = 39; g_bgB = 60; g_bgAlpha = 216;
                RgbToHsv(g_bgR, g_bgG, g_bgB, g_pickerH, g_pickerS, g_pickerV);
                SaveConfig();
                if (g_hwndOverlay) RenderOverlay(g_hwndOverlay);
                RenderFlyout(hwnd);
            } else if (x >= 134 && x <= 248) {
                PostMessageW(g_hwndOverlay, WM_COMMAND, IDM_TRAY_EXIT, 0);
            }
            return 0;
        }

        if (y <= 36) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        float BarX = 88.0f, BarW = kFlyW - 106.0f;

        if (g_draggingHueSat) {
            g_pickerH = std::clamp(((float)x - kPalX) / kPalW, 0.0f, 1.0f) * 360.0f;
            g_pickerS = 1.0f - std::clamp(((float)y - kPalY) / kPalH, 0.0f, 1.0f);
            ApplyLiveHsvColor();
        } else if (g_draggingVal) {
            g_pickerV = std::clamp(((float)x - BarX) / BarW, 0.02f, 1.0f);
            ApplyLiveHsvColor();
        } else if (g_draggingOpacity) {
            float t = std::clamp(((float)x - BarX) / BarW, 0.0f, 1.0f);
            g_bgAlpha = (uint8_t)(38.0f + t * 217.0f);
            if (g_hwndOverlay) RenderOverlay(g_hwndOverlay);
            RenderFlyout(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_draggingHueSat || g_draggingVal || g_draggingOpacity) {
            g_draggingHueSat = g_draggingVal = g_draggingOpacity = false;
            ReleaseCapture();
            SaveConfig();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ToggleFlyoutWindow(HWND hwndOwner) {
    if (g_hwndFlyout && IsWindow(g_hwndFlyout)) {
        if (IsWindowVisible(g_hwndFlyout)) { ShowWindow(g_hwndFlyout, SW_HIDE); return; }
    } else {
        g_hwndFlyout = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            L"MediaOverlayFlyoutClass", L"Settings",
            WS_POPUP, 0, 0, kFlyW, kFlyH, hwndOwner, nullptr, GetModuleHandleW(nullptr), nullptr
        );
    }
    if (!g_hwndFlyout) return;

    POINT pt;
    GetCursorPos(&pt);
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = std::clamp((int)pt.x - kFlyW / 2, 8, screenW - kFlyW - 8);
    int y = (int)pt.y + 14;
    if (y + kFlyH > screenH - 12) y = (std::max)(8, (int)pt.y - kFlyH - 14);

    SetWindowPos(g_hwndFlyout, HWND_TOPMOST, x, y, kFlyW, kFlyH, SWP_SHOWWINDOW);
    RenderFlyout(g_hwndFlyout);
    SetForegroundWindow(g_hwndFlyout);
}

static HICON CreateTrayIcon() {
    Gdiplus::Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 240, 244, 255));
    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 59, 130, 246));
    DrawRoundedPill(g, &whiteBrush, 5.0f, 13.0f, 4.0f, 12.0f, 2.0f);
    DrawRoundedPill(g, &blueBrush, 14.0f, 6.0f, 4.0f, 21.0f, 2.0f);
    DrawRoundedPill(g, &whiteBrush, 23.0f, 10.0f, 4.0f, 15.0f, 2.0f);
    HICON hIcon = nullptr;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

static void InitTrayIcon(HWND hwnd) {
    g_hTrayIcon = CreateTrayIcon();
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAYMSG;
    g_nid.hIcon = g_hTrayIcon ? g_hTrayIcon : LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"AQUA WAVE (Alt+Q = Record with Audio)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hTrayIcon) { DestroyIcon(g_hTrayIcon); g_hTrayIcon = nullptr; }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwndOverlay = hwnd;
        LoadConfig();

        LoadDefaultLolPair();

        UpdateCompactWidthForText(L"AQUA WAVE");
        g_curCompactW = g_targetCompactW;

        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_lastFrameTime);

        HDC hdcScreen = GetDC(nullptr);
        g_hdcMem = CreateCompatibleDC(hdcScreen);
        g_hDefaultBmp = (HBITMAP)GetCurrentObject(g_hdcMem, OBJ_BITMAP);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kCanvasW;
        bmi.bmiHeader.biHeight = -kCanvasH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        g_hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &g_pBits, nullptr, 0);
        SelectObject(g_hdcMem, g_hBmp);
        ReleaseDC(nullptr, hdcScreen);

        SetWindowPos(hwnd, HWND_TOPMOST, g_posX - (int)kMainAnchorX, g_posY - kCanvasPadY, kCanvasW, kCanvasH, SWP_SHOWWINDOW);

        RegisterHotKey(hwnd, HOTKEY_TOGGLE_SHOW, MOD_CONTROL | MOD_SHIFT, 'O');
        RegisterHotKey(hwnd, HOTKEY_TOGGLE_CLICKTHROUGH, MOD_CONTROL | MOD_SHIFT, 'T');
        RegisterHotKey(hwnd, HOTKEY_TOGGLE_RECORD, MOD_ALT, 'Q');

        InitTrayIcon(hwnd);
        SetTimer(hwnd, TIMER_ANIM_ID, 15, nullptr);
        RenderOverlay(hwnd);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ScreenToClient(hwnd, &pt);
        float pillX, pillY, pillW, pillH;
        GetCurrentPillRect(pillX, pillY, pillW, pillH);

        bool inMain = (pt.x >= (int)pillX && pt.x <= (int)(pillX + pillW) &&
                       pt.y >= (int)pillY && pt.y <= (int)(pillY + pillH));

        bool inRecDot = false;
        if (g_recAnim > 0.1f && !g_isExpanded) {
            float recX = pillX + pillW + kRecIslandGap;
            inRecDot = (pt.x >= (int)recX && pt.x <= (int)(recX + kRecIslandSize) &&
                        pt.y >= (int)pillY && pt.y <= (int)(pillY + kCompactHeight));
        }

        return (inMain || inRecDot) ? HTCLIENT : HTTRANSPARENT;
    }

    case WM_APP_TRAYMSG:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP || lParam == WM_CONTEXTMENU) {
            ToggleFlyoutWindow(hwnd);
        }
        return 0;

    case WM_APP_MEDIA_CHANGED: {
        std::vector<uint8_t> newThumb;
        std::wstring newTitle;
        bool trackSwitched = (wParam == 1);
        {
            std::lock_guard<std::mutex> lock(g_mediaMutex);
            g_localMedia = g_sharedMedia;
            newThumb = g_localMedia.thumbBytes;
            newTitle = g_localMedia.title;
        }

        std::wstring display = (g_localMedia.hasSession && !newTitle.empty()) ? newTitle : L"AQUA WAVE";
        g_localMedia.displayText = display;
        UpdateCompactWidthForText(display);

        Gdiplus::Bitmap* rawBmp = (!newThumb.empty()) ? CreateBitmapWithWIC(newThumb.data(), newThumb.size()) : nullptr;
        BuildArtPairFromRaw(rawBmp, &g_pTrackCircleHi, &g_pTrackSquareHi);
        if (rawBmp) delete rawBmp;

        if (trackSwitched) {
            // Применяем анимацию появления только если карточка СВЕРНУТА
            if (!g_isExpanded) {
                g_trackFadeAnim = 0.20f; 
            }
            
            g_marqueeOffset = 0.0f;
            g_marqueePauseTimer = 1.5f;
            g_marqueeDirection = 1;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_ANIM_ID) {
            StepAnimationsAndRender(hwnd);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);

        float pillX, pillY, pillW, pillH;
        GetCurrentPillRect(pillX, pillY, pillW, pillH);

        if (g_recAnim > 0.5f && !g_isExpanded && g_isBuiltinRecording) {
            float recX = pillX + pillW + kRecIslandGap;
            if (x >= (int)recX && x <= (int)(recX + kRecIslandSize) &&
                y >= (int)pillY && y <= (int)(pillY + kCompactHeight)) {
                ToggleBuiltinRecording();
                if (g_hwndFlyout && IsWindowVisible(g_hwndFlyout)) RenderFlyout(g_hwndFlyout);
                return 0;
            }
        }

        if (g_isExpanded && g_shapeAnim > 0.7f) {
            float barX = pillX + 58.0f;
            float barW = pillW - 116.0f;
            float barY = pillY + 71.0f;

            if (y >= (int)(barY - 12.0f) && y <= (int)(barY + 16.0f) &&
                x >= (int)(barX - 10.0f) && x <= (int)(barX + barW + 10.0f)) {
                g_seekingTimeline = true;
                SetCapture(hwnd);
                float ratio = std::clamp((x - barX) / barW, 0.0f, 1.0f);
                double effDur = (g_localMedia.durationSec > 1.0) ? g_localMedia.durationSec : 180.0;
                g_localMedia.positionSec = ratio * effDur;
                RenderOverlay(hwnd);
                return 0;
            }

            float ctrlY = pillY + 100.0f;
            float centerX = pillX + pillW * 0.5f;
            if (y >= (int)(ctrlY - 18.0f) && y <= (int)(ctrlY + 18.0f)) {
                if (abs(x - (int)(centerX - 44.0f)) <= 20) {
                    g_prevBtnPulse = 1.0f;
                    SendMediaCommand(3);
                    return 0;
                } else if (abs(x - (int)centerX) <= 22) {
                    g_localMedia.isPlaying = !g_localMedia.isPlaying;
                    {
                        std::lock_guard<std::mutex> lock(g_mediaMutex);
                        g_sharedMedia.isPlaying = g_localMedia.isPlaying;
                        g_ignoreWorkerPlayUntil = GetTickCount64() + 750;
                    }
                    g_playBtnPulse = 1.0f;
                    SendMediaCommand(1);
                    return 0;
                } else if (abs(x - (int)(centerX + 44.0f)) <= 20) {
                    g_nextBtnPulse = 1.0f;
                    SendMediaCommand(2);
                    return 0;
                }
            }
        }

        POINT ptScreen;
        GetCursorPos(&ptScreen);
        g_dragScreenStart = ptScreen;
        g_dragClickOffset.x = ptScreen.x - g_posX;
        g_dragClickOffset.y = ptScreen.y - g_posY;
        g_isDragging = true;
        g_hasDraggedDistance = false;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_seekingTimeline) {
            int x = (short)LOWORD(lParam);
            float pillX, pillY, pillW, pillH;
            GetCurrentPillRect(pillX, pillY, pillW, pillH);
            float barX = pillX + 58.0f;
            float barW = pillW - 116.0f;
            float ratio = std::clamp((x - barX) / barW, 0.0f, 1.0f);
            double effDur = (g_localMedia.durationSec > 1.0) ? g_localMedia.durationSec : 180.0;
            g_localMedia.positionSec = ratio * effDur;
            RenderOverlay(hwnd);
            return 0;
        }

        if (g_isDragging && !g_isPositionLocked) {
            POINT ptScreen;
            GetCursorPos(&ptScreen);
            if (abs(ptScreen.x - g_dragScreenStart.x) > 4 || abs(ptScreen.y - g_dragScreenStart.y) > 4) {
                g_hasDraggedDistance = true;
            }
            if (g_hasDraggedDistance) {
                int screenW = GetSystemMetrics(SM_CXSCREEN);
                int screenH = GetSystemMetrics(SM_CYSCREEN);
                g_posX = std::clamp((int)(ptScreen.x - g_dragClickOffset.x), 140, screenW - 180);
                g_posY = std::clamp((int)(ptScreen.y - g_dragClickOffset.y), 0, screenH - 160);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_seekingTimeline) {
            g_seekingTimeline = false;
            ReleaseCapture();
            double seekTarget = g_localMedia.positionSec;
            uint64_t nowTick = GetTickCount64();
            {
                std::lock_guard<std::mutex> lock(g_mediaMutex);
                g_sharedMedia.positionSec = seekTarget;
                g_sharedMedia.lastQueryTick = nowTick;
                g_ignoreWorkerSeekUntil = nowTick + 1200;
                g_manualSeekBaseSec = seekTarget;
                g_manualSeekTick = nowTick;
            }
            SendMediaCommand(4, seekTarget);
            RenderOverlay(hwnd);
            return 0;
        }

        if (g_isDragging) {
            g_isDragging = false;
            ReleaseCapture();
            if (!g_hasDraggedDistance) {
                g_isExpanded = !g_isExpanded;
            } else {
                SaveConfig();
            }
        }
        return 0;
    }

    case WM_RBUTTONUP:
        ToggleFlyoutWindow(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDM_TRAY_EXIT) DestroyWindow(hwnd);
        return 0;

    case WM_HOTKEY:
        if (wParam == HOTKEY_TOGGLE_SHOW) {
            g_isVisible = !g_isVisible;
            ShowWindow(hwnd, g_isVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
        } else if (wParam == HOTKEY_TOGGLE_CLICKTHROUGH) {
            g_isClickThrough = !g_isClickThrough;
            LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_isClickThrough ? (exStyle | WS_EX_TRANSPARENT) : (exStyle & ~WS_EX_TRANSPARENT));
        } else if (wParam == HOTKEY_TOGGLE_RECORD) {
            ToggleBuiltinRecording();
            if (g_hwndFlyout && IsWindowVisible(g_hwndFlyout)) RenderFlyout(g_hwndFlyout);
        }
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_hwndFlyout && IsWindow(g_hwndFlyout)) DestroyWindow(g_hwndFlyout);
        KillTimer(hwnd, TIMER_ANIM_ID);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_SHOW);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_CLICKTHROUGH);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_RECORD);
        g_running = false;
        g_isBuiltinRecording = false;
        if (g_recordThread.joinable()) g_recordThread.join();
        if (g_pTrackCircleHi) { delete g_pTrackCircleHi; g_pTrackCircleHi = nullptr; }
        if (g_pTrackSquareHi) { delete g_pTrackSquareHi; g_pTrackSquareHi = nullptr; }
        if (g_pDefCircleHi) { delete g_pDefCircleHi; g_pDefCircleHi = nullptr; }
        if (g_pDefSquareHi) { delete g_pDefSquareHi; g_pDefSquareHi = nullptr; }
        if (g_hBmp) {
            if (g_hDefaultBmp) SelectObject(g_hdcMem, g_hDefaultBmp);
            DeleteObject(g_hBmp);
        }
        if (g_hdcMem) DeleteDC(g_hdcMem);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_pWicFactory));

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    InitCustomFonts();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
    wc.lpszClassName = L"MediaPillOverlayClass";
    RegisterClassExW(&wc);

    WNDCLASSEXW wcFlyout = {};
    wcFlyout.cbSize = sizeof(WNDCLASSEXW);
    wcFlyout.style = CS_HREDRAW | CS_VREDRAW;
    wcFlyout.lpfnWndProc = FlyoutWndProc;
    wcFlyout.hInstance = hInstance;
    wcFlyout.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
    wcFlyout.lpszClassName = L"MediaOverlayFlyoutClass";
    RegisterClassExW(&wcFlyout);

       HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"MediaPillOverlay", WS_POPUP,
        0, 0, kCanvasW, kCanvasH, nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        timeEndPeriod(1);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    std::thread workerThread(MediaWorker, hwnd);

    MSG msg = {};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        StepAnimationsAndRender(hwnd);

        if (FAILED(DwmFlush())) {
            Sleep(4);
        }
    }

    g_running = false;
    g_isBuiltinRecording = false;
    if (g_recordThread.joinable()) g_recordThread.join();
    if (workerThread.joinable()) workerThread.join();

    delete g_pFontExpTime;
    delete g_pFontExpArtist;
    delete g_pFontExpTitle;
    delete g_pFontCompact;
    delete g_pFontFamily;
    if (!g_loadedCustomFontPath.empty()) {
        RemoveFontResourceExW(g_loadedCustomFontPath.c_str(), FR_PRIVATE, 0);
    }
    delete g_pPrivateFonts;

    Gdiplus::GdiplusShutdown(gdiplusToken);
    if (g_pWicFactory) g_pWicFactory->Release();
    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}