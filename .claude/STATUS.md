# STATUS — UC2_HIKpy

**Stand:** 2026-09-17 · Branch `main` · Basis-Commit `64c711c` (Änderungen noch nicht committet)
**Letzter Arbeitsschritt:** Ring Buffer (`UC2_RingBuff`) implementiert, getestet, gebaut.

---

## 1. Was zuletzt gemacht wurde

Der Ring Buffer aus `Todos.md` ist umgesetzt — als eigenständige Komponente, noch **nicht**
mit `FrameGrabber` verdrahtet und **noch nicht** über pybind11 exponiert (beides war
bewusst ausgeklammert).

### Neue Dateien

| Datei | Zeilen | Inhalt |
|---|---:|---|
| `core/include/UC2_RingBuff.h` | 730 | öffentliche API, POD-Layout, C-ABI-VTables, vollständig Doxygen-dokumentiert |
| `core/src/UC2_RingBuff.cpp` | 1546 | Implementierung inkl. `FileStreamSink` |
| `core/tests/test_ringbuffer.cpp` | 852 | 52 Fälle: Konfiguration, Geometrie, Flags, Policies, Seqlock, VTable |
| `core/tests/test_ringbuffer_stream.cpp` | 399 | 7 Fälle: Container-Format, Sidecar, Fehlerpfade |
| `core/tests/test_ringbuffer_mt.cpp` | 323 | 4 Fälle: Nebenläufigkeit, ~10 000 Frames pro Lauf |

### Geänderte Dateien

- `CMakeLists.txt` (Root) — **C++14 → C++17** (projektweit)
- `core/CMakeLists.txt` — C++17-Default, `src/UC2_RingBuff.cpp` zur `hikcam_core` hinzugefügt
- `core/tests/CMakeLists.txt` — die drei neuen Testdateien registriert

---

## 2. Entscheidungen (vom Nutzer abgenommen, 2026-09-17)

Diese vier wurden vor der Implementierung explizit entschieden — nicht ohne Rücksprache ändern:

| Thema | Entscheidung | Konsequenz |
|---|---|---|
| **Plattenformat** | Eine append-only Container-Datei je Session (`.uc2rb`) + `.json`-Sidecar | ein `writev()` pro Frame; optimal für Pi 5 / SD-Karte |
| **C++-Standard** | Projektweiter Sprung auf **C++17** | `std::filesystem`, `std::optional` verfügbar; alle Alt-TUs geprüft |
| **Wait-Policy** | Warten auf **alle registrierten** Consumer, mit Timeout → dann überschreiben | ein abgestürzter Consumer kann die Kamera nicht dauerhaft blockieren |
| **Multi-Prozess** | Layout jetzt shm-fähig, aber **nur Heap-Allokator** ausgeliefert | shm ist später eine zusätzliche VTable-Implementierung, keine Protokolländerung |

---

## 3. Architektur in Kurzform

### 3.1 Drei Regeln, die den Multi-Prozess-Schritt später billig machen

Der gesamte Zustand liegt in **einem zusammenhängenden Speicherblock**. Damit derselbe
Block später in mehrere Prozesse gemappt werden kann, gilt durchgängig:

1. **Keine Zeiger im Block** — ausschließlich Byte-Offsets ab Blockanfang.
2. **Keine C++-Objekte mit vtable im Block** — ein vtable-Zeiger ist eine prozesslokale
   Codeadresse. Genau deshalb sind die Erweiterungspunkte **C-Structs aus Funktionszeigern
   außerhalb** des Blocks (`UC2_MemoryOps`, `UC2_SinkOps`) und keine abstrakten Basisklassen.
   Das ist zugleich die Form, die eine `PyCapsule` (`void*` + Destruktor) direkt aufnehmen kann.
3. **Kein `std::string`/`std::vector` im Block** — Metadaten sind `char[]` fester Länge.

`static_assert`s in `UC2_RingBuff.cpp` sichern Größen (512/64/64/64 Byte), Standard-Layout
und `is_always_lock_free` für 32- und 64-Bit-Atomics ab.

### 3.2 Speicherlayout

```
base + 0              UC2_SessionHeader   512 B, fix
base + 512            UC2_Control         Producer-Cursor, Consumer-Tabelle, Zähler
                                          (jedes Feld auf eigener Cacheline, 64 B)
base + slotsOffset    UC2_SlotRecord[N]   64 B je Slot
base + payloadOffset  Payload 0..N-1      Stride = frameBytes auf 64 B aufgerundet
```

`slotIndex = sequence % slotCount` — damit ist „lesen per Index“ trivial und die
Arithmetik prozessübergreifend eindeutig.

### 3.3 Synchronisation

- **Korrektheit kommt ausschließlich aus Atomics.** Jeder Slot trägt einen Seqlock-Zähler
  (gerade = stabil, ungerade = Schreibvorgang läuft). Ein Leser liest den Zähler vor und
  nach der Nutzung; `ReadHandle::stillValid()` sagt, ob der Producer den Slot
  zwischenzeitlich recycelt hat.
- **`std::mutex`/`std::condition_variable` liegen außerhalb des Blocks** und dienen nur
  dazu, Leerlauf-Spinning zu vermeiden. Beim Multi-Prozess-Schritt werden sie durch
  `pthread_*` mit `PTHREAD_PROCESS_SHARED` ersetzt (läuft auf Linux **und** macOS) —
  **ohne** Änderung am Protokoll.
- Genau **ein** Producer: ein zweites gleichzeitiges `beginWrite()` wird abgewiesen und
  geloggt, nicht stillschweigend verschränkt.

### 3.4 Kopierbudget

Genau **eine** Kopie auf dem Weg hinein: Quelle → Slot, ausgeführt vom Aufrufer direkt in
`WriteHandle::data()`. Lesen kopiert **gar nicht** (Consumer bekommen `const uint8_t*` in
den Slot). Der Plattenschreiber nutzt `writev()` mit zwei iovecs (Frame-Header + Payload)
direkt aus dem Slot — auch hinaus also keine Zwischenkopie.

---

## 4. API-Kurzreferenz

```cpp
RingBuffer::Config cfg;
cfg.storagePath   = "/data/rec";              // leer  -> nur RAM, kein Streaming
cfg.slotCount     = 64;                       // >= 2
cfg.comment       = "Messreihe 7";
cfg.policy        = FullPolicy::Overwrite;    // oder ::Wait
cfg.waitTimeoutMs = 1000;

RingBuffer rb(cfg);
rb.setImageSize(1920, 1200);                  // später aus der Kamera
rb.setChannels(1);                            // beliebig > 0, nicht auf 3 begrenzt
rb.setDepth(PixelDepth::UInt8);               // UInt8 | UInt16 | Float64
rb.setCameraId("MV-CA023-10UM:01234");
rb.configure();                               // prüft + allokiert (danach eingefroren)
rb.startRecording();                          // stempelt Startzeit, öffnet die Sinks

// Schreiben (die eine erlaubte Kopie)
RingBuffer::WriteHandle w;
if (rb.beginWrite(w)) {                       // stempelt tsBefore, setzt kFlagWriting
    std::memcpy(w.data(), fv.data, fv.nbytes);
    rb.commitWrite(w, fv.nbytes);             // stempelt tsAfter, setzt kFlagWritten
}

// Lesen (kopierfrei)
RingBuffer::ReadHandle r;
rb.readLatest(r);                             // letztes geschriebenes Bild
rb.readSlot(7, r);                            // Bild nach Index im Puffer
rb.readSequence(1234, r);                     // Bild nach globaler Frame-Nummer
r.stillValid();                               // wurde der Slot unter uns recycelt?
r.release(/*markRead=*/true, /*markSaved=*/false);
```

**Handles sind RAII:** ein nicht committetes `WriteHandle` bricht im Destruktor ab
(ein halb gefüllter Slot wird nie veröffentlicht), ein `ReadHandle` gibt den Slot frei
und markiert ihn als gelesen.

### Flags (alle vier aus der Anforderung)

`kFlagWriting` → `kFlagWritten` → `kFlagRead` → `kFlagSaved`, abfragbar über
`slotFlags(slot)` und `ReadHandle::flags()`.

### Zeitstempel — bewusste Designentscheidung

Pro Frame werden **monotone** Nanosekunden gespeichert (`steady_clock`), nicht die
Wanduhr: Letztere springt bei NTP-Korrekturen und Sommerzeit, was Jitter-Auswertungen
unbrauchbar macht. Der Session-Header trägt `startWallNs` **und** `startMonoNs` zum
*selben* Zeitpunkt — damit ist die absolute Zeit jedes Frames exakt rekonstruierbar.

### Consumer-Registrierung

`registerConsumer()` → ID (max. `kMaxConsumers` = 8). **Nur registrierte** Consumer halten
den Producer im Wait-Modus auf; `kAnonymousConsumer` (Default) liest, ohne zu bremsen.
Das verhindert, dass ein beiläufiger Leser (Vorschau, Debug) die Kamera ausbremst.

---

## 5. Plattenformat (`.uc2rb`)

```
[UC2_SessionHeader  512 B]   magic "UC2RB", format/layout-Version, w/h/channels/depth,
                             frameBytes, slotCount, startWallNs, startMonoNs,
                             cameraId[64], comment[256]
[UC2_FrameHeader 64 B][Payload]   sequence 0
[UC2_FrameHeader 64 B][Payload]   sequence 1
...
```

`UC2_FrameHeader`: sequence, nbytes, tsBeforeNs, tsAfterNs, slot, flags, w/h/channels/depth.

Dateiname `uc2_<cameraId>_<YYYYmmdd_HHMMSS>[_NNN].uc2rb`; der `_NNN`-Suffix verhindert,
dass zwei Sessions innerhalb derselben Sekunde einander überschreiben. Daneben liegt ein
`.json`-Sidecar mit denselben Metadaten in lesbarer Form.

**Schreibpfad:** `UC2_SinkOps::onFrame` läuft auf dem Producer-Thread und läutet nur die
Glocke. Ein eigener Writer-Thread holt die Bytes als registrierter Consumer selbst aus dem
Ring. Der Kamera-Thread fasst also **nie** das Dateisystem an — auf dem Pi 5 der
entscheidende Punkt. Diese Aufteilung ist zugleich die Naht, an der der Streamer später
zu einem eigenen Prozess wird.

---

## 6. Verifikation (Stand 2026-09-17, x86-64, GCC 16.2.1)

| Prüfung | Ergebnis |
|---|---|
| `ctest` gesamt | **75/75 grün** (19 bestehende + 56 neue), 6796 Assertions |
| Build `hikcam_core` + `HikStandAlone` + Tests | 0 Fehler, **0 Warnungen** (`-Wall -Wextra`) |
| Neue Dateien zusätzlich mit `-Wpedantic` | 0 Warnungen |
| Alle 6 Alt-TUs unter C++17 | 0 Fehler, 0 Warnungen |
| pybind11-Modul `_hik` unter C++17 | baut und linkt |
| valgrind memcheck, `[ring]~[mt]` | 0 Fehler, keine Leaks |
| valgrind memcheck, `[mt]` | 0 Fehler, keine Leaks |
| `[mt]` 5× wiederholt (normales Scheduling) | stabil grün |

> **Sanitizer:** ASan/UBSan/TSan konnten hier **nicht** laufen — auf der Maschine fehlt die
> Laufzeitbibliothek (`libasan.so.8.0.0` nicht installiert). Ersatzweise wurde valgrind
> memcheck verwendet. Ein ASan-Lauf auf dem Pi wäre vor Produktiveinsatz sinnvoll.

---

## 7. Offene Punkte

### 7.1 Bekannte, bewusst akzeptierte Einschränkung

**Der Payload-Datenwettlauf im Overwrite-Modus ist by design.** Beim kopierfreien Lesen
liest der Consumer Bytes, die der Producer parallel überschreiben kann. Nach dem
C++-Speichermodell ist das formal ein Data Race; **TSan und helgrind werden es melden**.
Es ist dasselbe Muster wie der Seqlock im Linux-Kernel: abgesichert wird nicht durch
Ausschluss, sondern durch Erkennung — `stillValid()` sagt zuverlässig, ob die gelesenen
Bytes konsistent waren. Der MT-Test „a frame that still validates is never torn" prüft
genau diese Eigenschaft über 4000 Frames. Im **Wait**-Modus tritt der Fall gar nicht auf,
weil der Producer einen noch nicht quittierten Slot nicht anfasst.

Wer garantiert konsistente Bytes ohne Nachprüfung braucht: `FullPolicy::Wait` + Consumer
registrieren.

### 7.2 Nächste Schritte am Ring Buffer

1. **pybind11-Anbindung** — `readLatest`/`readSlot` als numpy-Views. Achtung: das ist
   genau die Stelle, an der der P1-Befund (siehe 7.3) entschieden werden muss. Der Ring
   Buffer liefert jetzt die Grundlage dafür: über `ReadHandle::stillValid()` lässt sich
   ein Generationszähler ohne Zusatzaufwand bauen.
2. **PyCapsule-Hook** — `UC2_SinkOps` ist dafür fertig; `destroy` ist die Stelle für den
   Capsule-Destruktor. Noch keine Implementierung vorhanden.
3. **Verdrahtung mit `FrameGrabber`** — `waitForFrame()` → `beginWrite`/`memcpy`/`commitWrite`.
   Löst nebenbei den Todo „waitForFrame blockiert den Python-Aufrufer".
4. **Multi-Prozess** — `shm_open`+`mmap`-Implementierung von `UC2_MemoryOps`, plus Umstieg
   der Condvar auf `PTHREAD_PROCESS_SHARED`. Zusätzlich zu klären: Namensvergabe der
   Segmente und Aufräumen nach Absturz.
5. **README** — Zeile ~171 sagt noch „**no ring buffer here** — an indexable history is
   provided by a separate external component". Das ist jetzt `UC2_RingBuff` und sollte
   dort verlinkt werden.
6. **ARM-Lauf** — noch nie auf dem Pi 5 gebaut. Die `static_assert`s auf
   `is_always_lock_free` würden dort sofort anschlagen, falls etwas nicht passt.

### 7.3 Aus dem Code-Review vom 2026-09-03 weiterhin offen

Das vorige STATUS.md war ein reines Review (Commit `64c711c`). Dessen Befunde sind
**unverändert offen** — am Altcode wurde nichts geändert:

| Prio | Befund | Bemerkung |
|---|---|---|
| **P1** | Zero-Copy-Lebensdauer in `bindings/hik_py.cpp:77` — numpy-Array mit No-Op-Capsule, wird beim nächsten `wait_for_frame()`/`stop()`/`set_roi()` ungültig | vor der pybind11-Anbindung des Ring Buffers zu entscheiden |
| **P2** | `CaptureApp.cpp:158-163` — `getchar()`-Thread hängt ohne Terminal-stdin dauerhaft, bei freigegebenem GIL | trifft `run_connection_test(frames=0)` |
| **P2** | `ImageWindow.cpp:49-57` — `XNextEvent` ohne Timeout, Konstruktor kann endlos hängen | Headless-Fallback greift hier nicht |
| **P3** | `FrameGrabber.cpp:266-268` — `setRoi()` verwirft den Rückgabewert von `start()` | Einzeiler: `return ok && (!wasRunning || start());` |
| **P3** | Root-`CMakeLists.txt` ruft kein `enable_testing()` → `ctest --test-dir build` findet nichts | `cmake -S core -B build/core` funktioniert dagegen |
| **P4** | `CaptureApp.cpp`: 4× `fprintf(stderr,…)` noch nicht auf `ERR()`/`WARN()` umgestellt | Voraussetzung für die Python-`logging`-Anbindung |
| **P4** | `FrameGrabber.h`: `#endif`-Kommentar noch `HIKCAMTEST_FRAMEGRABBER_H` | kosmetisch |
| **P4** | `LICENSE` fehlt, `pyproject.toml:11` deklariert aber MIT | Todo „soll MIT bleiben?" offen |
| **P4** | keine CI (`.github/` fehlt) | die 75 hardwarefreien Tests liefen bisher nur lokal |

---

## 8. Build- und Testbefehle

```bash
# Konfigurieren + bauen mit Tests
cmake -S core -B build/core -DHIK_BUILD_TESTS=ON
cmake --build build/core -j8

# Alles
ctest --test-dir build/core

# Nur der Ring Buffer
./build/core/tests/hikcam_core_tests "[ring]"
./build/core/tests/hikcam_core_tests "[mt]"        # Nebenläufigkeit
./build/core/tests/hikcam_core_tests "[stream]"    # Plattenformat
```

Tags: `[ring]` `[policy]` `[seqlock]` `[vtable]` `[stream]` `[mt]` `[time]`.
