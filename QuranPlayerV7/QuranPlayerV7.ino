/*
 * Quran Player V7 — buffered multi-task player + voice "Deen AI" (ESP32-C3)
 *
 * ARDUINO IDE SETTINGS
 *   Board package     : esp32 by Espressif, 3.3.x
 *   Partition Scheme  : Huge APP (3MB No OTA/1MB SPIFFS)
 *   USB CDC On Boot   : Enabled   (REQUIRED to see Serial logs; GPIO20/21 are
 *                                  used by I2S, so UART Serial cannot be used)
 * LIBRARIES
 *   U8g2
 *   arduino-libhelix (pschatzmann)
 *   (arduino-audio-tools is no longer needed)
 *
 * BUTTONS (GPIO 4,3,2,1,0 = KEY1..KEY5)
 *   KEY1 = Up / Previous / "-"
 *   KEY2 = Down / Next / "+"
 *   KEY3 = OK / Select / Play-Pause
 *   KEY4 = Back   (stops audio)
 *   KEY5 = AI     (opens the AI page; on that page HOLD to speak, release to send)
 *
 * MICROPHONE (INMP441, I2S) — shares the two clock wires with the amplifier
 *   VDD -> 3V3      GND -> GND      L/R -> GND
 *   SCK -> GPIO7    WS  -> GPIO20   SD  -> GPIO10
 *
 * DEEN AI
 *   Voice -> your Flask backend -> OpenAI -> answer on screen and spoken.
 *   AI rules, models and voice are set in the admin dashboard.
 *   AI answers can be wrong: check rulings with a qualified scholar.
 *   No AI key is stored here, only this device's Product ID and token.
 *
 * WI-FI SETUP (from any phone)
 *   First start, or saved Wi-Fi not found: join the Wi-Fi "Quran Player" on your
 *   phone; the setup page opens (or visit 192.168.4.1). To change Wi-Fi later,
 *   hold KEY4 (Back) while switching the device on.
 *
 * RULES
 *   Boot     -> play Bismillah (1:1) once, then Home (Back/Stop skips)
 *   Quran    -> continuous surah playback (ayah-by-ayah), Stop keeps place
 *   Ustaz    -> play only the selected ayah; when it ends, STOP
 *   Adhan    -> today's prayer times (playback comes in Phase 2)
 *   Home     -> total silence; KEY5 opens AI from every screen
 *   AI page  -> player stops (your place is kept), speaker silent, mic on
 *
 * HOW IT STAYS SMOOTH (3 jobs that never block each other)
 *   audio task (highest) : MP3 buffer -> decoder -> I2S, with soft fades
 *   loop()     (middle)  : buttons, screen, LED
 *   net task   (lowest)  : keeps one HTTPS connection open and fills a 32 KB buffer
 *   Buttons are sampled by a 5 ms timer, so a press is never lost.
 */

 #include <WiFi.h>
 #include <WebServer.h>
 #include <DNSServer.h>
 #include <Preferences.h>
 #include <WiFiClientSecure.h>
 #include <esp_system.h>
 #include <esp_timer.h>
 #include <driver/i2s_std.h>
 #include <driver/gpio.h>
 #include <freertos/FreeRTOS.h>
 #include <freertos/task.h>
 #include <freertos/queue.h>
 #include <freertos/semphr.h>
 #include <U8g2lib.h>
 #include <MP3DecoderHelix.h>          // pulls in arduino-libhelix
 #include "libhelix-mp3/mp3dec.h"
 #include <math.h>
 #include <time.h>
 #include "huruf_bitmaps.h"
 
 // Wi-Fi is NOT written in the code: the owner enters it from a phone.
 #define SETUP_AP_NAME  "Quran Player"
 
 // ---- your backend (copy ID and token from the dashboard) ----
 #define BACKEND_HOST "ruju-zxb0.onrender.com"   // no https://, no slash
 #define PRODUCT_ID   "QP-D179E0F4"
 #define DEVICE_TOKEN "PwmtsYePeRjc37_lFsyEp-7MsnhPqPVfIwcxut4MHWM"
 #define FW_VERSION   "7.1"
 // Default content site (dashboard can override per device). No trailing slash.
 #define CONTENT_BASE_URL "https://buxinelectronics-art.github.io/Ruju"
 #define CONTENT_SURAH 255          // Clip.surah for GitHub Pages / content MP3s
 #define ADHAN_WINDOW_MIN 2         // fire only within this many minutes of the time
 #define ADHAN_AI_WAIT_MS 120000UL  // max wait while AI is busy
 
 #define I2S_BCLK 7
 #define I2S_LRC  20
 #define I2S_DOUT 21
 #define MIC_DIN  10            // INMP441 SD
 #define MIC_SHIFT 13           // mic loudness: 12 = louder, 14 = quieter
 #define LED_PIN  9             // status LED: solid ON idle, slow breathe while playing
 #define LED_PWM_FREQ 5000
 #define LED_PWM_RES  8         // 0..255
 
 #if ARDUINO_USB_CDC_ON_BOOT
   #define LOGF(...) Serial.printf(__VA_ARGS__)
 #else
   #define LOGF(...) do {} while (0)
 #endif
 
 const int BTN[5] = {4, 3, 2, 1, 0};
 // Do not use B1..B5 — conflict with Arduino binary.h
 enum { KEY1 = 0, KEY2 = 1, KEY3 = 2, KEY4 = 3, KEY5 = 4 };
 
 U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
 
 // ---------------- tuning ----------------
 static const size_t   RING_SIZE      = 32768;  // compressed audio buffer (~4 s at 64 kbps)
 static const size_t   PREBUFFER      = 12288;  // fill this much before (re)starting sound
 static const size_t   LOW_WATER      = 1600;   // fade out before the buffer runs dry
 static const size_t   IN_SIZE        = 4096;   // decoder input window
 static const int      CLIP_SLOTS     = 6;      // ayahs that can be queued in the buffer
 static const uint32_t FADE_MS        = 12;     // click-free start / stop / pause
 static const uint32_t GAP_AYAH_MS    = 300;
 static const uint32_t GAP_SURAH_MS   = 500;
 static const uint32_t STEP_DELAY_MS  = 400;    // wait for more Next/Prev presses before loading
 static const uint32_t STALL_MS       = 8000;   // no data this long -> reconnect and resume
 static const int      MAX_RETRIES    = 6;      // failures in a row before "Network error"
 static const uint32_t WIFI_WAIT_MS   = 15000;
 static const uint32_t BOOT_WIFI_MS   = 25000;
 
 static const uint32_t MIC_RATE       = 16000;
 static const uint32_t MIC_WARMUP     = MIC_RATE * 3 / 10;   // INMP441 needs ~0.26 s to wake
 static const uint32_t REC_MAX        = MIC_RATE * 10;       // longest question: 10 s
 static const uint32_t REC_MIN        = MIC_RATE * 4 / 10;
 static const uint32_t REC_TAIL       = MIC_RATE / 5;        // keep 0.2 s after release
 static const int      REC_SILENT     = 300;                 // peak below this = no voice
 static const size_t   REC_SEG        = 4096;
 static const int      REC_SEGS       = 16;                  // extra RAM blocks beyond ringBuf
 static const uint32_t REC_HEAP_KEEP  = 60000;               // leave this much for HTTPS
 static const size_t   FLAC_BLOCK     = 1024;
 static const uint32_t AI_REPLY_MS    = 60000;              // a sleeping server is slow
 static const uint32_t HELLO_MS       = 300000;             // check in every 5 minutes
 
 // =====================================================================
 //                              QURAN DATA
 // =====================================================================
 struct Reciter {
   const char* name;       // short OLED label
   const char* ayahFolder; // everyayah.com folder
 };
 
 const Reciter RECITERS[] = {
   {"Alafasy",  "Alafasy_64kbps"},
   {"Sudais",   "Abdurrahmaan_As-Sudais_64kbps"},
   {"Husary",   "Husary_64kbps"},
   {"Hudhaify", "Hudhaify_64kbps"},
 };
 const int RECITER_COUNT = sizeof(RECITERS) / sizeof(RECITERS[0]);
 
 // Short names only (flash space)
 const char* const NAMES[114] = {
   "Fatihah","Baqarah","Imran","Nisa","Maidah","Anam","Araf","Anfal","Tawbah","Yunus",
   "Hud","Yusuf","Rad","Ibrahim","Hijr","Nahl","Isra","Kahf","Maryam","Taha",
   "Anbiya","Hajj","Muminun","Nur","Furqan","Shuara","Naml","Qasas","Ankabut","Rum",
   "Luqman","Sajdah","Ahzab","Saba","Fatir","Yasin","Saffat","Sad","Zumar","Ghafir",
   "Fussilat","Shura","Zukhruf","Dukhan","Jathiyah","Ahqaf","Muhammad","Fath","Hujurat","Qaf",
   "Dhariyat","Tur","Najm","Qamar","Rahman","Waqiah","Hadid","Mujadila","Hashr","Mumtahanah",
   "Saff","Jumuah","Munafiqun","Taghabun","Talaq","Tahrim","Mulk","Qalam","Haqqah","Maarij",
   "Nuh","Jinn","Muzzammil","Muddathir","Qiyamah","Insan","Mursalat","Naba","Naziat","Abasa",
   "Takwir","Infitar","Mutaffifin","Inshiqaq","Buruj","Tariq","Ala","Ghashiyah","Fajr","Balad",
   "Shams","Layl","Duha","Sharh","Tin","Alaq","Qadr","Bayyinah","Zalzalah","Adiyat",
   "Qariah","Takathur","Asr","Humazah","Fil","Quraysh","Maun","Kawthar","Kafirun","Nasr",
   "Masad","Ikhlas","Falaq","Nas"
 };
 
 const uint16_t AYAHS[114] = {
   7,286,200,176,120,165,206,75,129,109,
   123,111,43,52,99,128,111,110,98,135,
   112,78,118,64,77,227,93,88,69,60,
   34,30,73,54,45,83,182,88,75,85,
   54,53,89,59,37,35,38,29,18,45,
   60,49,62,55,78,96,29,22,24,13,
   14,11,11,18,12,12,30,52,52,44,
   28,28,20,56,40,31,50,40,46,42,
   29,19,36,25,22,17,19,26,30,20,
   15,21,11,8,8,19,5,8,8,11,
   11,8,3,9,5,4,7,3,6,3,
   5,4,5,6
 };
 
 // =====================================================================
 //                     PLAYER ENGINE — shared state
 //   Everything in "sh" is touched only while holding "mtx".
 //   Every play/stop bumps sh.gen; both tasks drop old work when it changes.
 // =====================================================================
 struct Clip { uint8_t surah; uint16_t ayah; uint8_t reciter; };
 
 enum PState : uint8_t { PS_IDLE, PS_BUFFERING, PS_PLAYING, PS_PAUSED, PS_FINISHED, PS_ERROR };
 enum PError : uint8_t { PE_NONE, PE_NETWORK, PE_WIFI, PE_NOTFOUND };
 
 struct ClipEntry {
   Clip     clip;
   uint32_t start;   // ring position of first byte
   uint32_t end;     // valid when done
   uint32_t len;     // expected MP3 bytes (0 = unknown)
   bool     done;    // fully downloaded
 };
 
 struct Status {
   uint32_t gen;
   PState   state;
   PError   error;
   Clip     clip;
   float    progress;
   uint32_t elapsedMs;
   uint32_t totalMs;
 };
 
 // ---- AI ----
 enum AiPhase : uint8_t { AI_IDLE, AI_RECORDING, AI_LISTENED, AI_THINKING, AI_ANSWER, AI_ERROR };
 
 struct AiShared {
   bool     open;            // AI page is open (microphone on)
   uint32_t gen;             // bumps on open / close / new question
   AiPhase  phase;
   bool     micReady;
   bool     recReady;        // audio task -> net task: a recording is waiting
   uint32_t recGen;
   bool     busy;            // net task is working on a question
   uint8_t  level;           // mic meter 0..100
   uint32_t recMs;
   uint32_t answerId;
   char     error[24];
   char     question[240];   // plain ASCII, for the screen
   char     answer[1200];
 };
 
 // Compressed recording: block 0 is the (idle) playback buffer, the rest is heap.
 struct Recording {
   uint8_t* seg[REC_SEGS];
   int      segs;
   size_t   len;
   uint32_t samples;
 };
 
 typedef bool (*CancelFn)(uint32_t gen);
 struct BitWriter { uint8_t* p; size_t n; uint32_t acc; int bits; };
 
 struct Shared {
   uint32_t gen;
   bool     active;
   bool     paused;
   bool     continuous;
   Clip     first;
   float    volume;
   uint32_t wr, rd;              // ever-increasing ring positions
   ClipEntry q[CLIP_SLOTS];
   int      qHead, qCount;
   bool     netFinished;         // no more clips will arrive
   PError   netError;
   Clip     errorClip;
   Status   st;
   AiShared ai;
   char     voiceId[24];        // spoken answer to play (Clip with surah 0)
   char     contentPath[72];    // relative path for CONTENT_SURAH clips (e.g. adhan/fajr.mp3?v=1)
   bool     cfgNew;             // settings received from the dashboard
   int      cfgVolume, cfgReciter;
   uint8_t  beepLeft;           // soft offline beeps requested from the UI
   // Text fetch from content host (index/item JSON). Net task only; never with AI mic.
   bool     docWant;
   bool     docDone;
   bool     docOk;
   uint8_t  docErr;             // 0 ok, 1 wifi, 2 notfound, 3 other
   uint32_t docGen;
   char     docReq[72];
   char     docBuf[3200];
 };
 
 // ---- other types (kept up here: the Arduino IDE needs them before any function) ----
 enum FetchResult : uint8_t { FR_OK, FR_ABORT, FR_RETRY, FR_NOTFOUND };
 
 struct Download {
   uint32_t received = 0;     // raw file bytes already taken from the server
   uint32_t fileSize = 0;     // 0 = unknown
   bool     opened = false;   // clip slot created in the ring
   uint8_t  head[10];         // first bytes, to detect an ID3 tag
   uint8_t  headLen = 0;
   bool     headDone = false;
   uint32_t skip = 0;         // ID3 bytes still to throw away
 };
 
 enum Screen : uint8_t {
   SCR_HOME = 0, SCR_PLAYER = 1, SCR_AYAHS = 2, SCR_SETTINGS = 3, SCR_AI = 4,
   SCR_ADHAN = 5, SCR_HURUF = 6, SCR_HADITH = 7, SCR_QA = 8
 };
 enum PlayMode : uint8_t { MODE_SURAH = 0, MODE_AYAH = 1 };
 enum BootPhase : uint8_t { BOOT_WIFI, BOOT_BISMILLAH, BOOT_DONE };
 enum PrayerId : uint8_t {
   PT_FAJR = 0, PT_SUNRISE = 1, PT_DHUHR = 2, PT_ASR = 3, PT_MAGHRIB = 4, PT_ISHA = 5, PT_COUNT = 6
 };
 enum MethodId : uint8_t { METH_MWL = 0, METH_ISNA = 1, METH_EGYPT = 2, METH_UMM = 3, METH_KARACHI = 4 };
 enum AsrId : uint8_t { ASR_SHAFI = 0, ASR_HANAFI = 1 };
 enum HighLatId : uint8_t { HL_NONE = 0, HL_MIDNIGHT = 1, HL_ONESEVENTH = 2, HL_ANGLE = 3 };
 enum DocErr : uint8_t { DOC_OK = 0, DOC_WIFI = 1, DOC_NOTFOUND = 2, DOC_BAD = 3 };
 struct KeyEvent { uint8_t key; uint8_t repeat; };   // repeat 0 = fresh press
 
 // Cloud / Preferences settings used by Adhan and (later) content screens.
 struct DeviceSettings {
   char     contentUrl[96];
   char     timezone[40];
   float    lat, lon;
   uint8_t  method, asr, highLat;
   int8_t   adj[6];          // Fajr..Isha minute adjustments
   bool     prayerOn[5];     // Fajr, Dhuhr, Asr, Maghrib, Isha (no Sunrise)
   int      volume, reciter; // -1 = leave local value
   uint32_t configVersion;
 };
 
 static Shared sh;
 static DeviceSettings ds;
 static DeviceSettings cfgPending;   // written by net task when hello changes
 static SemaphoreHandle_t mtx;
 static uint8_t ringBuf[RING_SIZE];
 static bool timeSynced = false;
 static int prayerMin[PT_COUNT];     // minutes from local midnight
 static int prayerYday = -1;         // recomputed when the local day changes
 static int adhanScroll = 0;         // Adhan list scroll (0..1 for 6 rows / 4 visible)
 static char contentHost[64] = "buxinelectronics-art.github.io";
 static char contentBase[64] = "/Ruju";   // path prefix, no trailing slash
 static uint32_t contentVerHadith = 1;
 static uint32_t contentVerQa = 1;
 
 // Adhan auto-play runtime (Phase 2)
 enum AdhanRun : uint8_t { AR_IDLE = 0, AR_WAIT_AI = 1, AR_PLAYING = 2, AR_BEEP = 3, AR_HOLD = 4 };
 static AdhanRun adhanRun = AR_IDLE;
 static int adhanWhich = -1;          // PrayerId being announced
 static uint32_t adhanWaitStart = 0;
 static uint8_t adhanFiredMask = 0;   // bits 0..4 = Fajr,Dhuhr,Asr,Maghrib,Isha
 static int adhanFiredYday = -1;
 static bool adhanAlert = false;      // full-screen "Time for ..." UI
 static bool adhanCanResume = false;
 static int adhanResumeSurah = 1, adhanResumeAyah = 1, adhanResumeReciter = 0;
 static PlayMode adhanResumeMode = MODE_SURAH;
 static Screen adhanResumeScreen = SCR_HOME;
 static bool adhanResumeWantPlay = false;
 
 static i2s_chan_handle_t i2sTx = nullptr;
 static i2s_chan_handle_t i2sRx = nullptr;
 static Recording rec;
 static volatile bool talkHeld = false;    // KEY5 is physically held
 static HMP3Decoder mp3 = nullptr;
 static bool audioOk = false;
 
 static inline void lockSh()   { xSemaphoreTake(mtx, portMAX_DELAY); }
 static inline void unlockSh() { xSemaphoreGive(mtx); }
 
 static Clip nextClip(Clip c) {
   if (c.ayah < AYAHS[c.surah - 1]) {
     c.ayah++;
   } else {
     c.surah = (c.surah >= 114) ? 1 : c.surah + 1;
     c.ayah = 1;
   }
   return c;
 }
 
 // ---------------- control API (called from loop) ----------------
 static uint32_t resetSessionLocked() {
   sh.gen++;
   sh.paused = false;
   sh.wr = sh.rd = 0;
   sh.qHead = sh.qCount = 0;
   sh.netFinished = false;
   sh.netError = PE_NONE;
   return sh.gen;
 }
 
 static uint32_t playerStart(Clip c, bool continuous) {
   lockSh();
   uint32_t g = resetSessionLocked();
   sh.first = c;
   sh.continuous = continuous;
   sh.active = true;
   sh.st = Status{g, PS_BUFFERING, PE_NONE, c, 0.0f, 0, 0};
   unlockSh();
   return g;
 }
 
 static void playerStop() {
   lockSh();
   uint32_t g = resetSessionLocked();
   sh.active = false;
   sh.st = Status{g, PS_IDLE, PE_NONE, sh.first, 0.0f, 0, 0};
   unlockSh();
 }
 
 static void playerPause(bool p) { lockSh(); sh.paused = p; unlockSh(); }
 static void playerVolume(float v) { lockSh(); sh.volume = v; unlockSh(); }
 static Status playerStatus() { lockSh(); Status s = sh.st; unlockSh(); return s; }
 
 static bool stale(uint32_t gen) { lockSh(); bool s = (gen != sh.gen); unlockSh(); return s; }
 static bool helloRunning = false;   // a check-in gives way to the player
 static bool aiStale(uint32_t gen) {
   lockSh();
   bool s = (gen != sh.ai.gen) || (helloRunning && sh.active);
   unlockSh();
   return s;
 }
 
 static void aiSetOpen(bool open) {
   lockSh();
   sh.ai.open = open;
   sh.ai.gen++;
   sh.ai.phase = AI_IDLE;
   sh.ai.micReady = false;
   unlockSh();
 }
 
 // KEY5 pressed on the AI page. false = still busy with the last question.
 static bool aiTalk() {
   lockSh();
   AiShared& a = sh.ai;
   bool ok = a.open && !a.busy && !a.recReady &&
             (a.phase == AI_IDLE || a.phase == AI_ANSWER || a.phase == AI_ERROR);
   if (ok) {
     a.gen++;
     a.phase = AI_RECORDING;
     a.level = 0;
     a.recMs = 0;
   }
   unlockSh();
   return ok;
 }
 
 static void aiFail(uint32_t gen, const char* msg) {
   lockSh();
   if (gen == sh.ai.gen) {
     sh.ai.phase = AI_ERROR;
     snprintf(sh.ai.error, sizeof(sh.ai.error), "%s", msg);
   }
   unlockSh();
 }
 
 // =====================================================================
 //                 RECORDING STORE + SMALL FLAC ENCODER
 //   Lossless FLAC (14-bit, 16 kHz mono) roughly halves the memory needed.
 // =====================================================================
 static void recFree() {
   for (int i = 0; i < rec.segs; i++) free(rec.seg[i]);
   rec.segs = 0;
   rec.len = 0;
   rec.samples = 0;
 }
 
 static bool recAppend(const uint8_t* d, size_t n) {
   while (n > 0) {
     uint8_t* dst;
     size_t room;
     if (rec.len < RING_SIZE) {
       dst = ringBuf + rec.len;
       room = RING_SIZE - rec.len;
     } else {
       size_t off = rec.len - RING_SIZE;
       int i = (int)(off / REC_SEG);
       if (i >= rec.segs) {
         if (i >= REC_SEGS || ESP.getFreeHeap() < REC_HEAP_KEEP + REC_SEG) return false;
         uint8_t* p = (uint8_t*)malloc(REC_SEG);
         if (!p) return false;
         rec.seg[rec.segs++] = p;
       }
       dst = rec.seg[i] + off % REC_SEG;
       room = REC_SEG - off % REC_SEG;
     }
     size_t k = n < room ? n : room;
     memcpy(dst, d, k);
     rec.len += k;
     d += k;
     n -= k;
   }
   return true;
 }
 
 static void putBits(BitWriter& w, uint32_t v, int count) {   // count <= 24
   w.acc = (w.acc << count) | (v & ((1UL << count) - 1));
   w.bits += count;
   while (w.bits >= 8) { w.bits -= 8; w.p[w.n++] = (uint8_t)(w.acc >> w.bits); }
 }
 
 static uint16_t flacCrc(const uint8_t* d, size_t n, uint16_t poly, int width) {
   uint16_t c = 0, top = (uint16_t)(1U << (width - 1)), mask = (uint16_t)((1UL << width) - 1);
   while (n--) {
     c ^= (uint16_t)(*d++) << (width - 8);
     for (int i = 0; i < 8; i++) c = (uint16_t)(((c & top) ? (c << 1) ^ poly : (c << 1)) & mask);
   }
   return c;
 }
 
 // 42-byte stream header
 static void flacHeader(uint8_t* h, uint32_t totalSamples) {
   memset(h, 0, 42);
   memcpy(h, "fLaC", 4);
   h[4] = 0x80; h[7] = 34;                            // last block: STREAMINFO
   h[8] = FLAC_BLOCK >> 8; h[10] = FLAC_BLOCK >> 8;   // min / max block size
   BitWriter w = {h + 18, 0, 0, 0};
   putBits(w, MIC_RATE, 20);
   putBits(w, 0, 3);                                  // 1 channel
   putBits(w, 15, 5);                                 // 16 bits
   putBits(w, 0, 4);
   putBits(w, totalSamples >> 16, 16);
   putBits(w, totalSamples & 0xFFFF, 16);
 }
 
 // Encode one block of FLAC_BLOCK samples. Returns bytes written (< 2 KB).
 static size_t flacFrame(const int16_t* in, uint32_t frameNo, uint8_t* out) {
   static int32_t r[FLAC_BLOCK];
   BitWriter w = {out, 0, 0, 0};
   putBits(w, 0xFFF8, 16);
   putBits(w, 0xA, 4);                 // 1024 samples
   putBits(w, 0x5, 4);                 // 16 kHz
   putBits(w, 0x08, 8);                // mono, 16 bit
   if (frameNo < 128) putBits(w, frameNo, 8);
   else { putBits(w, 0xC0 | (frameNo >> 6), 8); putBits(w, 0x80 | (frameNo & 63), 8); }
   putBits(w, flacCrc(out, w.n, 0x07, 8), 8);
 
   // fixed predictor, order 2: residual of the 14-bit signal
   uint32_t k[8];
   uint64_t cost = 0;
   const int part = FLAC_BLOCK / 8;
   for (size_t i = 2; i < FLAC_BLOCK; i++)
     r[i] = (in[i] >> 2) - 2 * (in[i - 1] >> 2) + (in[i - 2] >> 2);
   for (int p = 0; p < 8; p++) {
     int first = p ? p * part : 2, last = (p + 1) * part;
     uint32_t sum = 0;
     for (int i = first; i < last; i++) sum += r[i] < 0 ? (uint32_t)(-r[i]) * 2 - 1 : (uint32_t)r[i] * 2;
     uint32_t mean = sum / (last - first);
     k[p] = 0;
     while (k[p] < 14 && (2UL << k[p]) <= mean) k[p]++;
     cost += 4 + (uint64_t)(last - first) * (k[p] + 1) + (sum >> k[p]);
   }
 
   if (cost < (uint64_t)FLAC_BLOCK * 14 - 64) {
     putBits(w, 0x15, 8);              // FIXED order 2, has wasted bits
     putBits(w, 1, 2);                 // 2 wasted bits
     putBits(w, in[0] >> 2, 14);
     putBits(w, in[1] >> 2, 14);
     putBits(w, 0, 2);                 // Rice coding
     putBits(w, 3, 4);                 // 8 partitions
     for (int p = 0; p < 8; p++) {
       int first = p ? p * part : 2, last = (p + 1) * part;
       putBits(w, k[p], 4);
       for (int i = first; i < last; i++) {
         uint32_t u = r[i] < 0 ? (uint32_t)(-r[i]) * 2 - 1 : (uint32_t)r[i] * 2;
         uint32_t q = u >> k[p];
         while (q >= 16) { putBits(w, 0, 16); q -= 16; }
         putBits(w, 1, q + 1);
         if (k[p]) putBits(w, u, k[p]);
       }
     }
   } else {                            // very loud noise: store as-is
     putBits(w, 0x03, 8);              // VERBATIM, has wasted bits
     putBits(w, 1, 2);
     for (size_t i = 0; i < FLAC_BLOCK; i++) putBits(w, in[i] >> 2, 14);
   }
   if (w.bits) putBits(w, 0, 8 - w.bits);
   uint16_t crc = flacCrc(out, w.n, 0x8005, 16);
   putBits(w, crc, 16);
   return w.n;
 }
 
 // =====================================================================
 //                    NETWORK TASK (HTTPS -> ring buffer)
 // =====================================================================
 static const char* const HOST = "everyayah.com";
 static WiFiClientSecure client;
 static WiFiClientSecure aiClient;        // our backend (certificate verified)
 
 // Reserve a slot for a new clip. Waits while all slots are full.
 static bool openClip(uint32_t gen, const Clip& c, uint32_t len) {
   for (;;) {
     lockSh();
     if (gen != sh.gen) { unlockSh(); return false; }
     if (sh.qCount < CLIP_SLOTS) {
       ClipEntry& e = sh.q[(sh.qHead + sh.qCount) % CLIP_SLOTS];
       e = ClipEntry{c, sh.wr, sh.wr, len, false};
       sh.qCount++;
       unlockSh();
       return true;
     }
     unlockSh();
     vTaskDelay(pdMS_TO_TICKS(20));
   }
 }
 
 static void closeClip(uint32_t gen) {
   lockSh();
   if (gen == sh.gen && sh.qCount > 0) {
     ClipEntry& e = sh.q[(sh.qHead + sh.qCount - 1) % CLIP_SLOTS];
     if (!e.done) {
       e.done = true;
       e.end = sh.wr;
       e.len = e.end - e.start;
     }
   }
   unlockSh();
 }
 
 // Copy into the ring, waiting for space. false = this session was cancelled.
 static bool ringPush(uint32_t gen, const uint8_t* d, size_t n) {
   while (n > 0) {
     lockSh();
     if (gen != sh.gen) { unlockSh(); return false; }
     size_t space = RING_SIZE - (size_t)(sh.wr - sh.rd);
     size_t k = n < space ? n : space;
     if (k > 0) {
       size_t at = sh.wr % RING_SIZE;
       size_t first = (k < RING_SIZE - at) ? k : RING_SIZE - at;
       memcpy(ringBuf + at, d, first);
       memcpy(ringBuf, d + first, k - first);
       sh.wr += k;
       d += k;
       n -= k;
     }
     unlockSh();
     if (n > 0) vTaskDelay(pdMS_TO_TICKS(15));
   }
   return true;
 }
 
 // Remove a leading ID3v2 tag (it can contain bytes that sound like noise).
 static bool feedAudio(uint32_t gen, Download& dl, const uint8_t* d, size_t n) {
   if (!dl.headDone) {
     while (n > 0 && dl.headLen < 10) { dl.head[dl.headLen++] = *d++; n--; }
     if (dl.headLen < 10) return true;
     dl.headDone = true;
     if (!memcmp(dl.head, "ID3", 3)) {
       uint32_t size = ((uint32_t)(dl.head[6] & 0x7F) << 21) | ((uint32_t)(dl.head[7] & 0x7F) << 14) |
                       ((uint32_t)(dl.head[8] & 0x7F) << 7)  |  (uint32_t)(dl.head[9] & 0x7F);
       if (dl.head[5] & 0x10) size += 10;      // footer
       dl.skip = size;
       lockSh();
       if (gen == sh.gen && sh.qCount > 0) {
         ClipEntry& e = sh.q[(sh.qHead + sh.qCount - 1) % CLIP_SLOTS];
         if (e.len > size + 10) e.len -= size + 10;
       }
       unlockSh();
     } else {
       if (!ringPush(gen, dl.head, 10)) return false;
     }
   }
   if (dl.skip > 0) {
     size_t k = n < dl.skip ? n : dl.skip;
     d += k;
     n -= k;
     dl.skip -= k;
   }
   return n == 0 ? true : ringPush(gen, d, n);
 }
 
 // Read one header line (without CR/LF). false = timeout, closed or cancelled.
 static bool readLine(WiFiClientSecure& c_, CancelFn cancelled, uint32_t gen,
                      char* out, size_t outSz, uint32_t timeoutMs) {
   size_t n = 0;
   uint32_t t0 = millis();
   for (;;) {
     if (c_.available() > 0) {
       int c = c_.read();
       if (c < 0) continue;
       if (c == '\n') { out[n] = 0; return true; }
       if (c != '\r' && n + 1 < outSz) out[n++] = (char)c;
     } else {
       if (!c_.connected() || millis() - t0 > timeoutMs || cancelled(gen)) return false;
       vTaskDelay(pdMS_TO_TICKS(5));
     }
   }
 }
 
 static bool headerIs(const char* line, const char* name, const char** value) {
   size_t n = strlen(name);
   if (strncasecmp(line, name, n) != 0 || line[n] != ':') return false;
   const char* v = line + n + 1;
   while (*v == ' ') v++;
   *value = v;
   return true;
 }
 
 static void contentParseUrl(const char* url) {
   // Expect https://host[/base...]
   snprintf(contentHost, sizeof(contentHost), "buxinelectronics-art.github.io");
   snprintf(contentBase, sizeof(contentBase), "/Ruju");
   if (!url || strncmp(url, "https://", 8) != 0) return;
   const char* host = url + 8;
   const char* slash = strchr(host, '/');
   size_t hostLen = slash ? (size_t)(slash - host) : strlen(host);
   if (hostLen == 0 || hostLen >= sizeof(contentHost)) return;
   memcpy(contentHost, host, hostLen);
   contentHost[hostLen] = 0;
   if (slash && slash[1]) {
     snprintf(contentBase, sizeof(contentBase), "%s", slash);
     size_t n = strlen(contentBase);
     while (n > 1 && contentBase[n - 1] == '/') contentBase[--n] = 0;
   } else {
     contentBase[0] = 0;
   }
 }
 
 static bool docStale(uint32_t gen) {
   lockSh(); bool s = (gen != sh.docGen) || sh.ai.open || sh.active; unlockSh();
   return s;
 }
 
 static void docRequest(const char* relPath) {
   lockSh();
   sh.docGen++;
   snprintf(sh.docReq, sizeof(sh.docReq), "%s", relPath);
   sh.docWant = true;
   sh.docDone = false;
   sh.docOk = false;
   sh.docErr = DOC_BAD;
   sh.docBuf[0] = 0;
   unlockSh();
 }
 
 static void docCancel() {
   lockSh();
   sh.docGen++;
   sh.docWant = false;
   sh.docDone = false;
   unlockSh();
 }
 
 // Fetch a small JSON/text file from the content host into sh.docBuf.
 static void docFetchOnce(uint32_t gen) {
   if (WiFi.status() != WL_CONNECTED) {
     lockSh();
     if (gen == sh.docGen) { sh.docDone = true; sh.docOk = false; sh.docErr = DOC_WIFI; sh.docWant = false; }
     unlockSh();
     return;
   }
   char reqPath[96], rel[72];
   lockSh(); snprintf(rel, sizeof(rel), "%s", sh.docReq); unlockSh();
   if (contentBase[0]) snprintf(reqPath, sizeof(reqPath), "%s/%s", contentBase, rel);
   else snprintf(reqPath, sizeof(reqPath), "/%s", rel);
 
   client.stop();
   client.useBuiltinCACertBundle();
   client.setHandshakeTimeout(12);
   if (!client.connect(contentHost, 443, 8000)) {
     lockSh();
     if (gen == sh.docGen) { sh.docDone = true; sh.docOk = false; sh.docErr = DOC_WIFI; sh.docWant = false; }
     unlockSh();
     return;
   }
   char req[280];
   int n = snprintf(req, sizeof(req),
                    "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: QuranPlayer/" FW_VERSION "\r\n"
                    "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                    reqPath, contentHost);
   if (client.write((const uint8_t*)req, n) != (size_t)n) { client.stop(); return; }
 
   char line[200];
   if (!readLine(client, docStale, gen, line, sizeof(line), STALL_MS)) { client.stop(); return; }
   const char* sp = strchr(line, ' ');
   int code = sp ? atoi(sp + 1) : 0;
   long contentLen = -1;
   for (;;) {
     if (!readLine(client, docStale, gen, line, sizeof(line), STALL_MS)) { client.stop(); return; }
     if (!line[0]) break;
     const char* v;
     if (headerIs(line, "Content-Length", &v)) contentLen = atol(v);
   }
   size_t got = 0;
   uint32_t last = millis();
   while (contentLen < 0 || (long)got < contentLen) {
     if (docStale(gen)) { client.stop(); return; }
     int avail = client.available();
     if (avail <= 0) {
       if (!client.connected()) break;
       if (millis() - last > STALL_MS) break;
       vTaskDelay(pdMS_TO_TICKS(5));
       continue;
     }
     uint8_t tmp[256];
     size_t want = (size_t)avail < sizeof(tmp) ? (size_t)avail : sizeof(tmp);
     if (contentLen >= 0 && (long)(got + want) > contentLen) want = (size_t)(contentLen - (long)got);
     int r = client.read(tmp, want);
     if (r <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
     lockSh();
     if (gen == sh.docGen) {
       size_t room = sizeof(sh.docBuf) - 1 - got;
       size_t k = (size_t)r < room ? (size_t)r : room;
       memcpy(sh.docBuf + got, tmp, k);
       got += k;
       sh.docBuf[got] = 0;
     }
     unlockSh();
     last = millis();
     if (got >= sizeof(sh.docBuf) - 1) break;
   }
   client.stop();
   lockSh();
   if (gen == sh.docGen) {
     sh.docDone = true;
     sh.docWant = false;
     if (code == 404) { sh.docOk = false; sh.docErr = DOC_NOTFOUND; }
     else if (code != 200 || got == 0) { sh.docOk = false; sh.docErr = DOC_BAD; }
     else { sh.docOk = true; sh.docErr = DOC_OK; }
   }
   unlockSh();
 }
 
 // One HTTP request for the rest of the file. Keeps the TLS connection alive.
 static FetchResult fetchOnce(uint32_t gen, const Clip& c, Download& dl) {
   const bool voice = (c.surah == 0);          // AI answer from our backend
   const bool content = (c.surah == CONTENT_SURAH);
   WiFiClientSecure& cl = voice ? aiClient : client;
   const char* host = voice ? BACKEND_HOST : (content ? contentHost : HOST);
   if (!cl.connected()) {
     cl.stop();
     if (voice || content) cl.useBuiltinCACertBundle(); else cl.setInsecure();
     cl.setHandshakeTimeout(12);               // seconds
     if (!cl.connect(host, 443, 8000)) {
       LOGF("connect failed, heap=%u\n", (unsigned)ESP.getFreeHeap());
       return FR_RETRY;
     }
   }
   if (stale(gen)) return FR_ABORT;
 
   char path[160];
   char auth[200] = "";
   if (voice) {
     lockSh();
     snprintf(path, sizeof(path), "/api/device/audio/%s", sh.voiceId);
     unlockSh();
     snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\nX-Product-ID: %s\r\n", DEVICE_TOKEN, PRODUCT_ID);
   } else if (content) {
     char rel[48];
     lockSh();
     snprintf(rel, sizeof(rel), "%s", sh.contentPath);
     unlockSh();
     if (contentBase[0])
       snprintf(path, sizeof(path), "%s/%s", contentBase, rel);
     else
       snprintf(path, sizeof(path), "/%s", rel);
   } else {
     snprintf(path, sizeof(path), "/data/%s/%03u%03u.mp3",
              RECITERS[c.reciter].ayahFolder, (unsigned)c.surah, (unsigned)c.ayah);
   }
   char req[480];
   int n = snprintf(req, sizeof(req),
                    "GET %s HTTP/1.1\r\nHost: %s\r\n%s"
                    "User-Agent: QuranPlayer/6\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
                    "Connection: keep-alive\r\n",
                    path, host, auth);
   if (dl.received > 0)
     n += snprintf(req + n, sizeof(req) - n, "Range: bytes=%lu-\r\n", (unsigned long)dl.received);
   n += snprintf(req + n, sizeof(req) - n, "\r\n");
   if (cl.write((const uint8_t*)req, n) != (size_t)n) { cl.stop(); return FR_RETRY; }
 
   // ---- reply headers ----
   char line[200];
   if (!readLine(cl, stale, gen, line, sizeof(line), STALL_MS)) { cl.stop(); return stale(gen) ? FR_ABORT : FR_RETRY; }
   const char* sp = strchr(line, ' ');
   int code = sp ? atoi(sp + 1) : 0;
   bool keep = (strncmp(line, "HTTP/1.1", 8) == 0);
   bool chunked = false;
   long contentLen = -1;
   long rangeTotal = -1;
   for (;;) {
     if (!readLine(cl, stale, gen, line, sizeof(line), STALL_MS)) { cl.stop(); return stale(gen) ? FR_ABORT : FR_RETRY; }
     if (!line[0]) break;
     const char* v;
     if (headerIs(line, "Content-Length", &v)) contentLen = atol(v);
     else if (headerIs(line, "Content-Range", &v)) { const char* s = strchr(v, '/'); if (s) rangeTotal = atol(s + 1); }
     else if (headerIs(line, "Transfer-Encoding", &v)) chunked = (strncasecmp(v, "chunked", 7) == 0);
     else if (headerIs(line, "Connection", &v)) keep = (strncasecmp(v, "close", 5) != 0);
   }
   if (code == 404) { cl.stop(); return FR_NOTFOUND; }
   if (code != 200 && code != 206) { LOGF("HTTP %d\n", code); cl.stop(); return FR_RETRY; }
 
   uint32_t skipOld = 0;                       // server ignored Range: discard what we have
   if (code == 200) {
     skipOld = dl.received;
     if (contentLen > 0) dl.fileSize = (uint32_t)contentLen;
   } else if (rangeTotal > 0) {
     dl.fileSize = (uint32_t)rangeTotal;
   }
 
   if (!dl.opened) {
     if (!openClip(gen, c, dl.fileSize)) { cl.stop(); return FR_ABORT; }
     dl.opened = true;
   }
 
   // ---- body ----
   static uint8_t buf[1024];
   long bodyLeft = chunked ? -1 : contentLen;  // -1 = until close
   long chunkLeft = 0;
   uint32_t lastData = millis();
   bool complete = false;
 
   while (!complete) {
     if (stale(gen)) { cl.stop(); return FR_ABORT; }
 
     if (chunked && chunkLeft == 0) {
       if (!readLine(cl, stale, gen, line, sizeof(line), STALL_MS)) break;
       if (!line[0]) continue;                 // CRLF after the previous chunk
       chunkLeft = strtol(line, nullptr, 16);
       if (chunkLeft <= 0) {                   // last chunk
         while (readLine(cl, stale, gen, line, sizeof(line), STALL_MS) && line[0]) {}
         complete = true;
       }
       continue;
     }
     if (!chunked && bodyLeft == 0) { complete = true; continue; }
 
     int avail = cl.available();
     if (avail <= 0) {
       if (!cl.connected()) {
         if (!chunked && contentLen < 0) complete = true;   // close marks the end
         break;
       }
       if (millis() - lastData > STALL_MS) break;
       vTaskDelay(pdMS_TO_TICKS(5));
       continue;
     }
     size_t want = (size_t)avail < sizeof(buf) ? (size_t)avail : sizeof(buf);
     if (chunked && (long)want > chunkLeft) want = (size_t)chunkLeft;
     if (!chunked && bodyLeft > 0 && (long)want > bodyLeft) want = (size_t)bodyLeft;
     int got = cl.read(buf, want);
     if (got <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); if (millis() - lastData > STALL_MS) break; continue; }
     if (chunked) chunkLeft -= got;
     else if (bodyLeft > 0) bodyLeft -= got;
 
     const uint8_t* p = buf;
     size_t m = (size_t)got;
     if (skipOld > 0) {
       size_t k = m < skipOld ? m : skipOld;
       p += k; m -= k; skipOld -= k;
     }
     if (m > 0) {
       dl.received += m;
       if (!feedAudio(gen, dl, p, m)) { cl.stop(); return FR_ABORT; }
     }
     lastData = millis();                      // after push: a full buffer is not a stall
     vTaskDelay(1);
   }
 
   if (!complete || !keep) cl.stop();
   return complete ? FR_OK : FR_RETRY;
 }
 
 // Download one ayah completely, resuming after drops. Returns PE_NONE on success.
 static PError fetchClip(uint32_t gen, const Clip& c, bool& aborted) {
   Download dl;
   int fails = 0;
   aborted = false;
   for (;;) {
     uint32_t t0 = millis();
     while (WiFi.status() != WL_CONNECTED) {
       if (stale(gen)) { aborted = true; return PE_NONE; }
       if (millis() - t0 > WIFI_WAIT_MS) { client.stop(); aiClient.stop(); closeClip(gen); return PE_WIFI; }
       if (millis() - t0 > WIFI_WAIT_MS / 2) {
         static uint32_t lastKick = 0;
         if (millis() - lastKick > WIFI_WAIT_MS) { lastKick = millis(); WiFi.reconnect(); }
       }
       vTaskDelay(pdMS_TO_TICKS(100));
     }
 
     WiFiClientSecure& cl = (c.surah == 0) ? aiClient : client;
     bool reused = cl.connected();
     uint32_t before = dl.received;
     FetchResult r = fetchOnce(gen, c, dl);
 
     if (r == FR_ABORT) { aborted = true; return PE_NONE; }
     if (r == FR_NOTFOUND) { closeClip(gen); return PE_NOTFOUND; }
     if (r == FR_OK) {
       if (!dl.headDone && dl.headLen > 0 && !ringPush(gen, dl.head, dl.headLen)) { aborted = true; return PE_NONE; }
       closeClip(gen);
       return PE_NONE;
     }
     // FR_RETRY
     if (dl.fileSize > 0 && dl.received >= dl.fileSize) { closeClip(gen); return PE_NONE; }
     if (dl.received > before) fails = 0;            // progress: keep going
     else if (!(reused && fails == 0)) fails++;      // old keep-alive socket: free retry
     else { cl.stop(); fails = 1; continue; }
     if (fails >= MAX_RETRIES) { closeClip(gen); return PE_NETWORK; }
     for (int i = 0; i < fails * 3; i++) {           // back off, but stay cancellable
       if (stale(gen)) { aborted = true; return PE_NONE; }
       vTaskDelay(pdMS_TO_TICKS(100));
     }
   }
 }
 
 // =====================================================================
 //                  DEEN AI  (runs inside the network task)
 // =====================================================================
 static char aiBody[4096];                 // reply from the server
 
 // ---- UTF-8 -> screen-safe ASCII ----
 static void asciiClean(const char* in, char* out, size_t outSz) {
   static const char* const LATIN1 =       // U+00C0 .. U+00FF
     "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
   size_t n = 0;
   const uint8_t* p = (const uint8_t*)in;
   while (*p && n + 8 < outSz) {
     uint32_t c = *p++;
     int more = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
     if (more) c &= 0x3F >> more;
     while (more-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
 
     char one[2] = {0, 0};
     const char* add = one;
     if (c == '\n' || c == '\t' || c == ' ' || c == 0xA0 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F) {
       one[0] = (c == '\n') ? '\n' : ' ';
       if (n == 0 || out[n - 1] == '\n' || (out[n - 1] == ' ')) continue;
     }
     else if (c == '*' || c == '#' || c == '`') continue;
     else if (c >= 32 && c < 127) one[0] = (char)c;
     else if (c >= 0xC0 && c <= 0xFF) one[0] = LATIN1[c - 0xC0];
     else if (c == 0x2018 || c == 0x2019 || c == 0x02BC || c == 0x02BE || c == 0x02BF) one[0] = '\'';
     else if (c == 0x201C || c == 0x201D) one[0] = '"';
     else if ((c >= 0x2010 && c <= 0x2015) || c == 0x2212 || c == 0x2022) one[0] = '-';
     else if (c == 0x2026) add = "...";
     else if (c == 0xFDFA) add = "(pbuh)";
     else if (c == 0x100 || c == 0x101) one[0] = (c & 1) ? 'a' : 'A';
     else if (c == 0x12A || c == 0x12B) one[0] = (c & 1) ? 'i' : 'I';
     else if (c == 0x16A || c == 0x16B) one[0] = (c & 1) ? 'u' : 'U';
     else if (c == 0x1E0C || c == 0x1E0D) one[0] = (c & 1) ? 'd' : 'D';
     else if (c == 0x1E24 || c == 0x1E25) one[0] = (c & 1) ? 'h' : 'H';
     else if (c == 0x1E62 || c == 0x1E63) one[0] = (c & 1) ? 's' : 'S';
     else if (c == 0x1E6C || c == 0x1E6D) one[0] = (c & 1) ? 't' : 'T';
     else if (c == 0x1E92 || c == 0x1E93) one[0] = (c & 1) ? 'z' : 'Z';
     else continue;                        // Arabic and other scripts are skipped
     while (*add) out[n++] = *add++;
   }
   while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n')) n--;
   out[n] = 0;
 }
 
 // ---- minimal JSON helpers ----
 static int hexVal(char c) {
   if (c >= '0' && c <= '9') return c - '0';
   c |= 32;
   return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
 }
 static bool hex4(const char* p, uint32_t& v) {
   v = 0;
   for (int i = 0; i < 4; i++) { int h = hexVal(p[i]); if (h < 0) return false; v = v * 16 + h; }
   return true;
 }
 
 // Read the JSON string starting after its opening quote; returns the position after it.
 static const char* jsonString(const char* p, char* out, size_t outSz) {
   size_t n = 0;
   while (*p && *p != '"') {
     uint32_t c = (uint8_t)*p++;
     bool raw = true;
     if (c == '\\' && *p) {
       char e = *p++;
       raw = false;
       if (e == 'n') c = '\n';
       else if (e == 't' || e == 'r' || e == 'b' || e == 'f') c = ' ';
       else if (e == 'u' && hex4(p, c)) {
         p += 4;
         uint32_t lo;
         if (c >= 0xD800 && c <= 0xDBFF && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, lo)) {
           c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
           p += 6;
         }
       } else c = (uint8_t)e;
     }
     if (out && n + 5 < outSz) {
       if (raw || c < 0x80) out[n++] = (char)c;
       else if (c < 0x800) { out[n++] = 0xC0 | (c >> 6); out[n++] = 0x80 | (c & 63); }
       else if (c < 0x10000) { out[n++] = 0xE0 | (c >> 12); out[n++] = 0x80 | ((c >> 6) & 63); out[n++] = 0x80 | (c & 63); }
       else { out[n++] = 0xF0 | (c >> 18); out[n++] = 0x80 | ((c >> 12) & 63); out[n++] = 0x80 | ((c >> 6) & 63); out[n++] = 0x80 | (c & 63); }
     }
   }
   if (out) out[n] = 0;
   return *p == '"' ? p + 1 : p;
 }
 
 // First string value stored under "key" anywhere in the document.
 static bool jsonFind(const char* json, const char* key, char* out, size_t outSz) {
   const char* p = json;
   char name[16];
   while ((p = strchr(p, '"')) != nullptr) {
     p = jsonString(p + 1, name, sizeof(name));
     const char* q = p;
     while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
     if (*q != ':' || strcmp(name, key) != 0) continue;
     q++;
     while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
     if (*q != '"') continue;
     jsonString(q + 1, out, outSz);
     return true;
   }
   return false;
 }
 
 // ---- HTTPS to our backend (certificate is verified, the token stays private) ----
 static bool aiSend(uint32_t gen, const void* data, size_t len) {
   const uint8_t* p = (const uint8_t*)data;
   while (len > 0) {
     if (aiStale(gen)) return false;
     size_t k = len < 1024 ? len : 1024;
     if (aiClient.write(p, k) != k) return false;
     p += k;
     len -= k;
   }
   return true;
 }
 
 static bool aiOpenRequest(uint32_t gen, const char* path, const char* type, size_t length) {
   if (!aiClient.connected()) {
     aiClient.stop();
     aiClient.useBuiltinCACertBundle();
     aiClient.setHandshakeTimeout(15);
     if (!aiClient.connect(BACKEND_HOST, 443, 10000)) {
       LOGF("backend connect failed, heap=%u\n", (unsigned)ESP.getFreeHeap());
       return false;
     }
   }
   char h[480];
   int n = snprintf(h, sizeof(h),
                    "POST %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\nX-Product-ID: %s\r\n"
                    "X-Firmware: " FW_VERSION "\r\nAccept-Encoding: identity\r\n"
                    "Content-Type: %s\r\nContent-Length: %u\r\nConnection: keep-alive\r\n\r\n",
                    path, BACKEND_HOST, DEVICE_TOKEN, PRODUCT_ID, type, (unsigned)length);
   return n < (int)sizeof(h) && aiSend(gen, h, n);
 }
 
 // Returns the HTTP status (0 = connection problem). Body goes into aiBody.
 static int aiReadReply(uint32_t gen) {
   char line[200];
   if (!readLine(aiClient, aiStale, gen, line, sizeof(line), AI_REPLY_MS)) return 0;
   const char* sp = strchr(line, ' ');
   int code = sp ? atoi(sp + 1) : 0;
   long length = -1;
   bool chunked = false, keep = true;
   for (;;) {
     if (!readLine(aiClient, aiStale, gen, line, sizeof(line), STALL_MS)) return 0;
     if (!line[0]) break;
     const char* v;
     if (headerIs(line, "Content-Length", &v)) length = atol(v);
     else if (headerIs(line, "Transfer-Encoding", &v)) chunked = (strncasecmp(v, "chunked", 7) == 0);
     else if (headerIs(line, "Connection", &v)) keep = (strncasecmp(v, "close", 5) != 0);
   }
   size_t n = 0;
   long left = chunked ? 0 : length;
   uint32_t last = millis();
   bool complete = false;
   while (!complete) {
     if (aiStale(gen)) return 0;
     if (chunked && left == 0) {
       if (!readLine(aiClient, aiStale, gen, line, sizeof(line), STALL_MS)) break;
       if (!line[0]) continue;
       left = strtol(line, nullptr, 16);
       if (left <= 0) {
         while (readLine(aiClient, aiStale, gen, line, sizeof(line), STALL_MS) && line[0]) {}
         complete = true;
       }
       continue;
     }
     if (!chunked && left == 0) { complete = true; break; }
     int avail = aiClient.available();
     if (avail <= 0) {
       if (!aiClient.connected()) { complete = (!chunked && length < 0); break; }
       if (millis() - last > STALL_MS) break;
       vTaskDelay(pdMS_TO_TICKS(5));
       continue;
     }
     uint8_t tmp[256];
     size_t want = (size_t)avail < sizeof(tmp) ? (size_t)avail : sizeof(tmp);
     if (left > 0 && (long)want > left) want = (size_t)left;
     int got = aiClient.read(tmp, want);
     if (got <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
     if (left > 0) left -= got;
     size_t k = (size_t)got < sizeof(aiBody) - 1 - n ? (size_t)got : sizeof(aiBody) - 1 - n;
     memcpy(aiBody + n, tmp, k);
     n += k;
     last = millis();
   }
   aiBody[n] = 0;
   if (!complete || !keep) aiClient.stop();
   return complete ? code : 0;
 }
 
 static void aiHttpError(uint32_t gen, int code) {
   char msg[24];
   LOGF("backend HTTP %d: %.200s\n", code, code ? aiBody : "");
   if (code == 0) snprintf(msg, sizeof(msg), "No connection");
   else if (!jsonFind(aiBody, "error", msg, sizeof(msg))) snprintf(msg, sizeof(msg), "Server error %d", code);
   aiFail(gen, msg);                            // the server sends short messages
 }
 
 // Upload the recording. Returns the HTTP status, 0 = no reply, -1 = not sent.
 static int aiAskOnce(uint32_t gen) {
   uint8_t fh[42];
   flacHeader(fh, rec.samples);
   if (!aiOpenRequest(gen, "/api/device/ask", "audio/flac", sizeof(fh) + rec.len)) return -1;
   if (!aiSend(gen, fh, sizeof(fh))) return -1;
   size_t sent = rec.len < RING_SIZE ? rec.len : RING_SIZE;
   if (!aiSend(gen, ringBuf, sent)) return -1;
   for (int i = 0; sent < rec.len && i < rec.segs; i++) {
     size_t k = rec.len - sent < REC_SEG ? rec.len - sent : REC_SEG;
     if (!aiSend(gen, rec.seg[i], k)) return -1;
     sent += k;
   }
   return aiReadReply(gen);
 }
 
 static void aiAnswerQuestion(uint32_t gen) {
   uint32_t t0 = millis();
   while (WiFi.status() != WL_CONNECTED) {
     if (aiStale(gen)) return;
     if (millis() - t0 > 8000) { aiFail(gen, "No WiFi"); return; }
     vTaskDelay(pdMS_TO_TICKS(100));
   }
   client.stop();                               // free the player's connection memory
   aiClient.stop();
   lockSh();
   if (gen == sh.ai.gen) { sh.ai.question[0] = 0; sh.ai.phase = AI_THINKING; }
   unlockSh();
 
   int code = aiAskOnce(gen);
   if (code < 0 && !aiStale(gen)) {             // never reached the server: try once more
     aiClient.stop();
     vTaskDelay(pdMS_TO_TICKS(600));
     code = aiAskOnce(gen);
   }
   if (aiStale(gen)) return;
   recFree();                                   // the player needs this memory next
   if (code != 200) { aiHttpError(gen, code < 0 ? 0 : code); return; }
 
   static char raw[1600], question[240], answer[1200];
   char voiceId[24] = "";
   if (!jsonFind(aiBody, "question", raw, sizeof(raw))) raw[0] = 0;
   asciiClean(raw, question, sizeof(question));
   if (!jsonFind(aiBody, "answer", raw, sizeof(raw))) { aiFail(gen, "No answer"); return; }
   asciiClean(raw, answer, sizeof(answer));
   if (!answer[0]) snprintf(answer, sizeof(answer), "(Listen to the answer)");
   jsonFind(aiBody, "audio_id", voiceId, sizeof(voiceId));
 
   lockSh();
   if (gen == sh.ai.gen) {
     // screen: question first, then the answer
     snprintf(sh.ai.answer, sizeof(sh.ai.answer), "Q: %s\n%s", question, answer);
     sh.ai.answerId++;
     sh.ai.phase = AI_ANSWER;
     if (voiceId[0]) {                          // speak it with the normal player
       uint32_t g = resetSessionLocked();
       memcpy(sh.voiceId, voiceId, sizeof(sh.voiceId));
       sh.first = Clip{0, 1, 0};
       sh.continuous = false;
       sh.active = true;
       sh.st = Status{g, PS_BUFFERING, PE_NONE, sh.first, 0.0f, 0, 0};
     }
   }
   unlockSh();
 }
 
 static int jsonInt(const char* json, const char* key, int fallback) {
   const char* p = strstr(json, key);
   if (!p || !(p = strchr(p, ':'))) return fallback;
   return atoi(p + 1);
 }
 
 static float jsonFloat(const char* json, const char* key, float fallback) {
   const char* p = strstr(json, key);
   if (!p || !(p = strchr(p, ':'))) return fallback;
   return (float)atof(p + 1);
 }
 
 static uint8_t methodFromName(const char* s) {
   if (!strcmp(s, "ISNA")) return METH_ISNA;
   if (!strcmp(s, "Egypt")) return METH_EGYPT;
   if (!strcmp(s, "UmmAlQura")) return METH_UMM;
   if (!strcmp(s, "Karachi")) return METH_KARACHI;
   return METH_MWL;
 }
 static uint8_t asrFromName(const char* s) {
   return (!strcmp(s, "Hanafi")) ? (uint8_t)ASR_HANAFI : (uint8_t)ASR_SHAFI;
 }
 static uint8_t highLatFromName(const char* s) {
   if (!strcmp(s, "MidNight")) return HL_MIDNIGHT;
   if (!strcmp(s, "OneSeventh")) return HL_ONESEVENTH;
   if (!strcmp(s, "AngleBased")) return HL_ANGLE;
   return HL_NONE;
 }
 
 static void dsDefaults(DeviceSettings& d) {
   memset(&d, 0, sizeof(d));
   snprintf(d.contentUrl, sizeof(d.contentUrl), "%s", CONTENT_BASE_URL);
   snprintf(d.timezone, sizeof(d.timezone), "GMT0");
   d.lat = 21.4225f;
   d.lon = 39.8262f;
   d.method = METH_MWL;
   d.asr = ASR_SHAFI;
   d.highLat = HL_NONE;
   for (int i = 0; i < 5; i++) d.prayerOn[i] = true;
   d.volume = -1;
   d.reciter = -1;
   d.configVersion = 1;
 }
 
 static void dsLoadPrefs() {
   dsDefaults(ds);
   Preferences p;
   if (!p.begin("deen", true)) return;
   p.getString("url", ds.contentUrl, sizeof(ds.contentUrl));
   p.getString("tz", ds.timezone, sizeof(ds.timezone));
   ds.lat = p.getFloat("lat", ds.lat);
   ds.lon = p.getFloat("lon", ds.lon);
   ds.method = (uint8_t)p.getUChar("method", ds.method);
   ds.asr = (uint8_t)p.getUChar("asr", ds.asr);
   ds.highLat = (uint8_t)p.getUChar("hl", ds.highLat);
   for (int i = 0; i < 6; i++) {
     char k[6]; snprintf(k, sizeof(k), "a%d", i);
     ds.adj[i] = (int8_t)p.getChar(k, 0);
   }
   for (int i = 0; i < 5; i++) {
     char k[6]; snprintf(k, sizeof(k), "p%d", i);
     ds.prayerOn[i] = p.getBool(k, true);
   }
   ds.configVersion = p.getUInt("cfgv", 1);
   p.end();
   if (!ds.contentUrl[0]) snprintf(ds.contentUrl, sizeof(ds.contentUrl), "%s", CONTENT_BASE_URL);
   if (!ds.timezone[0]) snprintf(ds.timezone, sizeof(ds.timezone), "GMT0");
 }
 
 static void dsSavePrefs() {
   Preferences p;
   if (!p.begin("deen", false)) return;
   p.putString("url", ds.contentUrl);
   p.putString("tz", ds.timezone);
   p.putFloat("lat", ds.lat);
   p.putFloat("lon", ds.lon);
   p.putUChar("method", ds.method);
   p.putUChar("asr", ds.asr);
   p.putUChar("hl", ds.highLat);
   for (int i = 0; i < 6; i++) {
     char k[6]; snprintf(k, sizeof(k), "a%d", i);
     p.putChar(k, (int8_t)ds.adj[i]);
   }
   for (int i = 0; i < 5; i++) {
     char k[6]; snprintf(k, sizeof(k), "p%d", i);
     p.putBool(k, ds.prayerOn[i]);
   }
   p.putUInt("cfgv", ds.configVersion);
   p.end();
 }
 
 static void timeApplyZone() {
   setenv("TZ", ds.timezone, 1);
   tzset();
   configTzTime(ds.timezone, "pool.ntp.org", "time.google.com");
 }
 
 static bool timeIsSynced() {
   time_t now = time(nullptr);
   timeSynced = (now > 1700000000L);
   return timeSynced;
 }
 
 // ---- Prayer times (PrayTimes-style, Phase 1: calculate + display only) ----
 static double ptDeg2rad(double d) { return d * M_PI / 180.0; }
 static double ptRad2deg(double r) { return r * 180.0 / M_PI; }
 static double ptFixAngle(double a) {
   a = fmod(a, 360.0);
   if (a < 0) a += 360.0;
   return a;
 }
 static double ptFixHour(double a) {
   a = fmod(a, 24.0);
   if (a < 0) a += 24.0;
   return a;
 }
 static double ptDsin(double d) { return sin(ptDeg2rad(d)); }
 static double ptDcos(double d) { return cos(ptDeg2rad(d)); }
 static double ptDarcsin(double x) { return ptRad2deg(asin(x)); }
 static double ptDarccos(double x) {
   if (x < -1.0) x = -1.0;
   if (x > 1.0) x = 1.0;
   return ptRad2deg(acos(x));
 }
 
 static void ptSun(double jd, double& decl, double& eqt) {
   double d = jd - 2451545.0;
   double g = ptFixAngle(357.529 + 0.98560028 * d);
   double q = ptFixAngle(280.459 + 0.98564736 * d);
   double L = ptFixAngle(q + 1.915 * ptDsin(g) + 0.020 * ptDsin(2 * g));
   double e = 23.439 - 0.00000036 * d;
   decl = ptDarcsin(ptDsin(e) * ptDsin(L));
   double ra = ptRad2deg(atan2(ptDcos(e) * ptDsin(L), ptDcos(L))) / 15.0;
   ra = ptFixHour(ra);
   eqt = q / 15.0 - ra;
 }
 
 static double ptMidDay(double jd, double t) {
   double decl, eqt;
   ptSun(jd + t, decl, eqt);
   return ptFixHour(12.0 - eqt);
 }
 
 static double ptTimeAngle(double jd, double angle, double t, double lat) {
   double decl, eqt;
   ptSun(jd + t, decl, eqt);
   double z = ptFixHour(12.0 - eqt);
   double num = -ptDsin(angle) - ptDsin(decl) * ptDsin(lat);
   double den = ptDcos(decl) * ptDcos(lat);
   if (fabs(den) < 1e-9) return z;
   double c = num / den;
   if (c < -1.0 || c > 1.0) return NAN;
   double v = ptDarccos(c) / 15.0;
   return z + (angle > 90.0 ? -v : v);
 }
 
 static double ptAsr(double jd, double t, double lat, int step) {
   double decl, eqt;
   ptSun(jd + t, decl, eqt);
   double z = ptFixHour(12.0 - eqt);
   double tanArg = 1.0 / ((double)step + tan(ptDeg2rad(fabs(lat - decl))));
   double a = -ptRad2deg(atan(tanArg));
   double num = -ptDsin(a) - ptDsin(decl) * ptDsin(lat);
   double den = ptDcos(decl) * ptDcos(lat);
   if (fabs(den) < 1e-9) return z;
   double c = num / den;
   if (c < -1.0 || c > 1.0) return NAN;
   return z + ptDarccos(c) / 15.0;
 }
 
 static void ptNightPortions(double night, double fajrAngle, double ishaAngle,
                             double& fajrPortion, double& ishaPortion) {
   fajrPortion = night / 2.0;
   ishaPortion = night / 2.0;
   if (ds.highLat == HL_ONESEVENTH) {
     fajrPortion = night / 7.0;
     ishaPortion = night / 7.0;
   } else if (ds.highLat == HL_ANGLE) {
     fajrPortion = night * fajrAngle / 60.0;
     ishaPortion = night * ishaAngle / 60.0;
   }
 }
 
 static int ptHoursToMin(double h) {
   if (isnan(h)) return -1;
   h = ptFixHour(h);
   int m = (int)lround(h * 60.0);
   if (m < 0) m = 0;
   if (m >= 24 * 60) m = 24 * 60 - 1;
   return m;
 }
 
 // ESP32 Arduino struct tm has no tm_gmtoff — derive offset from local vs UTC.
 static double localUtcOffsetHours() {
   time_t now = time(nullptr);
   if (now < 1700000000L) return 0.0;
   struct tm localTm, utcTm;
   localtime_r(&now, &localTm);
   gmtime_r(&now, &utcTm);
   int localSec = localTm.tm_hour * 3600 + localTm.tm_min * 60 + localTm.tm_sec;
   int utcSec = utcTm.tm_hour * 3600 + utcTm.tm_min * 60 + utcTm.tm_sec;
   int diff = localSec - utcSec;
   if (localTm.tm_yday != utcTm.tm_yday || localTm.tm_year != utcTm.tm_year) {
     // Crossed midnight between the two representations.
     if (diff < -12 * 3600) diff += 24 * 3600;
     else if (diff > 12 * 3600) diff -= 24 * 3600;
     else if (localTm.tm_year > utcTm.tm_year ||
              (localTm.tm_year == utcTm.tm_year && localTm.tm_yday > utcTm.tm_yday))
       diff += 24 * 3600;
     else
       diff -= 24 * 3600;
   }
   return diff / 3600.0;
 }

 // Fills prayerMin[6] in local wall-clock minutes. Requires a synced clock.
 static void prayerComputeForLocalDay(const struct tm& lt) {
   int y = lt.tm_year + 1900, m = lt.tm_mon + 1, day = lt.tm_mday;
   if (m <= 2) { y -= 1; m += 12; }
   double A = floor(y / 100.0);
   double B = 2 - A + floor(A / 4.0);
   double jd = floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + day + B - 1524.5;
   jd = jd - ds.lon / (15.0 * 24.0);

   double tzHours = localUtcOffsetHours();
 
   double fajrA = 18, ishaA = 17;
   bool ishaInterval = false;
   int ishaMinAfter = 0;
   switch (ds.method) {
     case METH_ISNA: fajrA = 15; ishaA = 15; break;
     case METH_EGYPT: fajrA = 19.5; ishaA = 17.5; break;
     case METH_UMM: fajrA = 18.5; ishaInterval = true; ishaMinAfter = 90; break;
     case METH_KARACHI: fajrA = 18; ishaA = 18; break;
     default: fajrA = 18; ishaA = 17; break;
   }
   int asrStep = (ds.asr == ASR_HANAFI) ? 2 : 1;
 
   double t[PT_COUNT] = {5, 6, 12, 13, 18, 18};
   for (int iter = 0; iter < 3; iter++) {
     double p[PT_COUNT];
     for (int i = 0; i < PT_COUNT; i++) p[i] = t[i] / 24.0;
     t[PT_FAJR]    = ptTimeAngle(jd, 180.0 - fajrA, p[PT_FAJR], ds.lat);
     t[PT_SUNRISE] = ptTimeAngle(jd, 180.0 - 0.833, p[PT_SUNRISE], ds.lat);
     t[PT_DHUHR]   = ptMidDay(jd, p[PT_DHUHR]);
     t[PT_ASR]     = ptAsr(jd, p[PT_ASR], ds.lat, asrStep);
     t[PT_MAGHRIB] = ptTimeAngle(jd, 0.833, p[PT_MAGHRIB], ds.lat);
     if (ishaInterval) t[PT_ISHA] = t[PT_MAGHRIB] + ishaMinAfter / 60.0;
     else t[PT_ISHA] = ptTimeAngle(jd, ishaA, p[PT_ISHA], ds.lat);
   }
 
   for (int i = 0; i < PT_COUNT; i++) {
     if (!isnan(t[i])) t[i] = ptFixHour(t[i] + tzHours - ds.lon / 15.0);
   }
 
   double night = ptFixHour(t[PT_SUNRISE] - t[PT_MAGHRIB]);
   double fajrP, ishaP;
   ptNightPortions(night, fajrA, ishaA, fajrP, ishaP);
   if (ds.highLat != HL_NONE) {
     if (isnan(t[PT_FAJR]) || ptFixHour(t[PT_SUNRISE] - t[PT_FAJR]) > fajrP)
       t[PT_FAJR] = ptFixHour(t[PT_SUNRISE] - fajrP);
     if (!ishaInterval && (isnan(t[PT_ISHA]) || ptFixHour(t[PT_ISHA] - t[PT_MAGHRIB]) > ishaP))
       t[PT_ISHA] = ptFixHour(t[PT_MAGHRIB] + ishaP);
   }
 
   for (int i = 0; i < PT_COUNT; i++) {
     int mins = ptHoursToMin(t[i]);
     if (mins >= 0) mins += ds.adj[i];
     while (mins < 0) mins += 24 * 60;
     while (mins >= 24 * 60) mins -= 24 * 60;
     prayerMin[i] = mins;
   }
   prayerYday = lt.tm_yday;
 }
 
 static bool prayerEnsureToday() {
   if (!timeIsSynced()) return false;
   time_t now = time(nullptr);
   struct tm lt;
   localtime_r(&now, &lt);
   if (lt.tm_yday != prayerYday) prayerComputeForLocalDay(lt);
   return true;
 }
 
 static int prayerNowMinutes() {
   time_t now = time(nullptr);
   struct tm lt;
   localtime_r(&now, &lt);
   return lt.tm_hour * 60 + lt.tm_min;
 }
 
 // Next prayer among Fajr/Dhuhr/Asr/Maghrib/Isha (Sunrise excluded). -1 if none.
 static int prayerNextIndex() {
   if (!prayerEnsureToday()) return -1;
   static const int order[5] = {PT_FAJR, PT_DHUHR, PT_ASR, PT_MAGHRIB, PT_ISHA};
   int now = prayerNowMinutes();
   for (int i = 0; i < 5; i++) {
     int id = order[i];
     if (prayerMin[id] >= now) return id;
   }
   return PT_FAJR;   // next day's Fajr
 }
 
 static void fmtPrayerClock(int mins, char* out, size_t n) {
   if (mins < 0) { snprintf(out, n, "--:--"); return; }
   snprintf(out, n, "%02d:%02d", (mins / 60) % 24, mins % 60);
 }
 
 static const char* prayerName(int id) {
   static const char* const names[PT_COUNT] = {
     "Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha"
   };
   return (id >= 0 && id < PT_COUNT) ? names[id] : "?";
 }
 
 static int prayerBit(int id) {
   switch (id) {
     case PT_FAJR: return 0;
     case PT_DHUHR: return 1;
     case PT_ASR: return 2;
     case PT_MAGHRIB: return 3;
     case PT_ISHA: return 4;
     default: return -1;
   }
 }
 
 static bool prayerEnabled(int id) {
   int b = prayerBit(id);
   return b >= 0 && ds.prayerOn[b];
 }
 
 static void adhanLoadFired() {
   Preferences p;
   if (!p.begin("deen", true)) return;
   adhanFiredYday = p.getInt("afday", -1);
   adhanFiredMask = p.getUChar("afmask", 0);
   p.end();
 }
 
 static void adhanSaveFired() {
   Preferences p;
   if (!p.begin("deen", false)) return;
   p.putInt("afday", adhanFiredYday);
   p.putUChar("afmask", adhanFiredMask);
   p.end();
 }
 
 static void adhanMarkFired(int id) {
   int b = prayerBit(id);
   if (b < 0) return;
   if (!prayerEnsureToday()) return;
   time_t now = time(nullptr);
   struct tm lt; localtime_r(&now, &lt);
   if (adhanFiredYday != lt.tm_yday) {
     adhanFiredYday = lt.tm_yday;
     adhanFiredMask = 0;
   }
   adhanFiredMask |= (uint8_t)(1u << b);
   adhanSaveFired();
 }
 
 static bool adhanAlreadyFired(int id) {
   int b = prayerBit(id);
   if (b < 0) return true;
   if (!prayerEnsureToday()) return true;
   time_t now = time(nullptr);
   struct tm lt; localtime_r(&now, &lt);
   if (adhanFiredYday != lt.tm_yday) return false;
   return (adhanFiredMask & (1u << b)) != 0;
 }
 
 static bool adhanAiBusy() {
   lockSh();
   const AiShared& a = sh.ai;
   bool speaking = sh.active && sh.first.surah == 0;
   bool busy = a.open && (a.busy || a.recReady || speaking ||
                          a.phase == AI_RECORDING || a.phase == AI_LISTENED ||
                          a.phase == AI_THINKING || a.phase == AI_ANSWER);
   unlockSh();
   return busy;
 }
 
 static void adhanRequestBeeps() {
   lockSh();
   sh.beepLeft = 3;
   unlockSh();
 }
 
 // Periodic check-in: shows the device online and fetches remote settings.
 static void helloStep() {
   static uint32_t last = 0, wait = 3000, version = 0;
   lockSh();
   bool idle = !sh.active && !sh.ai.open;
   uint32_t gen = sh.ai.gen;
   unlockSh();
   if (!idle || WiFi.status() != WL_CONNECTED || millis() - last < wait) return;
   client.stop();
   helloRunning = true;
   int code = aiOpenRequest(gen, "/api/device/hello", "application/json", 2) && aiSend(gen, "{}", 2)
              ? aiReadReply(gen) : 0;
   helloRunning = false;
   aiClient.stop();
   last = millis();
   wait = (code == 200) ? HELLO_MS : 60000;
   if (code != 200) { LOGF("hello failed: %d\n", code); return; }
   uint32_t v = (uint32_t)jsonInt(aiBody, "\"config_version\"", 0);
   if (v == 0) v = 1;
   if (v == version && version != 0) return;     // apply only when changed
   version = v;
 
   DeviceSettings pending;
   dsDefaults(pending);
   pending.configVersion = v;
   pending.volume = jsonInt(aiBody, "\"volume\"", -1);
   pending.reciter = jsonInt(aiBody, "\"reciter\"", -1);
   pending.lat = jsonFloat(aiBody, "\"latitude\"", pending.lat);
   pending.lon = jsonFloat(aiBody, "\"longitude\"", pending.lon);
   char tmp[96];
   if (jsonFind(aiBody, "content_url", tmp, sizeof(tmp)) && tmp[0])
     snprintf(pending.contentUrl, sizeof(pending.contentUrl), "%s", tmp);
   if (jsonFind(aiBody, "timezone", tmp, sizeof(tmp)) && tmp[0])
     snprintf(pending.timezone, sizeof(pending.timezone), "%s", tmp);
   if (jsonFind(aiBody, "method", tmp, sizeof(tmp))) pending.method = methodFromName(tmp);
   if (jsonFind(aiBody, "asr", tmp, sizeof(tmp))) pending.asr = asrFromName(tmp);
   if (jsonFind(aiBody, "high_lat", tmp, sizeof(tmp))) pending.highLat = highLatFromName(tmp);
   pending.adj[PT_FAJR] = (int8_t)jsonInt(aiBody, "\"adj_fajr\"", 0);
   pending.adj[PT_SUNRISE] = (int8_t)jsonInt(aiBody, "\"adj_sunrise\"", 0);
   pending.adj[PT_DHUHR] = (int8_t)jsonInt(aiBody, "\"adj_dhuhr\"", 0);
   pending.adj[PT_ASR] = (int8_t)jsonInt(aiBody, "\"adj_asr\"", 0);
   pending.adj[PT_MAGHRIB] = (int8_t)jsonInt(aiBody, "\"adj_maghrib\"", 0);
   pending.adj[PT_ISHA] = (int8_t)jsonInt(aiBody, "\"adj_isha\"", 0);
   pending.prayerOn[0] = jsonInt(aiBody, "\"prayer_fajr\"", 1) != 0;
   pending.prayerOn[1] = jsonInt(aiBody, "\"prayer_dhuhr\"", 1) != 0;
   pending.prayerOn[2] = jsonInt(aiBody, "\"prayer_asr\"", 1) != 0;
   pending.prayerOn[3] = jsonInt(aiBody, "\"prayer_maghrib\"", 1) != 0;
   pending.prayerOn[4] = jsonInt(aiBody, "\"prayer_isha\"", 1) != 0;
 
   lockSh();
   cfgPending = pending;
   sh.cfgVolume = pending.volume;
   sh.cfgReciter = pending.reciter;
   sh.cfgNew = true;
   unlockSh();
 }
 
 // true if there was a recording to handle
 static bool aiNetStep() {
   lockSh();
   bool have = sh.ai.recReady;
   uint32_t gen = sh.ai.recGen;
   if (have) { sh.ai.recReady = false; sh.ai.busy = true; }
   unlockSh();
   if (!have) return false;
   if (!aiStale(gen)) aiAnswerQuestion(gen);
   lockSh(); bool speaking = sh.active && sh.first.surah == 0; unlockSh();
   if (!speaking) aiClient.stop();              // else reuse it to fetch the voice
   recFree();
   lockSh(); sh.ai.busy = false; unlockSh();
   return true;
 }
 
 static void netTask(void*) {
   uint32_t doneGen = 0;
   for (;;) {
     if (aiNetStep()) continue;
     helloStep();
 
     lockSh();
     bool wantDoc = sh.docWant && !sh.ai.open && !sh.active;
     uint32_t docGen = sh.docGen;
     unlockSh();
     if (wantDoc) {
       docFetchOnce(docGen);
       continue;
     }
 
     lockSh();
     uint32_t gen = sh.gen;
     bool aiOpen = sh.ai.open;
     bool run = sh.active && gen != doneGen;
     Clip c = sh.first;
     bool continuous = sh.continuous;
     unlockSh();
 
     if (aiOpen && client.connected()) client.stop();   // give the AI the memory
     if (!run) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
     doneGen = gen;
     // CONTENT_SURAH and Quran both use "client"; AI voice uses aiClient.
 
     for (;;) {
       bool aborted = false;
       PError err = fetchClip(gen, c, aborted);
       if (aborted) break;
       if (err != PE_NONE || !continuous) {
         lockSh();
         if (gen == sh.gen) {
           sh.netError = err;
           sh.errorClip = c;
           sh.netFinished = true;
         }
         unlockSh();
         break;
       }
       c = nextClip(c);
     }
     if (c.surah == 0) aiClient.stop();
     else client.stop();
   }
 }
 
 // =====================================================================
 //                    AUDIO TASK (ring -> MP3 -> I2S)
 // =====================================================================
 static uint8_t  inBuf[IN_SIZE + 64];
 static size_t   inLen = 0;
 static int16_t  pcm[1152 * 2];          // decoder output, then stereo I2S block
 static uint32_t i2sRate = 44100;
 static int32_t  curGain = 0;            // Q15
 static uint32_t clipRate = 0;           // locked by the first good frame
 static uint32_t clipSamples = 0;
 static uint32_t clipKbps = 0;
 
 static bool i2sInit() {
   i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
   cc.dma_desc_num = 8;
   cc.dma_frame_num = 384;                 // 12 KB total, ~70 ms at 44.1 kHz
   cc.auto_clear = true;                   // silence (not a loop) when we stop writing
   if (i2s_new_channel(&cc, &i2sTx, nullptr) != ESP_OK) return false;
 
   i2s_std_config_t sc = {};
   sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(i2sRate);
   sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
   sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
   sc.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
   sc.gpio_cfg.ws   = (gpio_num_t)I2S_LRC;
   sc.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
   sc.gpio_cfg.din  = I2S_GPIO_UNUSED;
   if (i2s_channel_init_std_mode(i2sTx, &sc) != ESP_OK) return false;
   return i2s_channel_enable(i2sTx) == ESP_OK;
 }
 
 static void i2sClose(i2s_chan_handle_t& h) {
   if (!h) return;
   i2s_channel_disable(h);
   i2s_del_channel(h);
   h = nullptr;
 }
 
 // The ESP32-C3 has one I2S unit: it is either the speaker or the microphone.
 static bool micInit() {
   i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
   cc.dma_desc_num = 6;
   cc.dma_frame_num = 256;
   if (i2s_new_channel(&cc, nullptr, &i2sRx) != ESP_OK) return false;
 
   i2s_std_config_t sc = {};
   sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE);
   sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
   sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
   sc.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
   sc.gpio_cfg.ws   = (gpio_num_t)I2S_LRC;
   sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
   sc.gpio_cfg.din  = (gpio_num_t)MIC_DIN;
   if (i2s_channel_init_std_mode(i2sRx, &sc) != ESP_OK) return false;
   gpio_set_pull_mode((gpio_num_t)MIC_DIN, GPIO_PULLDOWN_ONLY);   // unused slot reads 0
   return i2s_channel_enable(i2sRx) == ESP_OK;
 }
 
 static void i2sSetRate(uint32_t rate) {
   if (rate == i2sRate) return;
   i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
   i2s_channel_disable(i2sTx);
   i2s_channel_reconfig_std_clock(i2sTx, &clk);
   i2s_channel_enable(i2sTx);
   i2sRate = rate;
 }
 
 static void i2sWrite(const int16_t* d, size_t bytes) {
   const uint8_t* p = (const uint8_t*)d;
   while (bytes > 0) {
     size_t w = 0;
     esp_err_t e = i2s_channel_write(i2sTx, p, bytes, &w, pdMS_TO_TICKS(500));
     p += w;
     bytes -= w;
     if (e != ESP_OK && w == 0) break;
   }
 }
 
 // Top up the decoder window from the current clip only.
 static void refillInput(uint32_t gen) {
   lockSh();
   if (gen == sh.gen && sh.qCount > 0) {
     ClipEntry& e = sh.q[sh.qHead];
     uint32_t limit = e.done ? e.end : sh.wr;
     size_t have = (size_t)(limit - sh.rd);
     size_t room = IN_SIZE - inLen;
     size_t k = have < room ? have : room;
     if (k > 0) {
       size_t at = sh.rd % RING_SIZE;
       size_t first = (k < RING_SIZE - at) ? k : RING_SIZE - at;
       memcpy(inBuf + inLen, ringBuf + at, first);
       memcpy(inBuf + inLen + first, ringBuf, k - first);
       inLen += k;
       sh.rd += k;
     }
   }
   unlockSh();
 }
 
 static void dropInput(size_t n) {
   if (n > inLen) n = inLen;
   memmove(inBuf, inBuf + n, inLen - n);
   inLen -= n;
 }
 
 // Decode one MP3 frame and play it, ramping the gain toward "target".
 // false = not enough data for a whole frame.
 static bool playFrame(int32_t target) {
   while (inLen >= 8) {
     int off = MP3FindSyncWord(inBuf, (int)inLen);
     if (off < 0) { dropInput(inLen - 1); return false; }
     if (off > 0) { dropInput(off); continue; }
 
     unsigned char* ptr = inBuf;
     int left = (int)inLen;
     int err = MP3Decode(mp3, &ptr, &left, pcm, 0);
 
     if (err == ERR_MP3_INDATA_UNDERFLOW) {
       if (inLen >= IN_SIZE) { dropInput(1); continue; }
       return false;
     }
     size_t used = inLen - (size_t)left;
     if (err != ERR_MP3_NONE) {
       dropInput((err == ERR_MP3_MAINDATA_UNDERFLOW && used > 0) ? used : 1);
       continue;
     }
     dropInput(used > 0 ? used : 1);
 
     MP3FrameInfo fi;
     MP3GetLastFrameInfo(mp3, &fi);
     if (fi.outputSamps <= 0 || fi.nChans < 1 || fi.samprate < 8000) continue;
     if (clipRate == 0) { clipRate = fi.samprate; clipKbps = fi.bitrate / 1000; }
     else if ((uint32_t)fi.samprate != clipRate) continue;   // false sync inside junk
     i2sSetRate(clipRate);
 
     int frames = fi.outputSamps / fi.nChans;
     int32_t step = 32767 / (int32_t)(clipRate * FADE_MS / 1000) + 1;
     // mono mix, volume ramp, duplicate to L+R (in place)
     if (fi.nChans == 2)
       for (int i = 0; i < frames; i++) pcm[i] = (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2);
     for (int i = 0; i < frames; i++) {
       if (curGain < target)      { curGain += step; if (curGain > target) curGain = target; }
       else if (curGain > target) { curGain -= step; if (curGain < target) curGain = target; }
       pcm[i] = (int16_t)(((int32_t)pcm[i] * curGain) >> 15);
     }
     for (int i = frames - 1; i >= 0; i--) pcm[2 * i + 1] = pcm[2 * i] = pcm[i];
 
     i2sWrite(pcm, (size_t)frames * 4);
     clipSamples += frames;
     return true;
   }
   return false;
 }
 
 // Fade to zero using one more frame, so Stop / Pause / Next never click.
 static void fadeOut() {
   if (curGain > 0) playFrame(0);
   curGain = 0;
 }
 
 static void publish(uint32_t gen, PState state, const Clip* clip) {
   lockSh();
   if (gen == sh.gen) {
     sh.st.gen = gen;
     sh.st.state = state;
     if (clip) {
       sh.st.clip = *clip;
       float prog = 0.0f;
       uint32_t total = 0;
       if (sh.qCount > 0) {
         ClipEntry& e = sh.q[sh.qHead];
         if (e.len > 0) {
           uint32_t used = sh.rd - e.start;
           used = used > inLen ? used - inLen : 0;
           prog = (float)used / (float)e.len;
           if (prog > 1.0f) prog = 1.0f;
           if (clipKbps > 0) total = (uint32_t)((uint64_t)e.len * 8 / clipKbps);
         }
       }
       sh.st.progress = prog;
       sh.st.totalMs = total;
       sh.st.elapsedMs = clipRate ? (uint32_t)((uint64_t)clipSamples * 1000 / clipRate) : 0;
     }
   }
   unlockSh();
 }
 
 // ---------------- microphone ----------------
 static bool     micOn = false;
 static uint32_t micWarm = 0;
 static bool     recActive = false;   // the audio task owns an unfinished recording
 static uint32_t recGen = 0;
 static size_t   recGood = 0;         // bytes of complete FLAC blocks
 
 static void recFreeIfMine() {
   if (recActive) { recActive = false; recFree(); }
 }
 
 static void micStop() {
   if (!micOn) return;
   recFreeIfMine();
   i2sClose(i2sRx);
   micOn = false;
   lockSh(); sh.ai.micReady = false; unlockSh();
   while (!i2sInit()) { i2sClose(i2sTx); vTaskDelay(pdMS_TO_TICKS(200)); }
 }
 
 static void micStep() {
   static int32_t  raw[256 * 2];
   static int16_t  block[FLAC_BLOCK];
   static uint8_t  frame[2200];
   static size_t   fill = 0;
   static uint32_t frameNo = 0, held = 0, tail = 0;
   static int32_t  dc = 0;
   static int      peak = 0;
 
   if (!micOn) {
     static const int16_t quiet[256] = {};
     for (uint32_t i = 0; i < i2sRate / 10 / 128 + 1; i++) i2sWrite(quiet, sizeof(quiet));
     i2sClose(i2sTx);
     pinMode(I2S_DOUT, OUTPUT);              // amplifier data line low = silent speaker
     digitalWrite(I2S_DOUT, LOW);
     if (mp3) { MP3FreeDecoder(mp3); mp3 = nullptr; }   // more room for the AI
     if (!micInit()) {
       i2sClose(i2sRx);
       lockSh(); uint32_t g = sh.ai.gen; unlockSh();
       aiFail(g, "Mic start failed");
       vTaskDelay(pdMS_TO_TICKS(500));
       micOn = true;                          // so micStop() restores the speaker
       return;
     }
     micOn = true;
     micWarm = 0;
     dc = 0;
   }
   if (!i2sRx) { vTaskDelay(pdMS_TO_TICKS(20)); return; }
 
   size_t bytes = 0;
   i2s_channel_read(i2sRx, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(100));
   int n = bytes / 8;
   if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); return; }
 
   static int16_t mono[256];
   int blockPeak = 0;
   for (int i = 0; i < n; i++) {
     // only one slot carries the microphone, the other reads zero
     int32_t x = (raw[2 * i] >> MIC_SHIFT) + (raw[2 * i + 1] >> MIC_SHIFT);
     dc += (x - (dc >> 8));                   // remove the mic's DC offset
     int32_t y = x - (dc >> 8);
     if (y > 32767) y = 32767;
     if (y < -32768) y = -32768;
     mono[i] = (int16_t)y;
     int a = y < 0 ? -y : y;
     if (a > blockPeak) blockPeak = a;
   }
   if (micWarm < MIC_WARMUP) { micWarm += n; return; }
 
   lockSh();
   sh.ai.micReady = true;
   bool want = sh.ai.phase == AI_RECORDING;
   uint32_t gen = sh.ai.gen;
   unlockSh();
 
   if (!want) { recFreeIfMine(); return; }
 
   if (!recActive || recGen != gen) {         // a new question starts
     recFree();
     recActive = true;
     recGen = gen;
     fill = 0; frameNo = 0; held = 0; tail = 0; peak = 0; recGood = 0;
     if (!talkHeld) {                         // released before the mic was ready
       recActive = false;
       aiFail(gen, "Hold 5 and speak");
       return;
     }
   }
 
   if (talkHeld) { held += tail + n; tail = 0; }
   else tail += n;
   if (blockPeak > peak) peak = blockPeak;
 
   bool full = false;
   for (int i = 0; i < n && !full; i++) {
     block[fill++] = mono[i];
     if (fill == FLAC_BLOCK) {
       size_t len = flacFrame(block, frameNo, frame);
       lockSh();
       if (gen == sh.ai.gen) {
         if (recAppend(frame, len)) { rec.samples += FLAC_BLOCK; frameNo++; }
         else full = true;                    // memory limit: send what we have
       }
       unlockSh();
       if (full) rec.len = recGood;           // drop a half-stored block
       else recGood = rec.len;
       fill = 0;
       if (rec.samples + FLAC_BLOCK > REC_MAX) full = true;
     }
   }
 
   int level = blockPeak / 80;
   lockSh();
   if (gen == sh.ai.gen) {
     sh.ai.level = level > 100 ? 100 : level;
     sh.ai.recMs = (uint32_t)((uint64_t)(rec.samples + fill) * 1000 / MIC_RATE);
   }
   unlockSh();
 
   if (!full && (talkHeld || tail < REC_TAIL)) return;
 
   // ---- finished ----
   recActive = false;
   if (held < REC_MIN) { recFree(); aiFail(gen, "Hold 5 and speak"); return; }
   if (peak < REC_SILENT) { recFree(); aiFail(gen, "Mic silent: check it"); return; }
   if (fill > 0 && !full) {                   // pad and store the last block
     memset(block + fill, 0, (FLAC_BLOCK - fill) * sizeof(int16_t));
     size_t len = flacFrame(block, frameNo, frame);
     lockSh();
     if (gen == sh.ai.gen && recAppend(frame, len)) { rec.samples += FLAC_BLOCK; recGood = rec.len; }
     unlockSh();
     rec.len = recGood;
   }
   lockSh();
   if (gen == sh.ai.gen) {
     sh.ai.phase = AI_LISTENED;
     sh.ai.recGen = gen;
     sh.ai.recReady = true;                   // the network task owns it now
     unlockSh();
   } else {
     unlockSh();
     recFree();
   }
 }
 
 static void audioTask(void*) {
   static int16_t zeros[256];
   uint32_t gen = 0;
   bool haveClip = false;
   bool buffering = true;
   uint32_t gapLeft = 0;          // silence still to play (in sample frames)
   uint8_t lastSurah = 0;
   Clip cur = {1, 1, 0};
 
   for (;;) {
     lockSh();
     uint32_t g = sh.gen;
     bool active = sh.active;
     bool paused = sh.paused;
     float v = sh.volume;
     bool wantMic = sh.ai.open;
     unlockSh();
     int32_t volGain = (int32_t)(v * v * 32767.0f);     // natural-feeling volume curve
 
     if (g != gen) {                       // new session or Stop
       fadeOut();
       gen = g;
       inLen = 0;
       haveClip = false;
       buffering = true;
       gapLeft = 0;
       lastSurah = 0;
     }
     if (wantMic && !active) { micStep(); continue; }
     if (micOn) micStop();
     if (!active) {
       // Soft offline beeps (Adhan fallback). Never runs during AI mic or playback.
       lockSh(); uint8_t beeps = sh.beepLeft; unlockSh();
       if (beeps > 0 && i2sTx) {
         i2sSetRate(22050);
         const int toneN = 22050 * 120 / 1000;     // ~120 ms
         const int gapN  = 22050 * 120 / 1000;
         for (int i = 0; i < toneN; i++) {
           int16_t s = ((i / 12) & 1) ? 1800 : -1800;   // soft ~880 Hz
           pcm[0] = pcm[1] = s;
           i2sWrite(pcm, 4);
         }
         for (int i = 0; i < gapN; i++) {
           pcm[0] = pcm[1] = 0;
           i2sWrite(pcm, 4);
         }
         lockSh();
         if (sh.beepLeft > 0) sh.beepLeft--;
         unlockSh();
         continue;
       }
       vTaskDelay(pdMS_TO_TICKS(10));
       continue;
     }
 
     if (paused) {
       fadeOut();
       publish(gen, PS_PAUSED, nullptr);
       vTaskDelay(pdMS_TO_TICKS(10));
       continue;
     }
 
     if (!haveClip) {
       lockSh();
       bool have = sh.qCount > 0;
       Clip c = have ? sh.q[sh.qHead].clip : cur;
       bool finished = !have && sh.netFinished && gen == sh.gen;
       if (finished) {
         sh.active = false;
         sh.st.state = sh.netError ? PS_ERROR : PS_FINISHED;
         sh.st.error = sh.netError;
         if (sh.netError) sh.st.clip = sh.errorClip;
       }
       unlockSh();
       if (finished) { curGain = 0; continue; }
       if (!have) {
         publish(gen, PS_BUFFERING, nullptr);
         vTaskDelay(pdMS_TO_TICKS(10));
         continue;
       }
       cur = c;
       haveClip = true;
       inLen = 0;
       clipRate = 0;
       clipSamples = 0;
       clipKbps = 0;
       if (lastSurah) gapLeft = i2sRate * (lastSurah == cur.surah ? GAP_AYAH_MS : GAP_SURAH_MS) / 1000;
       lastSurah = cur.surah;
       // fresh decoder state for every file
       if (mp3) MP3FreeDecoder(mp3);
       mp3 = MP3InitDecoder();
     }
 
     if (!mp3) {                           // very unlikely: retry the allocation
       vTaskDelay(pdMS_TO_TICKS(50));
       mp3 = MP3InitDecoder();
       continue;
     }
 
     if (gapLeft > 0) {                    // pause between ayahs, still responsive
       uint32_t n = gapLeft < 128 ? gapLeft : 128;
       i2sWrite(zeros, n * 4);
       gapLeft -= n;
       continue;
     }
 
     lockSh();
     if (gen != sh.gen || sh.qCount == 0) { unlockSh(); continue; }
     bool done = sh.q[sh.qHead].done;
     size_t buffered = (size_t)(sh.wr - sh.rd);
     unlockSh();
 
     if (buffering) {
       if (!done && buffered + inLen < PREBUFFER) {
         publish(gen, PS_BUFFERING, &cur);
         vTaskDelay(pdMS_TO_TICKS(10));
         continue;
       }
       buffering = false;
     }
 
     refillInput(gen);
 
     lockSh();
     if (gen != sh.gen || sh.qCount == 0) { unlockSh(); continue; }
     done = sh.q[sh.qHead].done;
     uint32_t limit = done ? sh.q[sh.qHead].end : sh.wr;
     size_t unread = (size_t)(limit - sh.rd);
     unlockSh();
 
     bool runningDry = !done && (unread + inLen) < LOW_WATER;
     bool played = playFrame(runningDry ? 0 : volGain);
 
     if (runningDry) {
       curGain = 0;
       buffering = true;                   // wait for a healthy buffer again
       continue;
     }
 
     if (played) {
       publish(gen, PS_PLAYING, &cur);
     } else if (done && unread == 0) {     // clip finished
       publish(gen, PS_PLAYING, &cur);
       lockSh();
       if (gen == sh.gen && sh.qCount > 0) {
         sh.rd = sh.q[sh.qHead].end;
         sh.qHead = (sh.qHead + 1) % CLIP_SLOTS;
         sh.qCount--;
       }
       unlockSh();
       haveClip = false;
     } else {
       vTaskDelay(1);
     }
   }
 }
 
 // =====================================================================
 //                            UI STATE
 // =====================================================================
 Screen screen = SCR_HOME;
 Screen prevScreen = SCR_HOME;
 int homeSel = 0;   // 0 Quran .. 6 Settings
 int homeTop = 0;   // first visible home row
 int setSel = 0;    // 0 Reciter, 1 Volume
 static const int HOME_COUNT = 7;
 static const int HOME_VISIBLE = 4;
 static const char* const HOME_LABELS[7] = {
   "Quran", "Ustaz", "Adhan", "Huruf", "Hadith", "Q&A", "Settings"
 };
 
 PlayMode playMode = MODE_SURAH;
 
 int reciterIdx = 0;
 int surah = 1;
 int ayah = 1;
 float volLevel = 0.70f;  // 0.05 .. 1.00
 
 bool session = false;        // a track is loading, playing or paused
 bool paused = false;
 bool stepPending = false;    // Next/Prev pressed, waiting before loading
 uint32_t stepAt = 0;
 uint32_t playGen = 0;
 Status ui = {};              // latest player status
 const char* status = "Ready";
 const char* bootReason = "?";
 
 BootPhase bootPhase = BOOT_WIFI;
 uint32_t bootStart = 0;
 bool dirty = true;
 
 // AI page
 Screen aiReturn = SCR_HOME, aiReturnPrev = SCR_HOME;
 AiPhase aiPhase = AI_IDLE;
 bool aiReady = false, aiBusy = false;
 uint8_t aiLevel = 0;
 uint32_t aiRecMs = 0, aiShownId = 0;
 char aiError[24] = "";
 static const int AI_COLS = 25, AI_ROWS = 5, AI_MAXLINES = 90;
 char aiLines[AI_MAXLINES][AI_COLS + 1];
 int aiLineCount = 0, aiScroll = 0;
 
 // =====================================================================
 //                              UI DRAWING
 //   Display 128x64, text drawn with setFontPosTop() (y = top of text)
 //   Layout:  0-9 status bar | 10 line | 12-53 content | 54 line | 56-62 footer
 // =====================================================================
 
 static void txtC(int y, const char* s) {
   u8g2.drawStr((128 - u8g2.getStrWidth(s)) / 2, y, s);
 }
 
 // ---------------- Wi-Fi setup from a phone ----------------
 static String htmlSafe(String t) {
   t.replace("&", "&amp;"); t.replace("<", "&lt;"); t.replace(">", "&gt;"); t.replace("\"", "&quot;");
   return t;
 }
 
 // Opens a hotspot with a sign-in page. Never returns: it restarts the device.
 static void runWifiSetup(bool haveSaved) {
   static WebServer web(80);
   static DNSServer dns;
   static String page;
 
   auto screenText = [](const char* a, const char* b, const char* c, const char* d) {
     u8g2.clearBuffer();
     u8g2.setFont(u8g2_font_6x10_tf);
     txtC(11, a); txtC(28, b); txtC(43, c); txtC(60, d);
     u8g2.sendBuffer();
   };
   screenText("Wi-Fi Setup", "Searching...", "", "");
 
   WiFi.disconnect();
   WiFi.mode(WIFI_STA);
   int found = WiFi.scanNetworks();
   page = F("<!doctype html><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'><title>Quran Player</title>"
            "<style>body{font-family:sans-serif;max-width:360px;margin:auto;padding:20px}"
            "input,select,button{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;font-size:16px}</style>"
            "<h2>Quran Player Wi-Fi</h2><form method=post action=/save>Choose your Wi-Fi<select name=s>");
   for (int i = 0; i < found && i < 20; i++)
     if (WiFi.SSID(i).length()) page += "<option>" + htmlSafe(WiFi.SSID(i)) + "</option>";
   page += F("</select><input name=o maxlength=32 placeholder='Or type a hidden network name'>"
             "<input name=p type=password maxlength=64 placeholder='Wi-Fi password'>"
             "<button>Connect</button></form><p>Only 2.4 GHz Wi-Fi works.</p>");
   WiFi.scanDelete();
 
   WiFi.mode(WIFI_AP);
   WiFi.softAP(SETUP_AP_NAME);
   delay(100);
   dns.start(53, "*", WiFi.softAPIP());     // makes the phone open the page by itself
 
   web.on("/", []() { web.send(200, "text/html", page); });
   web.on("/save", HTTP_POST, []() {
     String name = web.arg("o").length() ? web.arg("o") : web.arg("s");
     if (!name.length() || name.length() > 32 || web.arg("p").length() > 64) {
       web.send(400, "text/plain", "Check the Wi-Fi name and password, then go back.");
       return;
     }
     Preferences prefs;
     prefs.begin("wifi", false);
     prefs.putString("ssid", name);
     prefs.putString("pass", web.arg("p"));
     prefs.end();
     web.send(200, "text/html", "<meta name=viewport content='width=device-width'><h3>Saved. Connecting...</h3>"
              "<p>If the password is wrong, the Quran Player Wi-Fi appears again in about 30 seconds.</p>");
     delay(1500);
     ESP.restart();
   });
   web.onNotFound([]() {
     web.sendHeader("Location", "http://192.168.4.1/");
     web.send(302, "text/plain", "");
   });
   web.begin();
   screenText("Wi-Fi Setup", "On phone join Wi-Fi:", SETUP_AP_NAME, "or open 192.168.4.1");
 
   uint32_t quietSince = millis();
   for (;;) {
     dns.processNextRequest();
     web.handleClient();
     if (WiFi.softAPgetStationNum() > 0) quietSince = millis();
     // router may simply have been slow after a power cut: try it again
     if (haveSaved && millis() - quietSince > 180000) ESP.restart();
     delay(2);
   }
 }
 
 static void fmtTime(uint32_t ms, char* o, size_t n) {
   unsigned long s = ms / 1000UL;
   if (s >= 3600UL) snprintf(o, n, "%lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
   else             snprintf(o, n, "%02lu:%02lu", s / 60UL, s % 60UL);
 }
 
 enum { ICON_BOOK = 0, ICON_AYAH = 1, ICON_GEAR = 2 };
 
 // small icons, about 12 x 7 px, (x,y) = top-left
 static void drawIcon(int id, int x, int y) {
   if (id == ICON_BOOK || id == ICON_AYAH) {
     u8g2.drawFrame(x, y, 6, 7);
     u8g2.drawFrame(x + 6, y, 6, 7);
     if (id == ICON_AYAH) {
       u8g2.drawHLine(x + 1, y + 2, 4);
       u8g2.drawHLine(x + 1, y + 4, 4);
       u8g2.drawHLine(x + 7, y + 2, 4);
       u8g2.drawHLine(x + 7, y + 4, 4);
     }
   } else {
     int cx = x + 5, cy = y + 3;
     u8g2.drawCircle(cx, cy, 2);
     u8g2.drawPixel(cx, cy - 3); u8g2.drawPixel(cx, cy + 3);
     u8g2.drawPixel(cx - 3, cy); u8g2.drawPixel(cx + 3, cy);
   }
 }
 
 // mosque logo, 15 x 9 px
 static void drawMosque(int x, int y) {
   u8g2.drawVLine(x, y + 2, 7);
   u8g2.drawVLine(x + 14, y + 2, 7);
   u8g2.drawPixel(x, y + 1);
   u8g2.drawPixel(x + 14, y + 1);
   u8g2.drawCircle(x + 7, y + 6, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
   u8g2.drawVLine(x + 3, y + 6, 3);
   u8g2.drawVLine(x + 11, y + 6, 3);
   u8g2.drawHLine(x, y + 8, 15);
   u8g2.drawPixel(x + 7, y);
   u8g2.drawPixel(x + 7, y + 1);
 }
 
 static void drawArrow(int y) {
   u8g2.drawTriangle(118, y + 2, 118, y + 7, 122, y + 4);
 }
 
 void drawStatusBar() {
   u8g2.setFont(u8g2_font_6x10_tr);
 
   char tb[8];
   if (timeIsSynced()) {
     time_t now = time(nullptr);
     struct tm lt;
     localtime_r(&now, &lt);
     snprintf(tb, sizeof(tb), "%02d:%02d", lt.tm_hour, lt.tm_min);
   } else {
     snprintf(tb, sizeof(tb), "--:--");
   }
   u8g2.drawStr(0, 0, tb);
 
   // wifi bars
   int bars = 0;
   if (WiFi.status() == WL_CONNECTED) {
     int r = WiFi.RSSI();
     bars = (r > -55) ? 4 : (r > -67) ? 3 : (r > -78) ? 2 : 1;
   }
   int wx = 128 - 15;
   for (int i = 0; i < 4; i++) {
     int h = 2 + i * 2;
     if (i < bars) u8g2.drawBox(wx + i * 4, 8 - h, 3, h);
     else u8g2.drawPixel(wx + i * 4 + 1, 7);
   }
 
   u8g2.drawHLine(0, 10, 128);
 }
 
 void drawFooter(const char* left, const char* mid, const char* right) {
   u8g2.drawHLine(0, 54, 128);
   u8g2.setFont(u8g2_font_5x7_tr);
   if (left)  u8g2.drawStr(0, 56, left);
   if (mid)   txtC(56, mid);
   if (right) u8g2.drawStr(128 - u8g2.getStrWidth(right), 56, right);
 }
 
 // ---------------- BOOT SPLASH ----------------
 void drawBootSplash() {
   drawStatusBar();
   drawMosque(12, 14);
   u8g2.setFont(u8g2_font_6x10_tr);
   u8g2.drawStr(31, 14, "Quran Player");
   txtC(30, status);
   char rb[28];
   snprintf(rb, sizeof(rb), "Reset: %s", bootReason);
   u8g2.setFont(u8g2_font_5x7_tr);
   txtC(44, rb);
   drawFooter(nullptr, "Back = skip", nullptr);
 }
 
 // ---------------- HOME (scrolling menu) ----------------
 void drawHome() {
   drawStatusBar();
   u8g2.setFont(u8g2_font_6x10_tr);
   if (homeSel < homeTop) homeTop = homeSel;
   if (homeSel >= homeTop + HOME_VISIBLE) homeTop = homeSel - HOME_VISIBLE + 1;
   for (int i = 0; i < HOME_VISIBLE; i++) {
     int idx = homeTop + i;
     if (idx >= HOME_COUNT) break;
     int y = 12 + i * 10;
     if (idx == homeSel) u8g2.drawRFrame(0, y, 128, 10, 3);
     u8g2.drawStr(6, y + 1, HOME_LABELS[idx]);
     if (idx == homeSel) drawArrow(y);
   }
   drawFooter("1/2 Move", "5 AI", "OK Select");
 }
 
 // ---------------- ADHAN ----------------
 void drawAdhan() {
   drawStatusBar();
   if (adhanAlert) {
     char line[28];
     u8g2.setFont(u8g2_font_6x10_tr);
     txtC(16, "Time for");
     snprintf(line, sizeof(line), "%s", prayerName(adhanWhich));
     txtC(30, line);
     if (adhanRun == AR_PLAYING) txtC(44, status);
     else if (adhanRun == AR_BEEP) txtC(44, "Beep...");
     else txtC(44, "Stopped");
     if (adhanRun == AR_HOLD)
       drawFooter("<Home", adhanCanResume ? "OK Resume" : nullptr, "5 AI");
     else
       drawFooter("4 Stop", nullptr, "5 AI");
     return;
   }
   u8g2.setFont(u8g2_font_5x7_tr);
   if (!prayerEnsureToday()) {
     u8g2.setFont(u8g2_font_6x10_tr);
     txtC(28, "Time not set");
     drawFooter("<Back", nullptr, "5 AI");
     return;
   }
   int next = prayerNextIndex();
   if (adhanScroll < 0) adhanScroll = 0;
   if (adhanScroll > 2) adhanScroll = 2;
   for (int i = 0; i < 4; i++) {
     int id = adhanScroll + i;
     if (id >= PT_COUNT) break;
     int y = 12 + i * 10;
     char line[28], clock[8];
     fmtPrayerClock(prayerMin[id], clock, sizeof(clock));
     bool enabled = (id == PT_SUNRISE) ? false : prayerEnabled(id);
     if (id == next) u8g2.drawRFrame(0, y, 128, 10, 3);
     snprintf(line, sizeof(line), "%s %s%s", prayerName(id), clock, enabled ? "" : " off");
     u8g2.setFont(u8g2_font_6x10_tr);
     u8g2.drawStr(2, y + 1, line);
   }
   char mid[20] = "";
   if (next >= 0) {
     char clock[8];
     fmtPrayerClock(prayerMin[next], clock, sizeof(clock));
     snprintf(mid, sizeof(mid), "Next %s", clock);
   }
   drawFooter("<Back", mid, "1/2 Scroll");
 }
 
 // ---- content library UI (Huruf / Hadith / Q&A) ----
 static int hurufIdx = 0;
 static bool contentPlaying = false;
 
 enum LibPhase : uint8_t { LP_LOAD = 0, LP_READY = 1, LP_EMPTY = 2, LP_ERR = 3 };
 static LibPhase hadithPhase = LP_LOAD;
 static int hadithCount = 0, hadithIdx = 0, hadithScroll = 0;
 static uint16_t hadithNums[64];
 static char hadithTitle[64], hadithSource[160];
 static char hadithText[1504];
 static int hadithLines = 0;
 static char hadithLine[40][26];
 
 enum QaMode : uint8_t { QA_TOPICS = 0, QA_ITEM = 1 };
 static QaMode qaMode = QA_TOPICS;
 static LibPhase qaPhase = LP_LOAD;
 static int qaTopicCount = 0, qaTopicSel = 0, qaItemCount = 0, qaItemIdx = 0, qaScroll = 0;
 static char qaTopicId[16][24];
 static char qaTopicName[16][28];
 static int qaTopicCounts[16];
 static char qaQuestion[304], qaAnswer[1504], qaSource[160];
 static int qaLines = 0;
 static char qaLine[48][26];
 
 static void wrapLines(const char* text, char lines[][26], int maxLines, int* outCount) {
   *outCount = 0;
   while (*text && *outCount < maxLines) {
     int len = 0, cut = -1;
     while (text[len] && text[len] != '\n' && len < 25) {
       if (text[len] == ' ') cut = len;
       len++;
     }
     if (len == 25 && text[len] && text[len] != ' ' && text[len] != '\n' && cut > 0) len = cut;
     memcpy(lines[*outCount], text, len);
     lines[*outCount][len] = 0;
     (*outCount)++;
     text += len;
     while (*text == ' ') text++;
     if (*text == '\n') text++;
   }
 }
 
 static void startContentMp3(const char* rel) {
   if (session) stopPlayback("Stopped");
   lockSh();
   snprintf(sh.contentPath, sizeof(sh.contentPath), "%s", rel);
   unlockSh();
   Clip c = { CONTENT_SURAH, 1, 0 };
   playGen = playerStart(c, false);
   session = true;
   paused = false;
   stepPending = false;
   contentPlaying = true;
   status = "Loading...";
   dirty = true;
 }
 
 static void stopContentAudio() {
   contentPlaying = false;
   if (session) {
     Status s = playerStatus();
     if (s.clip.surah == CONTENT_SURAH) stopPlayback("Ready");
   }
 }
 
 static void hurufOpen() {
   hurufIdx = 0;
   stopContentAudio();
 }
 
 static void hadithOpen() {
   hadithPhase = LP_LOAD;
   hadithCount = hadithIdx = hadithScroll = 0;
   hadithTitle[0] = hadithText[0] = hadithSource[0] = 0;
   hadithLines = 0;
   stopContentAudio();
   char path[72];
   snprintf(path, sizeof(path), "hadith/index.json?v=%lu", (unsigned long)contentVerHadith);
   docRequest(path);
 }
 
 static void hadithLoadItem() {
   if (hadithCount <= 0) { hadithPhase = LP_EMPTY; return; }
   if (hadithIdx < 0) hadithIdx = 0;
   if (hadithIdx >= hadithCount) hadithIdx = hadithCount - 1;
   hadithPhase = LP_LOAD;
   hadithScroll = 0;
   stopContentAudio();
   char path[72];
   uint16_t num = (hadithIdx >= 0 && hadithIdx < hadithCount) ? hadithNums[hadithIdx] : (uint16_t)(hadithIdx + 1);
   snprintf(path, sizeof(path), "hadith/%03u.json?v=%lu", (unsigned)num, (unsigned long)contentVerHadith);
   docRequest(path);
 }
 
 static void qaOpen() {
   qaMode = QA_TOPICS;
   qaPhase = LP_LOAD;
   qaTopicCount = qaTopicSel = qaItemCount = qaItemIdx = qaScroll = 0;
   stopContentAudio();
   char path[72];
   snprintf(path, sizeof(path), "qa/index.json?v=%lu", (unsigned long)contentVerQa);
   docRequest(path);
 }
 
 static void qaLoadItem() {
   if (qaItemCount <= 0) { qaPhase = LP_EMPTY; return; }
   if (qaItemIdx < 0) qaItemIdx = 0;
   if (qaItemIdx >= qaItemCount) qaItemIdx = qaItemCount - 1;
   qaPhase = LP_LOAD;
   qaScroll = 0;
   stopContentAudio();
   char path[80];
   snprintf(path, sizeof(path), "qa/%s/%03d.json?v=%lu",
            qaTopicId[qaTopicSel], qaItemIdx + 1, (unsigned long)contentVerQa);
   docRequest(path);
 }
 
 static void contentPollDocs() {
   lockSh();
   bool done = sh.docDone;
   bool ok = sh.docOk;
   uint8_t err = sh.docErr;
   static char buf[3200];
   if (done) memcpy(buf, sh.docBuf, sizeof(buf));
   sh.docDone = false;
   unlockSh();
   if (!done) return;
 
   if (screen == SCR_HADITH) {
     if (!ok) {
       hadithPhase = (err == DOC_WIFI) ? LP_ERR : (err == DOC_NOTFOUND ? LP_EMPTY : LP_ERR);
       status = (err == DOC_WIFI) ? "No Wi-Fi" : (err == DOC_NOTFOUND) ? "Not found" : "Error";
       dirty = true;
       return;
     }
     if (strstr(buf, "\"count\"") && strstr(buf, "\"items\"")) {
       hadithCount = jsonInt(buf, "\"count\"", 0);
       contentVerHadith = (uint32_t)jsonInt(buf, "\"version\"", (int)contentVerHadith);
       hadithCount = 0;
       const char* p = buf;
       while (hadithCount < 64 && (p = strstr(p, "\"number\"")) != nullptr) {
         int num = jsonInt(p, "\"number\"", 0);
         if (num > 0) hadithNums[hadithCount++] = (uint16_t)num;
         p += 8;
       }
       if (hadithCount <= 0) { hadithPhase = LP_EMPTY; dirty = true; return; }
       hadithIdx = 0;
       hadithLoadItem();
       dirty = true;
       return;
     }
     jsonFind(buf, "title", hadithTitle, sizeof(hadithTitle));
     jsonFind(buf, "text", hadithText, sizeof(hadithText));
     jsonFind(buf, "source", hadithSource, sizeof(hadithSource));
     wrapLines(hadithText, hadithLine, 40, &hadithLines);
     hadithPhase = LP_READY;
     dirty = true;
     return;
   }
 
   if (screen == SCR_QA) {
     if (!ok) {
       qaPhase = (err == DOC_WIFI) ? LP_ERR : (err == DOC_NOTFOUND ? LP_EMPTY : LP_ERR);
       status = (err == DOC_WIFI) ? "No Wi-Fi" : (err == DOC_NOTFOUND) ? "Not found" : "Error";
       dirty = true;
       return;
     }
     if (qaMode == QA_TOPICS && strstr(buf, "\"topics\"")) {
       contentVerQa = (uint32_t)jsonInt(buf, "\"version\"", (int)contentVerQa);
       qaTopicCount = 0;
       const char* p = buf;
       while (qaTopicCount < 16 && (p = strstr(p, "\"id\"")) != nullptr) {
         char id[24], name[28];
         if (!jsonFind(p, "id", id, sizeof(id))) { p += 4; continue; }
         if (!jsonFind(p, "name", name, sizeof(name))) name[0] = 0;
         qaTopicCounts[qaTopicCount] = jsonInt(p, "\"count\"", 0);
         snprintf(qaTopicId[qaTopicCount], sizeof(qaTopicId[0]), "%s", id);
         snprintf(qaTopicName[qaTopicCount], sizeof(qaTopicName[0]), "%s", name[0] ? name : id);
         qaTopicCount++;
         p += 4;
       }
       if (qaTopicCount <= 0) { qaPhase = LP_EMPTY; dirty = true; return; }
       qaTopicSel = 0;
       qaPhase = LP_READY;
       dirty = true;
       return;
     }
     jsonFind(buf, "question", qaQuestion, sizeof(qaQuestion));
     jsonFind(buf, "answer", qaAnswer, sizeof(qaAnswer));
     jsonFind(buf, "source", qaSource, sizeof(qaSource));
     char combined[1808];
     snprintf(combined, sizeof(combined), "Q: %s\n%s", qaQuestion, qaAnswer);
     wrapLines(combined, qaLine, 48, &qaLines);
     qaPhase = LP_READY;
     dirty = true;
   }
 }
 
 void drawHuruf() {
   drawStatusBar();
   if (hurufIdx < 0) hurufIdx = 0;
   if (hurufIdx >= HURUF_COUNT) hurufIdx = HURUF_COUNT - 1;
   int x = (128 - HURUF_W) / 2;
   u8g2.drawXBM(x, 12, HURUF_W, HURUF_H, HURUF_BITS[hurufIdx]);
   char mid[20];
   snprintf(mid, sizeof(mid), "%02d %s", hurufIdx + 1, HURUF_NAMES[hurufIdx]);
   drawFooter("1/2 Letter", mid, contentPlaying ? "OK Stop" : "OK Play");
 }
 
 void drawHadith() {
   drawStatusBar();
   u8g2.setFont(u8g2_font_6x10_tr);
   if (hadithPhase == LP_LOAD) { txtC(28, "Loading..."); drawFooter("<Back", nullptr, "5 AI"); return; }
   if (hadithPhase == LP_EMPTY) { txtC(28, "Empty"); drawFooter("<Back", nullptr, "5 AI"); return; }
   if (hadithPhase == LP_ERR) { txtC(28, status); drawFooter("<Back", nullptr, "5 AI"); return; }
   char head[32];
   snprintf(head, sizeof(head), "#%d %s", hadithIdx + 1, hadithTitle);
   u8g2.setFont(u8g2_font_5x7_tr);
   u8g2.drawStr(0, 12, head);
   for (int i = 0; i < 3; i++) {
     int li = hadithScroll + i;
     if (li >= hadithLines) break;
     u8g2.drawStr(0, 22 + i * 8, hadithLine[li]);
   }
   if (hadithSource[0]) {
     char src[28];
     snprintf(src, sizeof(src), "%.24s", hadithSource);
     u8g2.drawStr(0, 46, src);
   }
   drawFooter("1/2 Scroll", contentPlaying ? "OK Stop" : "OK Play", "5 AI");
 }
 
 void drawQA() {
   drawStatusBar();
   u8g2.setFont(u8g2_font_6x10_tr);
   if (qaPhase == LP_LOAD) { txtC(28, "Loading..."); drawFooter("<Back", nullptr, "5 AI"); return; }
   if (qaPhase == LP_EMPTY) { txtC(28, "Empty"); drawFooter("<Back", nullptr, "5 AI"); return; }
   if (qaPhase == LP_ERR) { txtC(28, status); drawFooter("<Back", nullptr, "5 AI"); return; }
   if (qaMode == QA_TOPICS) {
     if (qaTopicSel < 0) qaTopicSel = 0;
     if (qaTopicSel >= qaTopicCount) qaTopicSel = qaTopicCount - 1;
     int top = qaTopicSel - 1;
     if (top < 0) top = 0;
     if (top > qaTopicCount - 3) top = qaTopicCount - 3;
     if (top < 0) top = 0;
     for (int i = 0; i < 3; i++) {
       int idx = top + i;
       if (idx >= qaTopicCount) break;
       int y = 14 + i * 12;
       if (idx == qaTopicSel) u8g2.drawRFrame(0, y, 128, 11, 2);
       u8g2.drawStr(4, y + 2, qaTopicName[idx]);
     }
     drawFooter("1/2 Topic", "OK Open", "5 AI");
     return;
   }
   u8g2.setFont(u8g2_font_5x7_tr);
   char head[28];
   snprintf(head, sizeof(head), "%s %d/%d", qaTopicName[qaTopicSel], qaItemIdx + 1, qaItemCount);
   u8g2.drawStr(0, 12, head);
   for (int i = 0; i < 3; i++) {
     int li = qaScroll + i;
     if (li >= qaLines) break;
     u8g2.drawStr(0, 22 + i * 8, qaLine[li]);
   }
   if (qaSource[0]) {
     char src[28];
     snprintf(src, sizeof(src), "%.24s", qaSource);
     u8g2.drawStr(0, 46, src);
   }
   drawFooter("1/2 Scroll", contentPlaying ? "OK Stop" : "OK Play", "5 AI");
 }
 
 // ---------------- PLAYER ----------------
 static void drawPrevIcon(int cx) {
   u8g2.drawBox(cx - 5, 44, 2, 7);
   u8g2.drawTriangle(cx - 3, 47, cx + 1, 44, cx + 1, 50);
   u8g2.drawTriangle(cx + 1, 47, cx + 5, 44, cx + 5, 50);
 }
 static void drawNextIcon(int cx) {
   u8g2.drawBox(cx + 4, 44, 2, 7);
   u8g2.drawTriangle(cx + 3, 47, cx - 1, 44, cx - 1, 50);
   u8g2.drawTriangle(cx - 1, 47, cx - 5, 44, cx - 5, 50);
 }
 
 void drawPlayer() {
   char buf[32];
   drawStatusBar();
 
   u8g2.setFont(u8g2_font_6x10_tr);
   snprintf(buf, sizeof(buf), "Surah %s", NAMES[surah - 1]);
   txtC(12, buf);
   u8g2.setFont(u8g2_font_5x7_tr);
   snprintf(buf, sizeof(buf), "(Ayah %d / %d)", ayah, AYAHS[surah - 1]);
   txtC(22, buf);
 
   // only show progress that belongs to what is on screen
   bool live = session && !stepPending && ui.gen == playGen &&
               ui.clip.surah == surah && ui.clip.ayah == ayah;
 
   u8g2.drawRFrame(2, 29, 124, 5, 2);
   int fw = live ? (int)(ui.progress * 120.0f) : 0;
   if (fw > 0) u8g2.drawBox(4, 30, fw, 3);
 
   char e[12], tt[12];
   fmtTime(live ? ui.elapsedMs : 0, e, sizeof(e));
   if (live && ui.totalMs > 0) fmtTime(ui.totalMs, tt, sizeof(tt));
   else strcpy(tt, "--:--");
   snprintf(buf, sizeof(buf), "%s / %s", e, tt);
   u8g2.setFont(u8g2_font_5x7_tr);
   txtC(35, buf);
 
   // controls: prev | play/pause (highlighted) | next
   u8g2.drawRFrame(20, 42, 24, 11, 3);
   u8g2.drawRFrame(52, 42, 24, 11, 3);
   u8g2.drawRFrame(53, 43, 22, 9, 2);
   u8g2.drawRFrame(84, 42, 24, 11, 3);
   drawPrevIcon(32);
   drawNextIcon(96);
   if (session && !paused) {           // pause icon
     u8g2.drawBox(61, 44, 2, 7);
     u8g2.drawBox(65, 44, 2, 7);
   } else {                            // play icon
     u8g2.drawTriangle(61, 44, 61, 50, 67, 47);
   }
 
   const char* st = status;
   if (session && !paused && !strcmp(status, "Playing")) st = "Now Playing...";
   drawFooter("<Back", st, "5 AI");
 }
 
 // ---------------- AYAHS ----------------
 void drawAyahs() {
   char buf[32];
   drawStatusBar();
   drawIcon(ICON_BOOK, 2, 12);
   u8g2.setFont(u8g2_font_6x10_tr);
   snprintf(buf, sizeof(buf), "Surah %s", NAMES[surah - 1]);
   u8g2.drawStr(18, 12, buf);
 
   int total = AYAHS[surah - 1];
   int start = ayah - 1;
   if (start > total - 2) start = total - 2;
   if (start < 1) start = 1;
 
   for (int i = 0; i < 3; i++) {
     int a = start + i;
     if (a > total) break;
     int y = 22 + i * 10;
     bool sel = (a == ayah);
     if (sel) u8g2.drawRFrame(0, y, 128, 10, 3);
     snprintf(buf, sizeof(buf), "Ayah %d", a);
     u8g2.setFont(u8g2_font_6x10_tr);
     u8g2.drawStr(5, y + 1, buf);
     if (sel) drawArrow(y);
   }
   snprintf(buf, sizeof(buf), "%d / %d", ayah, total);
   drawFooter("< Back", buf, "OK Play");
 }
 
 // ---------------- SETTINGS ----------------
 void drawSettings() {
   char buf[24];
   drawStatusBar();
   drawIcon(ICON_GEAR, 2, 12);
   u8g2.setFont(u8g2_font_6x10_tr);
   u8g2.drawStr(18, 12, "Settings");
 
   int y = 22;
   if (setSel == 0) u8g2.drawRFrame(0, y, 128, 10, 3);
   u8g2.drawStr(5, y + 1, "Reciter");
   snprintf(buf, sizeof(buf), "< %s >", RECITERS[reciterIdx].name);
   u8g2.drawStr(123 - u8g2.getStrWidth(buf), y + 1, buf);
 
   y = 33;
   if (setSel == 1) u8g2.drawRFrame(0, y, 128, 10, 3);
   int pct = (int)(volLevel * 100.0f + 0.5f);
   u8g2.drawStr(5, y + 1, "Volume");
   u8g2.drawFrame(50, y + 2, 48, 6);
   int w = (pct * 44) / 100;
   if (w > 0) u8g2.drawBox(52, y + 4, w, 2);
   snprintf(buf, sizeof(buf), "%d%%", pct);
   u8g2.drawStr(123 - u8g2.getStrWidth(buf), y + 1, buf);
 
   y = 44;
   u8g2.drawStr(5, y + 1, "WiFi");
   const char* ws = (WiFi.status() == WL_CONNECTED) ? "Online" : "Offline";
   u8g2.drawStr(123 - u8g2.getStrWidth(ws), y + 1, ws);
 
   drawFooter("<Back", "1/2 change", "OK Row");
 }
 
 // ---------------- AI ----------------
 // Word-wrap text into aiLines.
 void aiWrap(const char* t) {
   while (*t && aiLineCount < AI_MAXLINES) {
     int len = 0, cut = -1;
     while (t[len] && t[len] != '\n' && len < AI_COLS) {
       if (t[len] == ' ') cut = len;
       len++;
     }
     if (len == AI_COLS && t[len] && t[len] != ' ' && t[len] != '\n' && cut > 0) len = cut;
     memcpy(aiLines[aiLineCount], t, len);
     aiLines[aiLineCount][len] = 0;
     aiLineCount++;
     t += len;
     while (*t == ' ') t++;
     if (*t == '\n') t++;
   }
 }
 
 void drawAI() {
   char buf[32];
   drawStatusBar();
   const char* right = "5 Talk";
   const char* mid = nullptr;
   u8g2.setFont(u8g2_font_6x10_tr);
 
   switch (aiPhase) {
     case AI_IDLE:
       drawMosque(25, 13);
       u8g2.drawStr(44, 13, "Deen AI");
       txtC(27, "Ask about religion");
       txtC(41, aiBusy ? "Please wait..." : aiReady ? "Hold 5 and speak" : "Starting mic...");
       break;
 
     case AI_RECORDING: {
       txtC(13, aiReady ? "Listening..." : "Get ready...");
       u8g2.drawRFrame(4, 26, 120, 10, 3);
       int w = aiLevel * 114 / 100;
       if (w > 0) u8g2.drawBox(7, 29, w, 4);
       snprintf(buf, sizeof(buf), "%lu s / %lu s", (unsigned long)(aiRecMs / 1000),
                (unsigned long)(REC_MAX / MIC_RATE));
       txtC(41, buf);
       right = nullptr;
       mid = "Release=send";
       break;
     }
 
     case AI_LISTENED:
     case AI_THINKING: {
       static const char* const dots[4] = {"", ".", "..", "..."};
       const char* d = dots[(millis() / 350) % 4];
       if (aiPhase == AI_THINKING) {
         u8g2.setFont(u8g2_font_5x7_tr);
         for (int i = 0; i < aiLineCount && i < 3; i++) u8g2.drawStr(0, 13 + i * 8, aiLines[i]);
         u8g2.setFont(u8g2_font_6x10_tr);
         snprintf(buf, sizeof(buf), "Thinking%s", d);
         u8g2.drawStr(34, 41, buf);
       } else {
         snprintf(buf, sizeof(buf), "Hearing you%s", d);
         u8g2.drawStr(25, 27, buf);
       }
       right = nullptr;
       break;
     }
 
     case AI_ANSWER: {
       u8g2.setFont(u8g2_font_5x7_tr);
       for (int i = 0; i < AI_ROWS && aiScroll + i < aiLineCount; i++)
         u8g2.drawStr(0, 13 + i * 8, aiLines[aiScroll + i]);
       int maxScroll = aiLineCount - AI_ROWS;
       if (maxScroll > 0) {                        // scroll bar
         int h = 40 * AI_ROWS / aiLineCount;
         if (h < 4) h = 4;
         u8g2.drawVLine(127, 13 + (40 - h) * aiScroll / maxScroll, h);
         mid = "1/2 Scroll";
       }
       break;
     }
 
     case AI_ERROR:
       txtC(20, aiError);
       txtC(38, "Hold 5 to retry");
       break;
   }
   drawFooter("<Back", mid, right);
 }
 
 void drawUI() {
   u8g2.clearBuffer();
   u8g2.setFontPosTop();
   u8g2.setDrawColor(1);
   if (bootPhase != BOOT_DONE) {
     drawBootSplash();
   } else {
     switch (screen) {
       case SCR_HOME:     drawHome();     break;
       case SCR_PLAYER:   drawPlayer();   break;
       case SCR_AYAHS:    drawAyahs();    break;
       case SCR_SETTINGS: drawSettings(); break;
       case SCR_AI:       drawAI();       break;
       case SCR_ADHAN:    drawAdhan();    break;
       case SCR_HURUF:    drawHuruf();  break;
       case SCR_HADITH:   drawHadith(); break;
       case SCR_QA:       drawQA();     break;
     }
   }
   u8g2.sendBuffer();
 }
 
 // =====================================================================
 //                          PLAYBACK CONTROL
 // =====================================================================
 void stopPlayback(const char* msg) {
   playerStop();
   session = false;
   paused = false;
   stepPending = false;
   status = msg;
   dirty = true;
 }
 
 void startPlayback() {
   stepPending = false;
   paused = false;
   if (!audioOk) { status = "No audio mem"; dirty = true; return; }
   Clip c = {(uint8_t)surah, (uint16_t)ayah, (uint8_t)reciterIdx};
   playGen = playerStart(c, playMode == MODE_SURAH);
   session = true;
   status = "Loading...";
   dirty = true;
 }
 
 void togglePlay() {
   if (!session) { startPlayback(); return; }
   if (stepPending) { startPlayback(); return; }   // OK = load it now
   paused = !paused;
   playerPause(paused);
   status = paused ? "Paused" : "Playing";
   dirty = true;
 }
 
 void changeSurah(int delta) {
   surah += delta;
   while (surah < 1) surah += 114;
   while (surah > 114) surah -= 114;
   ayah = 1;
   dirty = true;
 }
 
 void changeAyah(int delta) {
   int dir = delta > 0 ? 1 : -1;
   for (int i = 0; i < abs(delta); i++) {
     ayah += dir;
     if (ayah < 1) {
       surah = (surah <= 1) ? 114 : surah - 1;
       ayah = AYAHS[surah - 1];
     } else if (ayah > AYAHS[surah - 1]) {
       surah = (surah >= 114) ? 1 : surah + 1;
       ayah = 1;
     }
   }
   dirty = true;
 }
 
 // Next / Prev: fade out at once, update the screen, load after presses settle.
 void stepTrack(int d) {
   bool was = session;
   if (was) playerStop();
   if (playMode == MODE_SURAH) changeSurah(d);
   else changeAyah(d);
   if (was) {
     stepPending = true;
     stepAt = millis() + STEP_DELAY_MS;
     paused = false;
     status = "Loading...";
   } else {
     status = "Ready";
   }
 }
 
 void goScreen(Screen s) {
   if (screen == SCR_AI && s != SCR_AI) { playerStop(); aiSetOpen(false); }
   if (screen != s && (screen == SCR_HURUF || screen == SCR_HADITH || screen == SCR_QA)) {
     docCancel();
     stopContentAudio();
   }
   if (s == SCR_HOME && session && !adhanAlert) stopPlayback("Ready");   // Home = silence
   if (s != screen) prevScreen = screen;
   screen = s;
   if (!session) status = "Ready";
   dirty = true;
 }
 
 void openAI() {
   if (session) stopPlayback("Stopped");       // player keeps its place
   aiReturn = screen;
   aiReturnPrev = prevScreen;
   aiSetOpen(true);
   aiPhase = AI_IDLE;
   aiReady = false;
   aiLineCount = aiScroll = 0;
   goScreen(SCR_AI);
 }
 
 void closeAI() {
   goScreen(aiReturn);
   prevScreen = aiReturnPrev;
 }
 
 void pollAI() {
   if (screen != SCR_AI) return;
   static char text[1200];
   lockSh();
   const AiShared& a = sh.ai;
   AiPhase p = a.phase;
   bool ready = a.micReady, busy = a.busy;
   uint8_t level = a.level;
   uint32_t ms = a.recMs, id = a.answerId;
   bool newPhase = p != aiPhase;
   bool newAnswer = p == AI_ANSWER && id != aiShownId;
   if (newAnswer) memcpy(text, a.answer, sizeof(text));
   if (p == AI_ERROR) memcpy(aiError, a.error, sizeof(aiError));
   unlockSh();
 
   if (newPhase || ready != aiReady || busy != aiBusy) dirty = true;
   if (newPhase && p == AI_THINKING) aiLineCount = 0;
   if (newAnswer) { aiShownId = id; aiLineCount = aiScroll = 0; aiWrap(text); dirty = true; }
   aiPhase = p;
   aiReady = ready;
   aiBusy = busy;
   aiLevel = level;
   aiRecMs = ms;
 }
 
 void goBack() {
   if (adhanAlert && (adhanRun == AR_PLAYING || adhanRun == AR_BEEP || adhanRun == AR_WAIT_AI)) {
     adhanEnterHold();
     return;
   }
   if (adhanAlert && adhanRun == AR_HOLD) {
     adhanDismiss();
     return;
   }
   if (screen == SCR_QA && qaMode == QA_ITEM) {
     stopContentAudio();
     docCancel();
     qaMode = QA_TOPICS;
     qaPhase = LP_READY;
     dirty = true;
     return;
   }
   if (session) stopPlayback("Stopped");
   contentPlaying = false;
   switch (screen) {
     case SCR_HOME:   break;
     case SCR_AI:     closeAI(); break;
     case SCR_PLAYER: goScreen(prevScreen == SCR_AYAHS ? SCR_AYAHS : SCR_HOME); break;
     default:         goScreen(SCR_HOME); break;
   }
 }
 
 void finishBoot() {
   if (session) stopPlayback("Ready");
   bootPhase = BOOT_DONE;
   screen = prevScreen = SCR_HOME;
   homeSel = 0;
   homeTop = 0;
   surah = 1;
   ayah = 1;
   playMode = MODE_SURAH;
   status = "Ready";
   dirty = true;
 }
 
 static void applyCloudSettings() {
   lockSh();
   bool cfg = sh.cfgNew;
   DeviceSettings pending = cfgPending;
   int cv = sh.cfgVolume, cr = sh.cfgReciter;
   sh.cfgNew = false;
   unlockSh();
   if (!cfg) return;
 
   bool zoneChanged = strcmp(ds.timezone, pending.timezone) != 0;
   bool urlChanged = strcmp(ds.contentUrl, pending.contentUrl) != 0;
   bool placeChanged = (fabsf(ds.lat - pending.lat) > 0.0001f) ||
                       (fabsf(ds.lon - pending.lon) > 0.0001f) ||
                       ds.method != pending.method || ds.asr != pending.asr ||
                       ds.highLat != pending.highLat ||
                       memcmp(ds.adj, pending.adj, sizeof(ds.adj)) != 0;
   ds = pending;
   dsSavePrefs();
   if (zoneChanged) timeApplyZone();
   if (urlChanged) contentParseUrl(ds.contentUrl);
   if (placeChanged || zoneChanged) prayerYday = -1;
   if (cv >= 5 && cv <= 100) { volLevel = cv / 100.0f; playerVolume(volLevel); }
   if (cr >= 0 && cr < RECITER_COUNT) reciterIdx = cr;
   dirty = true;
   LOGF("cloud cfg v%u applied\n", (unsigned)ds.configVersion);
 }
 
 static void adhanBeginPlay(int prayerId) {
   adhanWhich = prayerId;
   adhanMarkFired(prayerId);
   adhanAlert = true;
 
   adhanCanResume = (surah >= 1 && surah <= 114);
   adhanResumeSurah = surah;
   adhanResumeAyah = ayah;
   adhanResumeReciter = reciterIdx;
   adhanResumeMode = playMode;
   adhanResumeScreen = (screen == SCR_AI) ? aiReturn : screen;
   if (adhanResumeScreen == SCR_ADHAN || adhanResumeScreen == SCR_AI ||
       adhanResumeScreen == SCR_HURUF || adhanResumeScreen == SCR_HADITH ||
       adhanResumeScreen == SCR_QA)
     adhanResumeScreen = SCR_PLAYER;
   adhanResumeWantPlay = session && !paused && surah >= 1 && surah <= 114;
 
   if (screen == SCR_AI) aiSetOpen(false);
   if (session) stopPlayback("Adhan");
 
   const char* file = (prayerId == PT_FAJR) ? "adhan/fajr.mp3" : "adhan/adhan.mp3";
   lockSh();
   snprintf(sh.contentPath, sizeof(sh.contentPath), "%s", file);
   unlockSh();
 
   Clip c = { CONTENT_SURAH, (uint16_t)((prayerId == PT_FAJR) ? 2 : 1), 0 };
   playGen = playerStart(c, false);
   session = true;
   paused = false;
   stepPending = false;
   status = "Adhan...";
   adhanRun = AR_PLAYING;
   goScreen(SCR_ADHAN);
   dirty = true;
 }
 
 static void adhanStartBeepFallback(int prayerId) {
   if (session) stopPlayback("Adhan");
   adhanWhich = prayerId;
   adhanMarkFired(prayerId);
   adhanAlert = true;
   adhanRequestBeeps();
   adhanRun = AR_BEEP;
   status = "Time for prayer";
   goScreen(SCR_ADHAN);
   dirty = true;
 }
 
 static void adhanEnterHold() {
   if (session) stopPlayback("Stopped");
   adhanRun = AR_HOLD;
   adhanAlert = true;
   status = "Stopped";
   dirty = true;
 }
 
 static void adhanDismiss() {
   adhanRun = AR_IDLE;
   adhanAlert = false;
   adhanWhich = -1;
   goScreen(SCR_HOME);
   dirty = true;
 }
 
 static void adhanResumePrev() {
   adhanRun = AR_IDLE;
   adhanAlert = false;
   if (!adhanCanResume) { goScreen(SCR_HOME); return; }
   surah = adhanResumeSurah;
   ayah = adhanResumeAyah;
   reciterIdx = adhanResumeReciter;
   playMode = adhanResumeMode;
   goScreen(adhanResumeScreen == SCR_AYAHS ? SCR_AYAHS : SCR_PLAYER);
   if (adhanResumeWantPlay && adhanResumeScreen != SCR_AYAHS) startPlayback();
   dirty = true;
 }
 
 static void adhanTick() {
   if (bootPhase != BOOT_DONE) return;
 
   if (adhanRun == AR_BEEP) {
     lockSh(); uint8_t left = sh.beepLeft; unlockSh();
     if (left == 0) adhanEnterHold();
     return;
   }
 
   if (adhanRun == AR_PLAYING) {
     if (!session) {
       Status s = playerStatus();
       if (s.gen == playGen && s.state == PS_ERROR) adhanStartBeepFallback(adhanWhich);
       else adhanEnterHold();
     }
     return;
   }
 
   if (adhanRun == AR_WAIT_AI) {
     if (!adhanAiBusy() || millis() - adhanWaitStart >= ADHAN_AI_WAIT_MS) {
       if (WiFi.status() != WL_CONNECTED) adhanStartBeepFallback(adhanWhich);
       else adhanBeginPlay(adhanWhich);
     }
     return;
   }
 
   if (adhanRun != AR_IDLE) return;
   if (!prayerEnsureToday()) return;
 
   time_t nowt = time(nullptr);
   struct tm lt; localtime_r(&nowt, &lt);
   if (adhanFiredYday != lt.tm_yday) {
     adhanFiredYday = lt.tm_yday;
     adhanFiredMask = 0;
     adhanSaveFired();
   }
 
   int now = lt.tm_hour * 60 + lt.tm_min;
   static const int order[5] = { PT_FAJR, PT_DHUHR, PT_ASR, PT_MAGHRIB, PT_ISHA };
   for (int i = 0; i < 5; i++) {
     int id = order[i];
     if (!prayerEnabled(id) || adhanAlreadyFired(id)) continue;
     int t = prayerMin[id];
     if (now < t || now >= t + ADHAN_WINDOW_MIN) continue;
 
     adhanWhich = id;
     if (adhanAiBusy()) {
       adhanRun = AR_WAIT_AI;
       adhanWaitStart = millis();
       LOGF("adhan waiting for AI (%s)\n", prayerName(id));
     } else if (WiFi.status() != WL_CONNECTED) {
       adhanStartBeepFallback(id);
     } else {
       adhanBeginPlay(id);
     }
     break;
   }
 }
 
 // Copy what the audio engine is doing into the UI.
 void pollPlayer() {
   if (!session || stepPending) return;
   Status s = playerStatus();
   if (s.gen != playGen) return;
   ui = s;
 
   const char* msg = status;
   if (s.state == PS_FINISHED) {
     session = false;
     paused = false;
     if (s.clip.surah == CONTENT_SURAH) contentPlaying = false;
     msg = "Done";
   } else if (s.state == PS_ERROR) {
     session = false;
     paused = false;
     if (s.clip.surah == CONTENT_SURAH) contentPlaying = false;
     if (s.clip.surah >= 1 && s.clip.surah <= 114) {
       surah = s.clip.surah;                 // OK retries from the ayah that failed
       ayah = s.clip.ayah;
     }
     msg = (s.error == PE_WIFI) ? "No WiFi" : (s.error == PE_NOTFOUND) ? "Not found" : "Network error";
     dirty = true;
   } else {
     if (s.clip.surah >= 1 && s.clip.surah <= 114 &&
         (s.clip.surah != surah || s.clip.ayah != ayah)) {
       surah = s.clip.surah;
       ayah = s.clip.ayah;
       dirty = true;
     }
     if (adhanRun == AR_PLAYING) msg = "Adhan...";
     else if (paused) msg = "Paused";
     else if (s.state == PS_PLAYING) msg = "Playing";
     else if (s.state == PS_BUFFERING) msg = (s.elapsedMs > 0) ? "Buffering..." : "Loading...";
     if (bootPhase == BOOT_BISMILLAH) msg = "Bismillah...";
   }
   if (msg != status && strcmp(msg, status) != 0) dirty = true;
   status = msg;
 }
 
 // =====================================================================
 //                              BUTTONS
 //   Sampled every 5 ms by a timer. A press counts after 30 ms of solid
 //   LOW, which rejects speaker / WiFi noise without ignoring real presses.
 // =====================================================================
 static QueueHandle_t keyQueue;
 
 static void buttonSample(void*) {
   // (KEY5 state is also published for press-and-hold talking)
   static uint8_t level[5];
   static bool down[5];
   static uint16_t held[5];
   static uint8_t reps[5];
   for (int i = 0; i < 5; i++) {
     if (digitalRead(BTN[i]) == LOW) { if (level[i] < 6) level[i]++; }
     else                            { if (level[i] > 0) level[i]--; }
 
     if (!down[i] && level[i] >= 6) {
       down[i] = true; held[i] = 0; reps[i] = 0;
       if (i == KEY5) talkHeld = true;
       KeyEvent ev = {(uint8_t)i, 0};
       xQueueSend(keyQueue, &ev, 0);
     } else if (down[i] && level[i] == 0) {
       down[i] = false;
       if (i == KEY5) talkHeld = false;
     } else if (down[i] && i != KEY5) {
       if (++held[i] >= 100) {                 // after 500 ms, every 120 ms
         held[i] = 76;
         if (reps[i] < 255) reps[i]++;
         KeyEvent ev = {(uint8_t)i, reps[i]};
         if (uxQueueSpacesAvailable(keyQueue) > 8) xQueueSend(keyQueue, &ev, 0);
       }
     }
   }
 }
 
 void handleKey(const KeyEvent& ev) {
   const int k = ev.key;
   const bool isRepeat = ev.repeat > 0;
   if (isRepeat && k != KEY1 && k != KEY2) return;
 
   if (bootPhase != BOOT_DONE) {
     if (k == KEY4 || k == KEY5) finishBoot();
     if (k == KEY5) openAI();
     return;
   }
 
   if (k == KEY4) { goBack(); return; }
   if (k == KEY5) {
     if (adhanAlert) {                         // clear Adhan so AI does not overlap
       if (session) stopPlayback("Stopped");
       adhanRun = AR_IDLE;
       adhanAlert = false;
       adhanWhich = -1;
     }
     if (screen != SCR_AI) openAI();            // 1st press: open the page
     else {                                     // on the page: hold to speak
       playerStop();                            // silence a spoken answer
       if (aiTalk()) pollAI();
     }
     dirty = true;
     return;
   }
 
   if (adhanAlert && adhanRun == AR_HOLD && k == KEY3) {
     adhanResumePrev();
     return;
   }
   if (adhanAlert && adhanRun != AR_IDLE && adhanRun != AR_HOLD) {
     if (k == KEY3) { adhanEnterHold(); return; }
     return;                                   // ignore nav while Adhan plays
   }
 
   const int dir = (k == KEY1) ? -1 : +1;
   switch (screen) {
     case SCR_AI: {
       int maxScroll = aiLineCount - AI_ROWS;
       if (aiPhase == AI_ANSWER && maxScroll > 0) {
         aiScroll += (k == KEY3) ? AI_ROWS : dir;          // OK = next page
         if (aiScroll > maxScroll) aiScroll = maxScroll;
         if (aiScroll < 0) aiScroll = 0;
         dirty = true;
       }
       break;
     }
 
     case SCR_HOME:
       if (k == KEY1) homeSel = (homeSel + HOME_COUNT - 1) % HOME_COUNT;
       if (k == KEY2) homeSel = (homeSel + 1) % HOME_COUNT;
       if (k == KEY3) {
         if (homeSel == 0) {
           playMode = MODE_SURAH;
           ayah = 1;                       // entering a surah starts at the beginning
           goScreen(SCR_PLAYER);
         } else if (homeSel == 1) {
           goScreen(SCR_AYAHS);             // Ustaz = single ayah
         } else if (homeSel == 2) {
           adhanScroll = 0;
           goScreen(SCR_ADHAN);
         } else if (homeSel == 3) {
           hurufOpen();
           goScreen(SCR_HURUF);
         } else if (homeSel == 4) {
           hadithOpen();
           goScreen(SCR_HADITH);
         } else if (homeSel == 5) {
           qaOpen();
           goScreen(SCR_QA);
         } else {
           goScreen(SCR_SETTINGS);
         }
       }
       dirty = true;
       break;
 
     case SCR_ADHAN:
       if (k == KEY1) { adhanScroll--; dirty = true; }
       if (k == KEY2) { adhanScroll++; dirty = true; }
       break;
 
     case SCR_HURUF:
       if (k == KEY1 || k == KEY2) {
         stopContentAudio();
         hurufIdx = (hurufIdx + (k == KEY1 ? HURUF_COUNT - 1 : 1)) % HURUF_COUNT;
         dirty = true;
       } else if (k == KEY3) {
         if (contentPlaying) stopContentAudio();
         else {
           char path[40];
           snprintf(path, sizeof(path), "huruf/%02d.mp3", hurufIdx + 1);
           startContentMp3(path);
         }
       }
       break;
 
     case SCR_HADITH:
       if (hadithPhase != LP_READY) break;
       if (k == KEY3) {
         if (contentPlaying) stopContentAudio();
         else {
           char path[48];
           uint16_t num = hadithNums[hadithIdx];
           snprintf(path, sizeof(path), "hadith/%03u.mp3?v=%lu",
                    (unsigned)num, (unsigned long)contentVerHadith);
           startContentMp3(path);
         }
       } else {
         int maxScroll = hadithLines - 3;
         if (maxScroll < 0) maxScroll = 0;
         if (k == KEY1) {
           if (hadithScroll > 0) hadithScroll--;
           else if (hadithIdx > 0) { hadithIdx--; hadithLoadItem(); }
         } else if (k == KEY2) {
           if (hadithScroll < maxScroll) hadithScroll++;
           else if (hadithIdx + 1 < hadithCount) { hadithIdx++; hadithLoadItem(); }
         }
         dirty = true;
       }
       break;
 
     case SCR_QA:
       if (qaPhase == LP_LOAD || qaPhase == LP_ERR) break;
       if (qaMode == QA_TOPICS) {
         if (k == KEY1) qaTopicSel = (qaTopicSel + qaTopicCount - 1) % qaTopicCount;
         if (k == KEY2) qaTopicSel = (qaTopicSel + 1) % qaTopicCount;
         if (k == KEY3) {
           qaItemCount = qaTopicCounts[qaTopicSel];
           qaItemIdx = 0;
           qaMode = QA_ITEM;
           qaLoadItem();
         }
         dirty = true;
       } else if (qaPhase == LP_READY) {
         if (k == KEY3) {
           if (contentPlaying) stopContentAudio();
           else {
             char path[64];
             snprintf(path, sizeof(path), "qa/%s/%03d.mp3?v=%lu",
                      qaTopicId[qaTopicSel], qaItemIdx + 1, (unsigned long)contentVerQa);
             startContentMp3(path);
           }
         } else {
           int maxScroll = qaLines - 3;
           if (maxScroll < 0) maxScroll = 0;
           if (k == KEY1) {
             if (qaScroll > 0) qaScroll--;
             else if (qaItemIdx > 0) { qaItemIdx--; qaLoadItem(); }
           } else if (k == KEY2) {
             if (qaScroll < maxScroll) qaScroll++;
             else if (qaItemIdx + 1 < qaItemCount) { qaItemIdx++; qaLoadItem(); }
           }
           dirty = true;
         }
       }
       break;
 
     case SCR_PLAYER:
       if (k == KEY3) togglePlay();
       else stepTrack(dir);
       break;
 
     case SCR_AYAHS:
       if (k == KEY3) {
         playMode = MODE_AYAH;
         goScreen(SCR_PLAYER);
         startPlayback();
       } else {
         changeAyah(ev.repeat > 15 ? dir * 5 : dir);    // hold longer = jump by 5
       }
       break;
 
     case SCR_SETTINGS:
       if (k == KEY3) {
         setSel ^= 1;
       } else if (setSel == 0) {
         reciterIdx = (reciterIdx + dir + RECITER_COUNT) % RECITER_COUNT;
       } else {
         volLevel += 0.05f * dir;
         if (volLevel < 0.05f) volLevel = 0.05f;
         if (volLevel > 1.0f) volLevel = 1.0f;
         playerVolume(volLevel);
       }
       dirty = true;
       break;
   }
 }
 
 // =====================================================================
 //                             LED / SETUP / LOOP
 // =====================================================================
 // Idle -> solid ON. Playing -> slow triangle breathe.
 void ledTick() {
   static uint32_t lastDuty = 999;
   uint32_t duty = 255;
   if (screen == SCR_AI && aiPhase == AI_RECORDING) {
     duty = (millis() / 120) & 1 ? 255 : 0;          // fast blink while listening
   } else if (session && !paused) {
     uint32_t t = millis() % 3200UL;
     duty = (t < 1600UL) ? (t * 255UL / 1600UL) : ((3200UL - t) * 255UL / 1600UL);
   }
   if (duty != lastDuty) { ledcWrite(LED_PIN, duty); lastDuty = duty; }
 }
 
 const char* resetReasonStr() {
   switch (esp_reset_reason()) {
     case ESP_RST_POWERON:   return "Power on";
     case ESP_RST_SW:        return "Software";
     case ESP_RST_PANIC:     return "CRASH";
     case ESP_RST_INT_WDT:
     case ESP_RST_TASK_WDT:
     case ESP_RST_WDT:       return "Watchdog";
     case ESP_RST_BROWNOUT:  return "BROWNOUT";
     case ESP_RST_DEEPSLEEP: return "Deep sleep";
     default:                return "Other";
   }
 }
 
 void setup() {
   delay(200);
 #if ARDUINO_USB_CDC_ON_BOOT
   Serial.begin(115200);
   Serial.setTxTimeoutMs(0);        // never stall when no PC is listening
 #endif
 
   ledcAttach(LED_PIN, LED_PWM_FREQ, LED_PWM_RES);
   ledcWrite(LED_PIN, 255);
   for (int i = 0; i < 5; i++) pinMode(BTN[i], INPUT_PULLUP);
   u8g2.setBusClock(400000);
   u8g2.begin();
 
   bootReason = resetReasonStr();
   LOGF("Reset reason: %s\n", bootReason);
   status = "Starting...";
   drawUI();
   esp_reset_reason_t rr = esp_reset_reason();
   if (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
       rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
     delay(2500);              // leave time to read it
   }
 
   Preferences prefs;
   prefs.begin("wifi", false);
   String ssid = prefs.getString("ssid", "");
   String pass = prefs.getString("pass", "");
   prefs.end();
   // no saved Wi-Fi, or KEY4 held at power-on -> setup
   if (!ssid.length() || digitalRead(BTN[3]) == LOW) runWifiSetup(ssid.length() > 0);
 
   dsLoadPrefs();
   contentParseUrl(ds.contentUrl);
   timeApplyZone();
   adhanLoadFired();
 
   mtx = xSemaphoreCreateMutex();
   keyQueue = xQueueCreate(24, sizeof(KeyEvent));
   sh.volume = volLevel;
   sh.beepLeft = 0;
   sh.contentPath[0] = 0;
 
   // Audio memory first, before WiFi takes the big blocks. Allocated once.
   audioOk = i2sInit();
   mp3 = MP3InitDecoder();
   audioOk = audioOk && mp3 != nullptr;
 
   WiFi.mode(WIFI_STA);
   WiFi.setSleep(false);
   WiFi.setAutoReconnect(true);
   WiFi.begin(ssid.c_str(), pass.c_str());
 
   // priorities: audio 5 > loop 3 > network 2
   vTaskPrioritySet(nullptr, 3);
   if (audioOk) audioOk = xTaskCreate(audioTask, "audio", 8192, nullptr, 5, nullptr) == pdPASS;
   if (audioOk) audioOk = xTaskCreate(netTask, "net", 12288, nullptr, 2, nullptr) == pdPASS;
 
   const esp_timer_create_args_t ta = {
     .callback = buttonSample, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
     .name = "keys", .skip_unhandled_events = true
   };
   esp_timer_handle_t th;
   esp_timer_create(&ta, &th);
   esp_timer_start_periodic(th, 5000);
 
   status = audioOk ? "WiFi..." : "No audio mem";
   bootStart = millis();
   dirty = true;
 }
 
 void loop() {
   KeyEvent ev;
   if (xQueueReceive(keyQueue, &ev, pdMS_TO_TICKS(10)) == pdTRUE) {
     do { handleKey(ev); } while (xQueueReceive(keyQueue, &ev, 0) == pdTRUE);
   }
 
   // ---- boot: WiFi -> Bismillah -> Home ----
   if (bootPhase == BOOT_WIFI) {
     if (WiFi.status() == WL_CONNECTED && audioOk) {
       bootPhase = BOOT_BISMILLAH;
       playMode = MODE_AYAH;
       surah = 1;
       ayah = 1;
       startPlayback();
       status = "Bismillah...";
     } else if (millis() - bootStart > BOOT_WIFI_MS || (!audioOk && millis() - bootStart > 3000)) {
       bool noAudio = !audioOk;
       if (!noAudio) runWifiSetup(true);        // saved Wi-Fi not reachable
       finishBoot();
       status = noAudio ? "No audio mem" : "No WiFi";
     }
   }
 
   if (stepPending && (int32_t)(millis() - stepAt) >= 0) startPlayback();
 
   pollPlayer();
   pollAI();
 
   applyCloudSettings();
   adhanTick();
   contentPollDocs();
 
   if (bootPhase == BOOT_BISMILLAH && !session) finishBoot();
 
   // Hard rule: on Home, nothing plays (Bismillah uses the splash, not Home)
   // Exception: Adhan alert may briefly use SCR_ADHAN, not Home.
   if (bootPhase == BOOT_DONE && screen == SCR_HOME && session && !adhanAlert)
     stopPlayback("Ready");
 
   // Keep NTP status fresh once Wi-Fi is up (non-blocking).
   static uint32_t lastTimePoll = 0;
   if (WiFi.status() == WL_CONNECTED && millis() - lastTimePoll > 2000) {
     lastTimePoll = millis();
     bool was = timeSynced;
     if (timeIsSynced() != was) dirty = true;
   }
 
   ledTick();
 
   // redraw on change; progress 2x per second; clock / WiFi bars every 5 s
   static uint32_t lastDraw = 0;
   uint32_t now = millis();
   bool moving = bootPhase == BOOT_DONE && screen == SCR_PLAYER && session && !paused;
   uint32_t period = moving ? 500 : 5000;
   if (screen == SCR_AI) {
     if (aiPhase == AI_RECORDING) period = 150;
     else if (aiPhase == AI_LISTENED || aiPhase == AI_THINKING) period = 350;
   }
   if (screen == SCR_ADHAN || adhanAlert || adhanRun == AR_WAIT_AI) period = 500;
   if (dirty || now - lastDraw >= period) {
     dirty = false;
     lastDraw = now;
     drawUI();
   }
 }