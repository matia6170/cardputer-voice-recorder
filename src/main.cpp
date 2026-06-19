#include <Arduino.h>

#include <SD.h>
#include <SPI.h>

#include <algorithm>
#include <vector>

#include "M5Cardputer.h"

// ---------- config ----------
static constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;
static constexpr uint32_t SAMPLE_RATE = 16000;          // 16 kHz mono 16-bit PCM
static constexpr size_t CHUNK_SAMPLES = 1024;           // 64 ms per chunk
static constexpr char REC_DIR[] = "/recordings";

static M5Canvas canvas(&M5Cardputer.Display);

// ---------- app state ----------
enum class Screen { Menu, Browser, Recorder, Player };
enum class RecState { Idle, Recording, Paused };

static Screen screen = Screen::Menu;

static const char* menuItems[] = {"File Browser", "Voice Recorder"};
static constexpr int MENU_COUNT = 2;
static int menuSel = 0;

struct FileEntry {
    String name;
    uint32_t size;
};
static std::vector<FileEntry> files;
static int browserSel = 0;
static int browserTop = 0;
static bool sdOk = false;

static RecState recState = RecState::Idle;
static File wavFile;
static String recName;
static uint32_t samplesWritten = 0;
static int16_t audioBuf[2][CHUNK_SAMPLES];  // shared: mic capture & playback
static uint8_t bufIdx = 0;
static uint32_t chunksSinceFlush = 0;

// player state
static File playFile;
static String playName;
static bool playing = false;
static uint32_t wavRate = 0, wavByteRate = 0;
static bool wavStereo = false;
static uint32_t dataStart = 0, dataSize = 0, bytePos = 0;
static int volume = 192;

static String statusMsg;
static uint32_t statusMsgUntil = 0;

// ---------- helpers ----------
static void showMsg(const String& msg, uint32_t ms = 3000) {
    statusMsg = msg;
    statusMsgUntil = millis() + ms;
}

static bool mountSD() {
    SD.end();
    sdOk = SD.begin(SD_CS, SPI, 25000000);
    return sdOk;
}

static void fmtTime(char* out, size_t n, uint32_t secs) {
    snprintf(out, n, "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

// ---------- keyboard ----------
struct Keys {
    bool enter = false, esc = false, up = false, down = false, space = false;
    bool left = false, right = false;
};

static Keys readKeys() {
    Keys k;
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto st = M5Cardputer.Keyboard.keysState();
        k.enter = st.enter;
        for (char c : st.word) {
            if (st.fn && c == '`') k.esc = true;
            if (c == ';') k.up = true;    // fn+; is up arrow; bare ; also accepted
            if (c == '.') k.down = true;  // fn+. is down arrow; bare . also accepted
            if (c == ',') k.left = true;  // fn+, is left arrow
            if (c == '/') k.right = true; // fn+/ is right arrow
            if (!st.fn && c == ' ') k.space = true;
        }
    }
    return k;
}

// ---------- WAV ----------
static void writeWavHeader(File& f, uint32_t dataBytes) {
    const uint16_t channels = 1, bits = 16, pcm = 1;
    const uint32_t byteRate = SAMPLE_RATE * channels * bits / 8;
    const uint16_t blockAlign = channels * bits / 8;
    const uint32_t riffSize = 36 + dataBytes;
    const uint32_t fmtLen = 16;
    f.write((const uint8_t*)"RIFF", 4);
    f.write((const uint8_t*)&riffSize, 4);
    f.write((const uint8_t*)"WAVE", 4);
    f.write((const uint8_t*)"fmt ", 4);
    f.write((const uint8_t*)&fmtLen, 4);
    f.write((const uint8_t*)&pcm, 2);
    f.write((const uint8_t*)&channels, 2);
    f.write((const uint8_t*)&SAMPLE_RATE, 4);
    f.write((const uint8_t*)&byteRate, 4);
    f.write((const uint8_t*)&blockAlign, 2);
    f.write((const uint8_t*)&bits, 2);
    f.write((const uint8_t*)"data", 4);
    f.write((const uint8_t*)&dataBytes, 4);
}

static String baseName(const String& path) {
    int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

static String nextRecName() {
    int maxN = 0;
    File dir = SD.open(REC_DIR);
    if (dir) {
        while (File f = dir.openNextFile()) {
            String n = baseName(f.name());
            f.close();
            if (n.startsWith("REC_")) {
                int v = n.substring(4).toInt();
                if (v > maxN) maxN = v;
            }
        }
        dir.close();
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "REC_%04d.wav", maxN + 1);
    return String(buf);
}

// ---------- recording ----------
static void enqueueBoth() {
    M5Cardputer.Mic.record(audioBuf[0], CHUNK_SAMPLES, SAMPLE_RATE);
    M5Cardputer.Mic.record(audioBuf[1], CHUNK_SAMPLES, SAMPLE_RATE);
    bufIdx = 0;
}

static void writeChunk() {
    wavFile.write((const uint8_t*)audioBuf[bufIdx], CHUNK_SAMPLES * sizeof(int16_t));
    samplesWritten += CHUNK_SAMPLES;
    if (++chunksSinceFlush >= 16) {  // ~1 s of audio
        wavFile.flush();
        chunksSinceFlush = 0;
    }
}

// While recording: when the mic queue drops below 2, the oldest buffer is
// full — write it to SD and hand it back to the mic.
static void pumpAudio() {
    int guard = 4;
    while (recState == RecState::Recording && M5Cardputer.Mic.isRecording() < 2 && guard--) {
        writeChunk();
        M5Cardputer.Mic.record(audioBuf[bufIdx], CHUNK_SAMPLES, SAMPLE_RATE);
        bufIdx ^= 1;
    }
}

// Finish the two in-flight buffers and write them, without re-enqueueing.
static void drainAudio() {
    for (int remaining = 1; remaining >= 0; --remaining) {
        uint32_t deadline = millis() + 500;
        while ((int)M5Cardputer.Mic.isRecording() > remaining && millis() < deadline) {
            M5.delay(1);
        }
        writeChunk();
        bufIdx ^= 1;
    }
}

static void startRecording() {
    if (!mountSD()) {
        showMsg("No SD card!");
        return;
    }
    if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
    recName = nextRecName();
    wavFile = SD.open(String(REC_DIR) + "/" + recName, FILE_WRITE);
    if (!wavFile) {
        showMsg("Can't create file!");
        return;
    }
    writeWavHeader(wavFile, 0);
    samplesWritten = 0;
    chunksSinceFlush = 0;
    enqueueBoth();
    recState = RecState::Recording;
}

static void pauseRecording() {
    drainAudio();
    wavFile.flush();
    recState = RecState::Paused;
}

static void resumeRecording() {
    enqueueBoth();
    recState = RecState::Recording;
}

static void stopRecording() {
    if (recState == RecState::Recording) drainAudio();
    const uint32_t dataBytes = samplesWritten * sizeof(int16_t);
    wavFile.seek(0);
    writeWavHeader(wavFile, dataBytes);
    wavFile.close();
    char t[8];
    fmtTime(t, sizeof(t), samplesWritten / SAMPLE_RATE);
    showMsg("Saved " + recName + " (" + t + ")");
    samplesWritten = 0;
    recState = RecState::Idle;
}

// ---------- playback ----------
static bool parseWavHeader(File& f) {
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint16_t fmt = 0, channels = 0, bits = 0;
    wavRate = 0;
    dataStart = dataSize = 0;
    while (true) {
        uint8_t ck[8];
        if (f.read(ck, 8) != 8) break;
        uint32_t len;
        memcpy(&len, ck + 4, 4);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fb[16];
            if (len < 16 || f.read(fb, 16) != 16) return false;
            memcpy(&fmt, fb, 2);
            memcpy(&channels, fb + 2, 2);
            memcpy(&wavRate, fb + 4, 4);
            memcpy(&bits, fb + 14, 2);
            if (len > 16) f.seek(f.position() + (len - 16) + (len & 1));
        } else if (memcmp(ck, "data", 4) == 0) {
            dataStart = f.position();
            dataSize = len;
            break;
        } else {
            f.seek(f.position() + len + (len & 1));
        }
    }
    if (fmt != 1 || bits != 16 || (channels != 1 && channels != 2) || !wavRate || !dataSize) {
        return false;
    }
    wavStereo = (channels == 2);
    wavByteRate = wavRate * channels * 2;
    dataSize = min(dataSize, (uint32_t)(f.size() - dataStart));
    f.seek(dataStart);
    return true;
}

static void openPlayer(const String& name) {
    playFile = SD.open(String(REC_DIR) + "/" + name, FILE_READ);
    if (!playFile) {
        showMsg("Can't open " + name);
        return;
    }
    if (!parseWavHeader(playFile)) {
        playFile.close();
        showMsg("Not a 16-bit PCM WAV");
        return;
    }
    playName = name;
    bytePos = 0;
    bufIdx = 0;
    M5Cardputer.Mic.end();  // mic and speaker can't run at the same time
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(volume);
    playing = true;  // autoplay
    screen = Screen::Player;
}

static void closePlayer() {
    playing = false;
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();
    playFile.close();
    M5Cardputer.Mic.begin();
    screen = Screen::Browser;
}

// Same ping-pong pattern as recording: when the speaker queue has room,
// read the next chunk from the file and hand it over.
static void pumpPlayback() {
    if (screen != Screen::Player || !playing) return;
    int guard = 4;
    while (playing && M5Cardputer.Speaker.isPlaying(0) < 2 && guard--) {
        if (bytePos >= dataSize) {
            if (M5Cardputer.Speaker.isPlaying(0) == 0) playing = false;  // finished
            break;
        }
        const size_t want = min((uint32_t)sizeof(audioBuf[0]), dataSize - bytePos);
        const int n = playFile.read((uint8_t*)audioBuf[bufIdx], want);
        if (n <= 0) {
            playing = false;
            break;
        }
        M5Cardputer.Speaker.playRaw(audioBuf[bufIdx], n / 2, wavRate, wavStereo, 1, 0);
        bytePos += n;
        bufIdx ^= 1;
    }
}

static void togglePlay() {
    if (playing) {
        playing = false;  // queued audio (<=128 ms) drains out
    } else {
        if (bytePos >= dataSize) {  // finished -> replay from start
            bytePos = 0;
            playFile.seek(dataStart);
        }
        playing = true;
    }
}

static void seekBy(int32_t secs) {
    const uint32_t blockAlign = wavStereo ? 4 : 2;
    int64_t np = (int64_t)bytePos + (int64_t)secs * wavByteRate;
    if (np < 0) np = 0;
    if (np > (int64_t)dataSize) np = dataSize;
    bytePos = ((uint32_t)np / blockAlign) * blockAlign;
    M5Cardputer.Speaker.stop(0);  // flush queue so the jump is immediate
    playFile.seek(dataStart + bytePos);
}

// ---------- file browser ----------
static void loadFiles() {
    files.clear();
    browserSel = browserTop = 0;
    if (!mountSD()) return;
    File dir = SD.open(REC_DIR);
    if (!dir) return;
    while (File f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            files.push_back({baseName(f.name()), (uint32_t)f.size()});
        }
        f.close();
    }
    dir.close();
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
}

// ---------- input handling ----------
static void handleMenu(const Keys& k) {
    if (k.up) menuSel = (menuSel + MENU_COUNT - 1) % MENU_COUNT;
    if (k.down) menuSel = (menuSel + 1) % MENU_COUNT;
    if (k.enter) {
        if (menuSel == 0) {
            loadFiles();
            screen = Screen::Browser;
        } else {
            screen = Screen::Recorder;
        }
    }
}

static constexpr int BROWSER_ROWS = 9;

static void handleBrowser(const Keys& k) {
    if (k.esc) {
        screen = Screen::Menu;
        return;
    }
    if (files.empty()) return;
    if (k.up && browserSel > 0) browserSel--;
    if (k.down && browserSel < (int)files.size() - 1) browserSel++;
    if (browserSel < browserTop) browserTop = browserSel;
    if (browserSel >= browserTop + BROWSER_ROWS) browserTop = browserSel - BROWSER_ROWS + 1;
    if (k.enter) openPlayer(files[browserSel].name);
}

static void handlePlayer(const Keys& k) {
    if (k.esc) {
        closePlayer();
        return;
    }
    if (k.enter || k.space) togglePlay();
    if (k.left) seekBy(-5);
    if (k.right) seekBy(5);
    if (k.up) {
        volume = min(255, volume + 16);
        M5Cardputer.Speaker.setVolume(volume);
    }
    if (k.down) {
        volume = max(0, volume - 16);
        M5Cardputer.Speaker.setVolume(volume);
    }
}

static void handleRecorder(const Keys& k) {
    switch (recState) {
        case RecState::Idle:
            if (k.esc) screen = Screen::Menu;
            else if (k.enter) startRecording();
            break;
        case RecState::Recording:
            if (k.enter) stopRecording();
            else if (k.space) pauseRecording();
            break;
        case RecState::Paused:
            if (k.enter) stopRecording();
            else if (k.space) resumeRecording();
            break;
    }
}

// ---------- drawing ----------
static void drawStatusMsg() {
    if (statusMsg.length() && millis() < statusMsgUntil) {
        canvas.setFont(&fonts::Font0);
        canvas.setTextSize(1);
        canvas.setTextDatum(bottom_center);
        canvas.setTextColor(TFT_GREENYELLOW);
        canvas.drawString(statusMsg, 120, 116);
    }
}

static void drawMenu() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString("Cardputer Recorder", 120, 8);

    canvas.setFont(&fonts::Font0);
    for (int i = 0; i < MENU_COUNT; ++i) {
        const int y = 48 + i * 26;
        if (i == menuSel) {
            canvas.fillRoundRect(40, y - 6, 160, 20, 4, TFT_NAVY);
            canvas.drawRoundRect(40, y - 6, 160, 20, 4, TFT_CYAN);
            canvas.setTextColor(TFT_WHITE);
        } else {
            canvas.setTextColor(TFT_LIGHTGREY);
        }
        canvas.setTextDatum(top_center);
        canvas.drawString(menuItems[i], 120, y);
    }

    drawStatusMsg();
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("fn+; up  fn+. down  Enter: select", 120, 130);
    canvas.pushSprite(0, 0);
}

static void drawBrowser() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString("/recordings", 4, 4);
    canvas.setTextDatum(top_right);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString(String(files.size()) + " file" + (files.size() == 1 ? "" : "s"), 236, 4);
    canvas.drawFastHLine(0, 15, 240, TFT_DARKGREY);

    if (!sdOk) {
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(TFT_RED);
        canvas.drawString("No SD card", 120, 67);
    } else if (files.empty()) {
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(TFT_DARKGREY);
        canvas.drawString("No recordings yet", 120, 67);
    } else {
        for (int row = 0; row < BROWSER_ROWS; ++row) {
            const int i = browserTop + row;
            if (i >= (int)files.size()) break;
            const int y = 20 + row * 11;
            if (i == browserSel) {
                canvas.fillRect(0, y - 1, 240, 11, TFT_NAVY);
                canvas.setTextColor(TFT_WHITE);
            } else {
                canvas.setTextColor(TFT_LIGHTGREY);
            }
            canvas.setTextDatum(top_left);
            canvas.drawString(files[i].name, 6, y);
            // duration for our own wav format, plus size
            char info[24];
            if (files[i].size > 44) {
                char t[8];
                fmtTime(t, sizeof(t), (files[i].size - 44) / (SAMPLE_RATE * 2));
                snprintf(info, sizeof(info), "%s %4uKB", t, (unsigned)(files[i].size / 1024));
            } else {
                snprintf(info, sizeof(info), "%uB", (unsigned)files[i].size);
            }
            canvas.setTextDatum(top_right);
            canvas.drawString(info, 234, y);
        }
    }

    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("fn+;/. move  Enter: play  fn+`: back", 120, 133);
    canvas.pushSprite(0, 0);
}

static void drawRecorder() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString(recState == RecState::Idle ? "Voice Recorder" : recName, 120, 5);

    // indicator
    const int cx = 55, cy = 62;
    if (recState == RecState::Recording) {
        if ((millis() / 500) % 2) {
            canvas.fillCircle(cx, cy, 10, TFT_RED);
        } else {
            canvas.drawCircle(cx, cy, 10, TFT_RED);
        }
    } else if (recState == RecState::Paused) {
        canvas.fillRect(cx - 7, cy - 9, 5, 18, TFT_YELLOW);
        canvas.fillRect(cx + 2, cy - 9, 5, 18, TFT_YELLOW);
    } else {
        canvas.drawCircle(cx, cy, 10, TFT_DARKGREY);
    }

    // stopwatch
    char t[8];
    fmtTime(t, sizeof(t), samplesWritten / SAMPLE_RATE);
    canvas.setTextSize(3);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(recState == RecState::Recording ? TFT_WHITE
                        : recState == RecState::Paused  ? TFT_YELLOW
                                                        : TFT_DARKGREY);
    canvas.drawString(t, 140, cy);

    // status label
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    if (recState == RecState::Recording) {
        canvas.setTextColor(TFT_RED);
        canvas.drawString("REC", cx, cy + 16);
    } else if (recState == RecState::Paused) {
        canvas.setTextColor(TFT_YELLOW);
        canvas.drawString("PAUSED", cx, cy + 16);
    }

    drawStatusMsg();
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(TFT_DARKGREY);
    if (recState == RecState::Idle) {
        canvas.drawString("Enter: record  fn+`: back", 120, 133);
    } else {
        canvas.drawString("Enter: stop & save  Space: pause", 120, 133);
    }
    canvas.pushSprite(0, 0);
}

static void drawPlayer() {
    canvas.fillSprite(TFT_BLACK);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(TFT_CYAN);
    canvas.drawString(playName, 120, 5);

    // play / pause icon
    const int cx = 35, cy = 48;
    if (playing) {
        canvas.fillTriangle(cx - 6, cy - 9, cx - 6, cy + 9, cx + 9, cy, TFT_GREEN);
    } else {
        canvas.fillRect(cx - 7, cy - 9, 5, 18, TFT_YELLOW);
        canvas.fillRect(cx + 2, cy - 9, 5, 18, TFT_YELLOW);
    }

    // elapsed / total time
    char cur[8], tot[8];
    fmtTime(cur, sizeof(cur), wavByteRate ? bytePos / wavByteRate : 0);
    fmtTime(tot, sizeof(tot), wavByteRate ? dataSize / wavByteRate : 0);
    canvas.setTextSize(2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString(String(cur) + " / " + tot, 140, cy);

    // progress bar
    canvas.drawRoundRect(20, 68, 200, 10, 3, TFT_DARKGREY);
    const int w = dataSize ? (int)((uint64_t)bytePos * 196 / dataSize) : 0;
    if (w > 0) canvas.fillRoundRect(22, 70, w, 6, 2, TFT_CYAN);

    // volume bar
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("Vol", 20, 88);
    canvas.drawRect(45, 88, 102, 7, TFT_DARKGREY);
    canvas.fillRect(46, 89, volume * 100 / 255, 5, TFT_GREEN);

    drawStatusMsg();
    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("Space: play/pause  fn+, fn+/: seek 5s", 120, 124);
    canvas.drawString("fn+; fn+.: volume  fn+`: back", 120, 133);
    canvas.pushSprite(0, 0);
}

// ---------- arduino ----------
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // enable keyboard

    M5Cardputer.Display.setRotation(1);
    canvas.setColorDepth(8);
    canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    mountSD();

    // mic and speaker can't run at the same time
    M5Cardputer.Speaker.end();
    M5Cardputer.Mic.begin();
}

void loop() {
    M5Cardputer.update();

    const Keys k = readKeys();
    switch (screen) {
        case Screen::Menu:     handleMenu(k); break;
        case Screen::Browser:  handleBrowser(k); break;
        case Screen::Recorder: handleRecorder(k); break;
        case Screen::Player:   handlePlayer(k); break;
    }

    pumpAudio();
    pumpPlayback();

    static uint32_t lastDraw = 0;
    if (millis() - lastDraw >= 50) {
        lastDraw = millis();
        switch (screen) {
            case Screen::Menu:     drawMenu(); break;
            case Screen::Browser:  drawBrowser(); break;
            case Screen::Recorder: drawRecorder(); break;
            case Screen::Player:   drawPlayer(); break;
        }
    }
}
