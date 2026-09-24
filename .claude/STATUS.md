# STATUS — UC2_HIKpy

**Stand:** 2026-09-23 · Branch `main` · Basis-Commit `20a9c8c` (Änderungen noch nicht committet)
**Letzte Arbeitsschritte:** Ring Buffer (`UC2_RingBuff`) implementiert, getestet, gebaut (§5–§10);
CTest-Integration in VS Code repariert (§12).

Dieses Dokument ist die einzige Projektdokumentation für Claude-Sessions: §1–§4 sind die
Dauerinformation (Aufbau, Konventionen, Fallstricke, Buildbefehle), §5–§12 der aktuelle
Arbeitsstand. Vor nicht-trivialer Arbeit §1–§4 und §11 lesen.

---

## 1. Projekt und Aufbau

`uc2_hikpy` kapselt das HikRobot-/MVS-Machine-Vision-SDK in einem C++-Core, exponiert es über
pybind11 nach Python und puffert Frames in einem shared-memory-fähigen Ring Buffer.

```
core/include, core/src   C++-Core (namespace hik) — die eigentliche Logik
bindings/hik_py.cpp      pybind11-Modul `_hik`
python/uc2_hikpy/        reines Python-Paket (Grabber, __main__, .pyi-Stubs)
core/tests/              Catch2-v3-Tests
tests/                   pytest-Tests
Examples/                Programme auf Basis des Core: HikStandalone.cpp (CLI, braucht Kamera),
                         RingBuffExample.cpp (ohne Kamera); nur Top-Level-Build, HIK_BUILD_EXAMPLES
Docs/                    Referenzdokumentation (RingBuff_REF.md)
.claude/STATUS.md        dieses Dokument
```

Schichtung — nicht kurzschließen:

```
Examples/HikStandalone.cpp ─┐
                            ├─→ CaptureApp ─→ MvsCamera ─→ MVS-SDK
bindings/hik_py ────────────┴─→ FrameGrabber ──┘
       ↑                             │
python/uc2_hikpy                     └─→ RingBuffer (UC2_RingBuff) — noch nicht verdrahtet
```

---

## 2. Konventionen

- **C++17**, im Top-Level erzwungen. Keine C++14-Idiome stillschweigend wieder einführen.
- Aller C++-Code liegt in `namespace hik`.
- Include-Guards `UC2HIK_<NAME>_H`; Member mit `m_`; Allman-Klammern; vier Leerzeichen Einzug.
- Doxygen `///` an der gesamten öffentlichen API. Kommentardichte der Umgebung treffen —
  dieser Code dokumentiert das *Warum*, nicht das *Was*.
- **Jede Diagnoseausgabe geht über `core/include/hikLog.h`** (`hik::ERR/WARN/INFO/DBG`),
  niemals `printf`/`cout`/`cerr`. Ausnahme ist echte Programmausgabe in `CaptureApp.cpp` und
  `HikStandalone.cpp` (Frame-Zeilen, Gerätelisten) — die gehört bewusst auf stdout.
- Alles muss unter `-Wall -Wextra` warnungsfrei übersetzen. Das ist der Normalzustand,
  kein Ziel.
- Fehler werden als `bool`-Rückgabe mit Out-Parametern gemeldet, nicht über Exceptions.

---

## 3. Fallstricke

Vier Dinge, die beim ersten Kontakt mit dem Code überraschen. Details jeweils im
angegebenen Abschnitt:

| Fallstrick | Details |
|---|---|
| **Zero-Copy-Lebensdauer.** `bindings/hik_py.cpp` gibt numpy eine View auf einen SDK-eigenen Puffer mit No-Op-Capsule. Das Array wird beim nächsten `wait_for_frame()`/`stop()`/`close()`/`set_roi()` still ungültig. Ungelöst. | §11.3 (P1) |
| **Der Payload-Datenwettlauf im Ring Buffer ist Absicht.** Im `FullPolicy::Overwrite` kann ein Consumer Bytes lesen, die der Producer gerade überschreibt; `ReadHandle::stillValid()` ist der Weg, das zu erfahren. TSan und helgrind melden es. `FullPolicy::Wait` vermeidet es ganz. | §11.1 |
| **Der Speicherblock des Ring Buffers muss zeigerfrei, vtable-frei und POD bleiben** — das ist es, was den geplanten Multi-Prozess-Schritt billig hält. `static_assert`s in `UC2_RingBuff.cpp` sichern das ab. | §7.1 |

---

## 4. Build- und Testbefehle

Es gibt zwei Konfigurationswege, und **beide müssen funktionsfähig bleiben**:

```bash
# Top-Level — das, was die IDE benutzt; baut Core, CLI und Python-Modul.
# Die beiden Test-Optionen stehen für die IDE in .vscode/settings.json.
cmake -S . -B build -DHIK_BUILD_TESTS=ON -DHIK_ENABLE_HARDWARE_TESTS=ON
cmake --build build -j8
ctest --test-dir build            # 78 Tests

# Nur der Core, ohne Python-Modul — schnellere Iteration auf der C++-Seite.
# Baut Bibliothek + Tests, aber keine Programme (die liegen in Examples/, nur Top-Level).
cmake -S core -B build/core -DHIK_BUILD_TESTS=ON
cmake --build build/core -j8
ctest --test-dir build/core       # 75 Tests (ohne [hardware])

# Python-Paket (editable). Nach C++-Änderungen nötig: Python importiert eine *Kopie*
# der .so in site-packages, ein reiner CMake-Rebuild reicht nicht.
uv pip install -e . --no-build-isolation
.venv/bin/python -m pytest tests -q -m "not hardware"

# Manueller Ring-Buffer-Test ohne Kamera (nur Top-Level-Build): 20 simulierte Mono16-Frames,
# Analyse- und Anzeige-Thread, Disk-Streaming. Endet, wenn das Vorschaufenster geschlossen wird.
./build/Examples/RingBuffExample [storageDir]      # Default /data/recordings
```

Einzelne Catch2-Gruppen direkt aus der Testbinary:

```bash
./build/core/tests/hikcam_core_tests "[ring]"      # Ring Buffer
./build/core/tests/hikcam_core_tests "[mt]"        # Nebenläufigkeit
./build/core/tests/hikcam_core_tests "[stream]"    # Plattenformat
```

Tags: `[ring]` `[policy]` `[seqlock]` `[vtable]` `[stream]` `[mt]` `[time]` `[grabber]`
`[pixel]` `[roi]` `[error]` `[options]` `[window]` `[sdk]` `[hardware]`.

**Ohne das MVS-SDK lässt sich nicht einmal konfigurieren.** Default `/opt/MVS`; überschreibbar
mit `-DMVS_ROOT=/pfad` oder der Umgebungsvariablen `MVCAM_SDK_PATH`. CMake bricht hart ab,
wenn `MvCameraControl.h` fehlt.

### Test-Konventionen

- Catch2 v3, wird beim Konfigurieren per `FetchContent` geholt (einmalig Netz nötig).
- `catch_discover_tests(... PROPERTIES SKIP_RETURN_CODE 4)` registriert jeden `TEST_CASE`
  einzeln bei CTest. Die `4` ist wesentlich: Catch2 beendet sich mit diesem Code, wenn alle
  Fälle eines Laufs übersprungen wurden — genau das tun die `[hardware]`-Tests ohne Kamera.
  Ohne die Property liest CTest den Code als Fehler (siehe §12).
- Hardware-Tests sind über `-DHIK_ENABLE_HARDWARE_TESTS=ON` zuschaltbar und nutzen `SKIP()`,
  wenn keine Kamera angeschlossen ist.
- Python-Hardware-Tests tragen `@pytest.mark.hardware`.

---

## 5. Ring Buffer — Stand der Umsetzung

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

## 6. Entscheidungen (vom Nutzer abgenommen, 2026-09-17)

Diese vier wurden vor der Implementierung explizit entschieden — nicht ohne Rücksprache ändern:

| Thema | Entscheidung | Konsequenz |
|---|---|---|
| **Plattenformat** | Eine append-only Container-Datei je Session (`.uc2rb`) + `.json`-Sidecar | ein `writev()` pro Frame; optimal für Pi 5 / SD-Karte |
| **C++-Standard** | Projektweiter Sprung auf **C++17** | `std::filesystem`, `std::optional` verfügbar; alle Alt-TUs geprüft |
| **Wait-Policy** | Warten auf **alle registrierten** Consumer, mit Timeout → dann überschreiben | ein abgestürzter Consumer kann die Kamera nicht dauerhaft blockieren |
| **Multi-Prozess** | Layout jetzt shm-fähig, aber **nur Heap-Allokator** ausgeliefert | shm ist später eine zusätzliche VTable-Implementierung, keine Protokolländerung |

---

## 7. Architektur in Kurzform

### 7.1 Drei Regeln, die den Multi-Prozess-Schritt später billig machen

Der gesamte Zustand liegt in **einem zusammenhängenden Speicherblock**. Damit derselbe
Block später in mehrere Prozesse gemappt werden kann, gilt durchgängig:

1. **Keine Zeiger im Block** — ausschließlich Byte-Offsets ab Blockanfang.
2. **Keine C++-Objekte mit vtable im Block** — ein vtable-Zeiger ist eine prozesslokale
   Codeadresse. Genau deshalb sind die Erweiterungspunkte **C-Structs aus Funktionszeigern
   außerhalb** des Blocks (`UC2_MemoryOps`, `UC2_SinkOps`) und keine abstrakten Basisklassen.
   Das ist zugleich die Form, die eine `PyCapsule` (`void*` + Destruktor) direkt aufnehmen kann.
3. **Kein `std::string`/`std::vector` im Block** — Metadaten sind `char[]` fester Länge.

`static_assert`s in `UC2_RingBuff.cpp` sichern Größen (8448/64/64/64 Byte), Standard-Layout
und `is_always_lock_free` für 32- und 64-Bit-Atomics ab.

### 7.2 Speicherlayout

```
base + 0              UC2_SessionHeader   8448 B, fix
base + 8448           UC2_Control         Producer-Cursor, Consumer-Tabelle, Zähler
                                          (jedes Feld auf eigener Cacheline, 64 B)
base + slotsOffset    UC2_SlotRecord[N]   64 B je Slot
base + payloadOffset  Payload 0..N-1      Stride = frameBytes auf 64 B aufgerundet
```

`slotIndex = sequence % slotCount` — damit ist „lesen per Index“ trivial und die
Arithmetik prozessübergreifend eindeutig.

### 7.3 Synchronisation

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

### 7.4 Kopierbudget

Genau **eine** Kopie auf dem Weg hinein: Quelle → Slot, ausgeführt vom Aufrufer direkt in
`WriteHandle::data()`. Lesen kopiert **gar nicht** (Consumer bekommen `const uint8_t*` in
den Slot). Der Plattenschreiber nutzt `writev()` mit zwei iovecs (Frame-Header + Payload)
direkt aus dem Slot — auch hinaus also keine Zwischenkopie.

---

## 8. API-Kurzreferenz

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

## 9. Plattenformat (`.uc2rb`)

```
[UC2_SessionHeader 8448 B]   magic "UC2RB", format/layout-Version, w/h/channels/depth,
                             frameBytes, slotCount, startWallNs, startMonoNs,
                             cameraId[64], comment[8192]
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

## 10. Verifikation (Stand 2026-09-17, x86-64, GCC 16.2.1)

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

## 11. Offene Punkte

### 11.1 Bekannte, bewusst akzeptierte Einschränkung

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

### 11.2 Nächste Schritte am Ring Buffer

1. **pybind11-Anbindung** — `readLatest`/`readSlot` als numpy-Views. Achtung: das ist
   genau die Stelle, an der der P1-Befund (siehe §11.3) entschieden werden muss. Der Ring
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

### 11.3 Aus dem Code-Review vom 2026-09-03 weiterhin offen

Das vorige STATUS.md war ein reines Review (Commit `64c711c`). Bis auf den erledigten
CTest-Punkt sind dessen Befunde **unverändert offen** — am Altcode wurde nichts geändert:

| Prio | Befund | Bemerkung |
|---|---|---|
| **P1** | Zero-Copy-Lebensdauer in `bindings/hik_py.cpp:77` — numpy-Array mit No-Op-Capsule, wird beim nächsten `wait_for_frame()`/`stop()`/`set_roi()` ungültig | vor der pybind11-Anbindung des Ring Buffers zu entscheiden |
| **P2** | `CaptureApp.cpp:158-163` — `getchar()`-Thread hängt ohne Terminal-stdin dauerhaft, bei freigegebenem GIL | trifft `run_connection_test(frames=0)` |
| **P2** | `ImageWindow.cpp:49-57` — `XNextEvent` ohne Timeout, Konstruktor kann endlos hängen | Headless-Fallback greift hier nicht |
| **P3** | `FrameGrabber.cpp:266-268` — `setRoi()` verwirft den Rückgabewert von `start()` | Einzeiler: `return ok && (!wasRunning || start());` |
| ~~P3~~ | ~~Root-`CMakeLists.txt` ruft kein `enable_testing()`~~ | **erledigt 2026-09-23**, siehe §12 |
| **P4** | `CaptureApp.cpp`: 4× `fprintf(stderr,…)` noch nicht auf `ERR()`/`WARN()` umgestellt | Voraussetzung für die Python-`logging`-Anbindung |
| **P4** | `FrameGrabber.h`: `#endif`-Kommentar noch `HIKCAMTEST_FRAMEGRABBER_H` | kosmetisch |
| **P4** | `LICENSE` fehlt, `pyproject.toml:11` deklariert aber MIT | Todo „soll MIT bleiben?" offen |
| ~~P4~~ | ~~`CMakeLists.txt:10` setzt `HIK_BUILD_APP` mit `CACHE … FORCE`~~ | **erledigt 2026-09-24**: `HIK_BUILD_APP` entfällt, `HikStandAlone` liegt unter `Examples/` (Schalter `HIK_BUILD_EXAMPLES`, im Wheel OFF) |
| **P4** | keine CI (`.github/` fehlt) | die 78 Tests liefen bisher nur lokal |

---

## 12. CTest-Integration in VS Code (2026-09-23)

**Symptom:** Klick auf „Run" an einem `TEST_CASE` endete mit
`ctest integration disabled. Please see the cmake.ctest.testExplorerIntegrationEnabled setting.`

**Die Meldung zeigt auf die falsche Stelle.** Die Einstellung hat in CMake Tools 1.24.42 den
Default `true` und war nirgends überschrieben. Laut Extension-Bundle (`dist/main.js`,
`CTestDriver.refreshTests`) prüft CMake Tools auf `<binaryDir>/CTestTestfile.cmake`; fehlt die
Datei, bleibt `testExplorer` undefined und `runCTest` gibt genau diese irreführende Meldung aus.

**Zwei Ursachen, beide behoben:**

1. `build/CMakeCache.txt` hatte `HIK_BUILD_TESTS:BOOL=OFF` → Tests wurden gar nicht gebaut
   (daher auch das `ninja: no work to do`).
2. Die Root-`CMakeLists.txt` rief kein `enable_testing()` auf. CMake schreibt
   `CTestTestfile.cmake` nur in Binärverzeichnisse, deren Quellverzeichnis Testing aktiviert
   hat — es entstand `build/core/CTestTestfile.cmake`, aber nicht `build/CTestTestfile.cmake`.
   Das war der P3-Befund aus §11.3.

**Änderungen:**

| Datei | Änderung |
|---|---|
| `CMakeLists.txt` | `enable_testing()` vor `add_subdirectory(core)`, bewusst unbedingt (nicht in `if(HIK_BUILD_TESTS)`), damit die Datei auch bei ausgeschalteten Tests existiert und der Explorer 0 Tests statt einer Fehlermeldung zeigt |
| `.vscode/settings.json` | `HIK_BUILD_TESTS=ON` und `HIK_ENABLE_HARDWARE_TESTS=ON` in `cmake.configureSettings` |
| `core/tests/CMakeLists.txt` | `catch_discover_tests(… PROPERTIES SKIP_RETURN_CODE 4)` |

**Zu `SKIP_RETURN_CODE 4`:** kam erst beim Verifizieren heraus. Catch2 beendet sich mit
Exit-Code 4, wenn alle Testfälle eines Laufs übersprungen wurden. Die `[hardware]`-Tests tun
genau das ohne angeschlossene Kamera (`SKIP(...)` in `test_capture_hw.cpp`). Ohne die
Property liest CTest den Code als Fehler — die drei Tests waren rot statt übersprungen.
`enable_testing()` allein hätte also eine dauerhaft rote Suite auf jeder Maschine ohne Kamera
hinterlassen.

**Verifiziert:** `build/CTestTestfile.cmake` existiert · `ctest --test-dir build` → 78 Tests,
100 % bestanden, 3 `[hardware]` als *Skipped* · `cmake -S core` weiterhin 75/75 grün ·
`uv pip install -e . --no-build-isolation` läuft durch · `pytest -m "not hardware"` 6/6 grün.
