# STATUS — UC2_cam_py

**Stand:** 2026-10-18 · Branch `main` · Basis-Commit `3dc4ea9` (Änderungen noch nicht committet)
**Letzte Arbeitsschritte:** Paket in `uc2_cam_py` umbenannt; Python-Kamera-Schnittstelle mit
Ring Buffer, Simulations-Backend und Log-Weiterleitung umgesetzt (§13); Profiling-/Sanitizer-
Infrastruktur (Docs/Tooling.md).

Dieses Dokument ist die einzige Projektdokumentation für Claude-Sessions: §1–§4 sind die
Dauerinformation (Aufbau, Konventionen, Fallstricke, Buildbefehle), §5–§13 der aktuelle
Arbeitsstand. Vor nicht-trivialer Arbeit §1–§4 und §11 lesen.

## Umgesetzte Pläne (neueste zuerst)

### `create_camera(..., debug_window=True)` — einziger Weg zum Vorschaufenster — 2026-10-21

**Implementiert:** ja · **Committet:** nein

- `create_camera` hat als letzten benannten Parameter `debug_window: bool = False` (bool-Prüfung
  wie `start_on_init`). `True` → `_preview.DebugWindow`: Daemon-Thread
  `uc2_cam_py-debug-window` besitzt das X11-Fenster (Xlib ohne `XInitThreads`), zeichnet alle
  ~30 ms das neueste Bild (nur bei neuer `sequence`). Der Konstruktor wartet bis zu 5 s auf
  den Öffnungsversuch, damit ein Fehler vor der Rückkehr von `create_camera` geloggt ist.
- Kein X11 (Build ohne X11 oder kein Display) → ERROR „debug_window=True: …“, **keine
  Exception**, Kamera läuft weiter. Bei fehlendem Display loggt der C++-Core zusätzlich sein
  eigenes ERROR. Fenster per Hand geschlossen → INFO, Kamera läuft weiter.
  `Camera.close()` beendet den Thread **vor** dem Schließen des Backends.
- Andere Wege entfernt: `preview.py` → internes `_preview.py` (ohne die Funktion `show()`);
  `run_connection_test(index, frames, snapshot)` ist jetzt ein Python-Wrapper, der
  `_hik.run_connection_test(..., display=False)` aufruft; `python -m uc2_cam_py` ohne
  `--no-display`. C++ (Binding, CaptureApp, HikStandAlone) unverändert → kein Rebuild.
- Tests: `test_display.py` auf `debug_window` umgestellt, dazu zwei neue Tests ohne Display
  (Standard aus; ohne `DISPLAY` → ERROR, keine Exception); Signatur- und Fehlertabellen
  ergänzt; `uc2_cam_py.preview` nicht mehr importierbar.
- Doku: `Docs/UC2_cam_py_interface.md` §2, §7.7, §9, §10; README (Beispiel, Baum).
- Verifikation: Ruff sauber · pyright 0 Fehler · pytest `-m "not hardware"` 138 passed
  (inkl. `display`-Tests mit echtem Fenster) · von Hand mit `DISPLAY=` geprüft.
  Hardware (`-m hardware`, `display and hardware`) noch vom Nutzer zu prüfen.
- Bekannt, nicht angefasst: Schlägt `start()` bei `start_on_init=True` im Konstruktor fehl,
  bleibt das Backend offen. `*******XOpenDisplay Fail *******` auf stdout ohne Display kommt
  vom MVS-SDK schon beim Import.

### Diagnosen in `camera.py` beseitigt, Python-Code auf 3.12-Syntax — 2026-10-20

**Implementiert:** ja · **Committet:** nein

- Ursache: Die 21 Pylance-Fehler waren Fehlalarme, keine Laufzeitfehler. `LoggerAdapter.warning`
  hatte den Typ `Any | None` (`getattr(..., None) or ...`, None-Prüfung nur in einer
  Comprehension); `x/y/w/h` im `roi`-Setter wurden in einem anderen `if` gebunden als benutzt.
  Die 59 Ruff-Warnungen kamen aus dem breiteren Standard-Regelsatz von Ruff ≥ 0.17 (UP, PYI,
  RUF, I, C4 …) und dem 3.8-Stil des Codes bei `requires-python = "==3.12.*"`.
- `_logging.py`: `LoggerAdapter` sucht die Methoden mit `find()` und prüft einmal explizit auf
  None → die Attribute sind `LogFn = Callable[[str], Any]`. Verhalten unverändert.
- `camera.py`: `roi`-Setter über `rect: Roi | None`; `_fmt` entfernt (nirgends benutzt);
  `__enter__ -> Self`, `__exit__` typisiert.
- Paket, Tests und `_hik.pyi`: `Optional`/`Dict`/`List`/`Tuple`/`Union` → 3.12-Syntax
  (Ruff-Autofix), überflüssige Anführungszeichen in Annotationen entfernt.
- `pyproject.toml`: `[tool.ruff]` — line-length 120, `extend-select` UP/PYI/RUF; ignoriert
  PYI021 (Docstrings im Stub sind die Hover-Hilfe) und RUF022 (`__all__` thematisch geordnet);
  isort kennt `uc2_cam_py` (first-party) und `conftest` (local); `dict(k=v)` erlaubt.
- Verifikation: `uvx ruff check python/ tests/` sauber · `uvx pyright` 0 Fehler ·
  pytest `-m "not hardware"` 134 passed.
- Nachträglich behoben: Tippfehler `"diable_existing_loggers"` in `tests/test_hik.py::setLogger`
  — dictConfig ignorierte den Schlüssel und schaltete bestehende Logger ab.

---

## 1. Projekt und Aufbau

`uc2_cam_py` ist eine herstellerneutrale Python-Kameraschnittstelle (Spezifikation:
`Docs/UC2_cam_py_interface.md`) auf einem C++-Core: Kameraabstraktion (`CameraBase`, Backends
HikRobot/MVS und Simulation), shared-memory-fähiger Ring Buffer mit Plattenstreaming, Logging
mit austauschbarer Senke. Python greift über pybind11 kopierfrei auf die Bilder zu.

```
core/include, core/src   C++-Core (namespace hik) — die eigentliche Logik
bindings/hik_py.cpp      pybind11-Modul `_hik`
python/uc2_cam_py/       Python-Paket: camera.py (Camera, create_camera), data.py (ImgData,
                         ImgMetaData), hikrobot.py/simulated.py, defaults.py,
                         _preview.py (Fenster für debug_window), _logging.py (Log-Forwarder),
                         _resources.py, _hik.pyi
core/tests/              Catch2-v3-Tests
tests/                   pytest-Tests
Examples/                Programme auf Basis des Core: HikStandalone.cpp (CLI, braucht Kamera),
                         RingBuffExample.cpp (ohne Kamera); nur Top-Level-Build, HIK_BUILD_EXAMPLES
Docs/                    Referenzdokumentation (RingBuff_REF.md, Tooling.md)
CMakePresets.json        Build-Varianten debug | profile | asan | tsan → build/<preset>
core/cmake/              HikTooling.cmake: Schalter HIK_SANITIZERS, HIK_PROFILING
tools/                   analyze.sh (valgrind/perf), Suppressions für Sanitizer und valgrind
.claude/STATUS.md        dieses Dokument
```

Schichtung — nicht kurzschließen:

```
python/uc2_cam_py (Camera) ─→ bindings/hik_py (_hik) ─→ CameraBase ─┬─→ HikCamera ─→ MvsCamera ─→ MVS-SDK
                                                                    ├─→ SimCamera
                                                                    └─→ RingBuffer + FileStreamSink
Examples/HikStandalone.cpp ─→ CaptureApp ─→ MvsCamera   (Diagnose; auch run_connection_test)
```

---

## 2. Konventionen

- **C++17**, im Top-Level erzwungen. Keine C++14-Idiome stillschweigend wieder einführen.
- Aller C++-Code liegt in `namespace hik`.
- Include-Guards `UC2HIK_<NAME>_H`; Member mit `m_`; Allman-Klammern; vier Leerzeichen Einzug.
- Doxygen `///` an der gesamten öffentlichen API. Kommentardichte der Umgebung treffen —
  dieser Code dokumentiert das *Warum*, nicht das *Was*.
- **Jede Diagnoseausgabe geht über `core/include/hikLog.h`** (`hik::ERR/WARN/INFO/DBG`),
  niemals `printf`/`cout`/`cerr`. Die Senke ist austauschbar (`setLogSink`); Standard ist
  stderr, im Python-Modul eine Queue, die ein Python-Thread an `logging` weitergibt. Ausnahme ist echte Programmausgabe in `CaptureApp.cpp` und
  `HikStandalone.cpp` (Frame-Zeilen, Gerätelisten) — die gehört bewusst auf stdout.
- Alles muss unter `-Wall -Wextra` warnungsfrei übersetzen. Das ist der Normalzustand,
  kein Ziel.
- Fehler werden als `bool`-Rückgabe mit Out-Parametern gemeldet, nicht über Exceptions.
- **Python:** 3.12-Syntax (`X | None`, `list[...]`, `Self`), keine `typing.Optional`/`List`.
  `uvx ruff check python/ tests/` muss sauber sein; Regeln in `pyproject.toml` `[tool.ruff]`.

---

## 3. Fallstricke

Dinge, die beim ersten Kontakt mit dem Code überraschen. Details jeweils im
angegebenen Abschnitt:

| Fallstrick | Details |
|---|---|
| **ROI/Binning während der Aufnahme starten die Session neu.** Der Ring Buffer friert seine Geometrie ein; `CameraBase` stoppt, ändert, startet neu → neuer Ring, neue `.uc2rb`-Datei. Jeder `start()` legt ohnehin einen neuen Ring an. | §13 |
| **Catch2 definiert `INFO`/`WARN` als Makros.** In Tests `hikLog.h` vor Catch2 einbinden und `(hik::INFO)(...)` schreiben, sonst zerlegt der Präprozessor Definition bzw. Aufruf. | `core/tests/test_log.cpp` |
| **Der Payload-Datenwettlauf im Ring Buffer ist Absicht.** Im `FullPolicy::Overwrite` kann ein Consumer Bytes lesen, die der Producer gerade überschreibt; `ReadHandle::stillValid()` ist der Weg, das zu erfahren. TSan, helgrind und drd melden es; für die MT-Tests ist es in `tools/sanitizers/tsan.supp` bzw. `tools/valgrind/hik.supp` gezielt unterdrückt (Docs/Tooling.md §5). `FullPolicy::Wait` vermeidet es ganz. | §11.1 |
| **Der Speicherblock des Ring Buffers muss zeigerfrei, vtable-frei und POD bleiben** — das ist es, was den geplanten Multi-Prozess-Schritt billig hält. `static_assert`s in `UC2_RingBuff.cpp` sichern das ab. | §7.1 |

---

## 4. Build- und Testbefehle

Es gibt zwei Konfigurationswege, und **beide müssen funktionsfähig bleiben**:

```bash
# Top-Level über CMakePresets.json — das, was die IDE benutzt (cmake.useCMakePresets=always).
# Vier Presets, je ein Baum build/<preset>: debug (Standard, mit Python-Modul),
# profile (-O2 -g + Frame Pointer), asan (ASan+UBSan+LSan), tsan. Details: Docs/Tooling.md.
cmake --preset debug && cmake --build --preset debug
ctest --preset debug              # 78 Tests; ebenso --preset asan / tsan / profile

# Nur der Core, ohne Python-Modul und ohne Programme — schnellere Iteration auf der C++-Seite.
# Sanitizer/Profiling hier per -DHIK_SANITIZERS="address;undefined"|thread bzw. -DHIK_PROFILING=ON.
cmake -S core -B build/core -G Ninja -DHIK_BUILD_TESTS=ON
cmake --build build/core -j8
ctest --test-dir build/core       # 75 Tests (ohne [hardware])

# Analyse (valgrind, perf/hotspot, callgrind/kcachegrind, c2c): tools/analyze.sh, siehe Docs/Tooling.md.

# Python-Paket (editable). Nach C++-Änderungen nötig: Python importiert eine *Kopie*
# der .so in site-packages, ein reiner CMake-Rebuild reicht nicht. Ebenso nach dem Löschen
# oder Umbenennen einer .py-Datei: die Editable-Installation hält eine feste Dateiliste.
uv pip install -e . --no-build-isolation
.venv/bin/python -m pytest tests -q -m "not hardware"    # 134 Tests, Sim-Backend, ~12 s
.venv/bin/python -m pytest tests -q -m hardware          # dieselben Fälle an der HikRobot-Kamera
UC2_DISPLAY_SECONDS=30 .venv/bin/python -m pytest -m display   # Live-Anzeige (get_last_image)
uvx ruff check python/ tests/                            # Lint, Konfiguration in pyproject.toml

# Manueller Ring-Buffer-Test ohne Kamera (nur Top-Level-Build): 20 simulierte Mono16-Frames,
# Analyse- und Anzeige-Thread, Disk-Streaming. Endet, wenn das Vorschaufenster geschlossen wird.
./build/debug/Examples/RingBuffExample [storageDir]   # Default /data/recordings
```

Einzelne Catch2-Gruppen direkt aus der Testbinary:

```bash
./build/debug/core/tests/hikcam_core_tests "[ring]"      # Ring Buffer
./build/debug/core/tests/hikcam_core_tests "[mt]"        # Nebenläufigkeit
./build/debug/core/tests/hikcam_core_tests "[stream]"    # Plattenformat
```

Tags: `[ring]` `[policy]` `[seqlock]` `[vtable]` `[stream]` `[mt]` `[time]` `[camera]`
`[sim]` `[log]` `[pixel]` `[roi]` `[error]` `[options]` `[window]` `[sdk]` `[hardware]`.

pytest-Marker: `hardware` (echte Kamera, sonst übersprungen), `display` (Vorschaufenster,
ohne DISPLAY/WAYLAND_DISPLAY übersprungen). Die Fixture `camera` in `tests/conftest.py` läuft
parametrisiert für `Simulated` und `HikRobot`.

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

1. ~~**pybind11-Anbindung**~~ — **erledigt 2026-10-18** (§13): `ImageHandle` hält Slot + Ring.
2. **PyCapsule-Hook** — `UC2_SinkOps` ist dafür fertig; `destroy` ist die Stelle für den
   Capsule-Destruktor. Noch keine Implementierung vorhanden.
3. ~~**Verdrahtung mit der Kamera**~~ — **erledigt 2026-10-18** (§13): `CameraBase::deliverFrame`;
   `FrameGrabber` entfällt.
4. **Multi-Prozess** — `shm_open`+`mmap`-Implementierung von `UC2_MemoryOps`, plus Umstieg
   der Condvar auf `PTHREAD_PROCESS_SHARED`. Zusätzlich zu klären: Namensvergabe der
   Segmente und Aufräumen nach Absturz.
5. ~~**README**~~ — **erledigt 2026-10-18**.
6. **ARM-Lauf** — noch nie auf dem Pi 5 gebaut. Die `static_assert`s auf
   `is_always_lock_free` würden dort sofort anschlagen, falls etwas nicht passt.

### 11.3 Aus dem Code-Review vom 2026-09-03 weiterhin offen

Das vorige STATUS.md war ein reines Review (Commit `64c711c`). Bis auf den erledigten
CTest-Punkt sind dessen Befunde **unverändert offen** — am Altcode wurde nichts geändert:

| Prio | Befund | Bemerkung |
|---|---|---|
| ~~P1~~ | ~~Zero-Copy-Lebensdauer (numpy-Array mit No-Op-Capsule)~~ | **erledigt 2026-10-18**: View-Basis ist ein `ImageHandle`, das Slot und `shared_ptr<RingBuffer>` hält |
| **P2** | `CaptureApp.cpp:158-163` — `getchar()`-Thread hängt ohne Terminal-stdin dauerhaft, bei freigegebenem GIL | trifft `run_connection_test(frames=0)` |
| **P2** | `ImageWindow.cpp:49-57` — `XNextEvent` ohne Timeout, Konstruktor kann endlos hängen | Headless-Fallback greift hier nicht |
| ~~P3~~ | ~~`FrameGrabber::setRoi()` verwirft den Rückgabewert von `start()`~~ | **entfällt 2026-10-18**: `FrameGrabber` entfernt; `CameraBase::changeGeometry` meldet den Neustart korrekt |
| ~~P3~~ | ~~Root-`CMakeLists.txt` ruft kein `enable_testing()`~~ | **erledigt 2026-09-23**, siehe §12 |
| ~~P4~~ | ~~`CaptureApp.cpp`: 4× `fprintf(stderr,…)`~~ | **erledigt 2026-10-18** (auf `ERR`/`WARN` umgestellt) |
| ~~P4~~ | ~~`FrameGrabber.h`: `#endif`-Kommentar~~ | **entfällt** (Datei entfernt) |
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

---

## 13. Python-Kamera-Schnittstelle `uc2_cam_py` (2026-10-18)

Umgesetzt nach freigegebenem Plan; Spezifikation: **`Docs/UC2_cam_py_interface.md`**
(konsistente Fassung des Nutzer-Entwurfs, inkl. Zuordnung Entwurf → Python-Namen und Liste der
Korrekturen/Ergänzungen).

### Entscheidungen (vom Nutzer abgenommen)

| Thema | Entscheidung |
|---|---|
| Umbenennung | Projektinhalt `uc2_hikpy` → `uc2_cam_py` (Paket, Distribution, CMake-Projekt, Doku). **Root-Ordner benennt der Nutzer selbst um**, danach `uv sync`; C++-Namespace `hik` und `hikcam_core` bleiben |
| Namen | durchgängig snake_case |
| Alte API | `Grabber`/`FrameGrabber`/`Frame`/`AutoMode`/`Roi` entfernt; `list_devices`, `run_connection_test` bleiben |
| Simulation | Hersteller `"Simulated"` (`SimCamera`, wandernder Gauß aus `SimPattern.h`) |

### Aufbau

| Schicht | Dateien | Inhalt |
|---|---|---|
| C++ neutral | `CameraBase.h/.cpp` | Ring Buffer je `start()` neu (Geometrie eingefroren), `deliverFrame()` = die eine Kopie, In-flight-Zähler für sauberes Stoppen, Neustart bei ROI/Binning, Lesen mit `shared_ptr<RingBuffer>`; Pixelformat-Tabelle (PFNC), `nearestOnGrid`, `nearestBinning`, ROI-Umrechnung |
| C++ Geräte | `HikCamera.*`, `SimCamera.*`, `SimPattern.*`, `MvsCamera.*` (erweitert: Access-Mode, stille Getter, Enum-Werte, Transportfilter) | Einheiten ms/fps/dB, ROI in ungebinnten Sensorpixeln |
| Logging | `hikLog.h/.cpp` | austauschbare Senke; Python: Queue + Daemon-Thread (`_logging.py`), kein C++-Thread wartet auf den GIL |
| Bindings | `bindings/hik_py.cpp` | `CameraBase`/`HikCamera`/`SimCamera`, `ImageHandle` (read-only numpy-View), `ImageWindow`, Log-Queue |
| Python | `python/uc2_cam_py/*` | `Camera` (Validierung, Klemmen, Logging laut Spezifikation, RAM-/Plattenprüfung, Settings-JSON im Session-Kommentar), `ImgData`/`ImgMetaData` |

### Verifikation

- `ctest` in debug/profile/asan/tsan: **100/100**, ohne neue Warnungen; Nur-Core-Weg 96/96.
- pytest `-m "not hardware"`: **134 passed** (inkl. 2 `display`-Tests mit Fenster); headless
  132 passed + 2 skipped; `-m hardware` ohne Kamera: 19 skipped.
- `RingBuffExample` (jetzt mit `SimPattern`): `RESULT: PASS`, identische Schwerpunkte.
- Nebenbei behoben: `-Wformat-truncation` in `UC2_RingBuff.cpp` (Suffix-Puffer 8 → 16 Byte).

### Offen / ungeprüft

- **Ohne Kamera nicht prüfbar** — vom Nutzer mit `pytest -m hardware` zu verifizieren:
  HikRobot-Knotennamen und Einheiten, Binning-Enum-Werte (= Faktor angenommen),
  `SensorWidth`/`SensorHeight`, Pixelformat-Umstellung, Auswahl per Seriennummer,
  Reihenfolge `list_devices(connection_type)` = Index.
- `hikrobot.MODELS` ist leer → `pixel_pitch_um` = None, bis Datenblattwerte eingetragen sind.
- Trigger nur Platzhalter; Geräte-Zeitstempel (Belichtung) nicht erfasst; `lost_packets`
  nicht mehr exponiert; C++-Logs ohne Kamerabezug (mehrere Kameras → letzter Logger).
- `MvsCamera.cpp: preloadRenderLib()` sucht den Renderer nur unter `lib/64` bzw. `lib/32`
  (betrifft auf dem Pi nur die SDK-Vorschau in `run_connection_test`).
