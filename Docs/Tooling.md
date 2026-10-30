# Profiling und Fehlersuche (C/C++)

Wie man den C/C++-Teil (Core, `Examples/`, Catch2-Tests) auf **Laufzeit** profiliert und auf
**Speicher- und Nebenläufigkeitsfehler** prüft. Der Python-Teil ist hier ausgenommen.

| Datei | Inhalt |
|---|---|
| [CMakePresets.json](../CMakePresets.json) | die vier Build-Varianten |
| [core/cmake/HikTooling.cmake](../core/cmake/HikTooling.cmake) | Schalter `HIK_SANITIZERS`, `HIK_PROFILING` |
| [tools/analyze.sh](../tools/analyze.sh) | valgrind/perf-Aufrufe mit passenden Optionen |
| [tools/sanitizers/](../tools/sanitizers/) | Suppressions für LSan, TSan, UBSan |
| [tools/valgrind/hik.supp](../tools/valgrind/hik.supp) | Suppressions für memcheck, helgrind, drd |

---

## 1. Die vier Presets

Jedes Preset baut in sein **eigenes** Verzeichnis `build/<preset>`. Beim Umschalten muss
deshalb nichts neu gebaut werden.

| Preset | Flags | Python-Modul | Wofür |
|---|---|---|---|
| `debug` | `-O0 -g` | ja | Entwickeln, gdb, **valgrind** (memcheck, helgrind, drd) |
| `profile` | `-O2 -g` + Frame Pointer | nein | **Laufzeit**: perf → hotspot, callgrind → kcachegrind, c2c |
| `asan` | `-O1 -g -fsanitize=address,undefined` | nein | Speicherzugriffsfehler, Use-after-free, Leaks (LSan), Undefined Behavior |
| `tsan` | `-O1 -g -fsanitize=thread` | nein | Data Races, Lock-Reihenfolge, Deadlocks |

Warum diese Aufteilung:
- **Laufzeit misst man nur im `profile`-Preset.** Debug-Code (`-O0`) hat ein völlig anderes
  Laufzeitprofil als optimierter Code. Die Frame Pointer (`-fno-omit-frame-pointer
  -mno-omit-leaf-frame-pointer`) kosten etwa 1 % und geben perf vollständige Aufrufketten.
- **ASan und TSan schließen sich gegenseitig aus**, daher zwei Presets. Valgrind wiederum läuft
  nicht auf Sanitizer-Binaries und nutzt `debug` (Fehler) bzw. `profile` (callgrind).
- Ein Python-Modul mit Sanitizern bräuchte `LD_PRELOAD`. Deshalb bauen nur `debug` und der
  Wheel-Build das Modul. Ein `-DHIK_SANITIZERS=…` schaltet es automatisch ab.

### Umschalten

**VS Code:** In der Statusleiste von CMake Tools das *Configure Preset* wählen (debug / profile /
asan / tsan). Danach wirken F7 (Build), der Test Explorer, F5 und die `analyze:`-Tasks auf
dieses Preset. IntelliSense folgt automatisch.

**Kommandozeile:**
```bash
cmake --preset profile            # einmalig bzw. nach CMake-Änderungen
cmake --build --preset profile
ctest --preset profile
```

**Nur-Core-Weg** (ohne Presets, ohne Beispiele): dieselben Schalter per `-D`:
```bash
cmake -S core -B build/core-asan -G Ninja -DHIK_BUILD_TESTS=ON "-DHIK_SANITIZERS=address;undefined"
```
Die Laufzeit-Optionen (`ASAN_OPTIONS` usw., siehe §2) muss man dann selbst setzen.

---

## 2. Speicher- und Nebenläufigkeitsfehler

### Sanitizer (schnell, erste Wahl)

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Die Presets setzen die Laufzeit-Optionen, auch fürs Bauen, weil `catch_discover_tests` die
Testbinary schon beim Build startet:

| Variable | Wesentliche Werte |
|---|---|
| `ASAN_OPTIONS` | `detect_leaks=1`, `detect_stack_use_after_return=1`, `strict_string_checks=1`, `check_initialization_order=1`, `fast_unwind_on_malloc=0` (\*) |
| `LSAN_OPTIONS` | `suppressions=tools/sanitizers/lsan.supp` |
| `UBSAN_OPTIONS` | `print_stacktrace=1`, `halt_on_error=1` (UB bricht ab, statt nur zu warnen) |
| `TSAN_OPTIONS` | `suppressions=tools/sanitizers/tsan.supp`, `second_deadlock_stack=1` |

(\*) Das MVS-SDK hat keine Frame Pointer. Ohne den langsamen Unwinder endet jeder Stack im SDK bei
`<unknown module>`, und seine Leaks ließen sich nicht per Suppression zuordnen.

Einzelnes Programm unter einem Sanitizer: in VS Code das Preset `asan`/`tsan` wählen und F5.
Eine Meldung hält dann im Debugger an. Oder über das Skript, mit einer Logdatei pro Prozess:
```bash
tools/analyze.sh san build/asan/Examples/RingBuffExample build/asan/recordings
```

### Valgrind (langsam, zweite Meinung, Debug-Build)

```bash
tools/analyze.sh memcheck build/debug/Examples/RingBuffExample build/debug/recordings
tools/analyze.sh helgrind build/debug/core/tests/hikcam_core_tests "[mt]"
tools/analyze.sh drd      build/debug/core/tests/hikcam_core_tests "[mt]"
```
- **memcheck**: ungültige Zugriffe, Lesen uninitialisierter Werte, Leaks. Es werden nur
  `definite` und `indirect` Leaks als Fehler gezählt.
- **helgrind / drd**: Races und Lock-Reihenfolge. Wichtig: **Beide modellieren `std::atomic`
  nicht.** Beim lock-freien Ring-Buffer-Protokoll (Seqlock, Bestätigungs-Cursor) melden sie deshalb
  Races, die TSan korrekt als synchronisiert erkennt. Für den Ring Buffer ist **TSan maßgeblich**.
- Der Exit-Code ist 99, wenn valgrind Fehler gefunden hat. Die Zusammenfassung pro Prozess steht
  am Ende der Ausgabe.

### Stand der Analyse (2026-09-24)

| Werkzeug | Ergebnis |
|---|---|
| `ctest --preset asan` | 78/78, keine Befunde im eigenen Code |
| `ctest --preset tsan` | 78/78, keine Befunde im eigenen Code |
| memcheck / helgrind / drd, `[mt]` + `[sdk]` | 0 Fehler nach Suppressions |
| `RingBuffExample` unter ASan und TSan | `RESULT: PASS`, keine Meldungen |

Zwei bekannte **Timing-Effekte** unter valgrind (keine Speicherfehler):
- Der Test *„under Wait a keeping-up consumer loses nothing“* scheitert unter memcheck/callgrind.
  Der Consumer ist dort so langsam, dass das Wait-Timeout (`waitTimeoutMs`) abläuft.
- `RingBuffExample` meldet unter memcheck `RESULT: FAIL`, weil der Disk-Streamer bei 5 fps nicht
  mithält und Frames verworfen werden.

---

## 3. Laufzeit-Profiling

Immer mit dem **`profile`**-Preset (das Skript warnt sonst).

### perf → hotspot (Stichproben, echte Zeit, geringe Störung)
```bash
tools/analyze.sh perf build/profile/Examples/RingBuffExample build/profile/recordings
```
- Nimmt mit `--call-graph fp` auf (Frame Pointer) und öffnet hotspot mit Flame Graph,
  Aufruferbaum und Timeline pro Thread/CPU.
- `--dwarf` statt Frame Pointer, wenn die Aufrufketten *durch* Systembibliotheken wichtig sind.
  Die System-glibc hat keine Frame Pointer; die Datenmenge wird dafür deutlich größer.
- Kindprozesse (fork/exec) des Programms werden automatisch mit erfasst.

### callgrind → kcachegrind (exakte Aufrufzahlen, simuliert, ~50× langsamer)
```bash
tools/analyze.sh callgrind build/profile/core/tests/hikcam_core_tests "[ring]"
```
Gut für „wie oft wird was aufgerufen“ und Kosten pro Zeile. Die absoluten Zeiten sind
simuliert, nicht gemessen.

### perf c2c: False Sharing zwischen Kernen
```bash
tools/analyze.sh c2c build/profile/Examples/RingBuffExample build/profile/recordings
```
Findet Cache-Lines, die zwischen Kernen hin- und herwandern. Damit lässt sich direkt prüfen, ob
das 64-Byte-Layout von `UC2_Control`, `UC2_SlotRecord` und `UC2_Consumer` wirkt. Die Ergebnisse
landen in `c2c-report.txt`. Auf AMD läuft c2c über IBS, auf Intel über PEBS.

### `kernel.perf_event_paranoid`
| Modus | benötigter Wert |
|---|---|
| `perf` (eigener Prozess) | ≤ 2 (Standard) |
| `c2c` | ≤ 1 |
| `perf-system` | ≤ 0 |

```bash
sudo sysctl kernel.perf_event_paranoid=0       # gilt bis zum Neustart
```

---

## 4. `tools/analyze.sh` — Kurzreferenz

```
tools/analyze.sh <modus> [--no-open] [--dwarf] [--] <binary> [args...]
tools/analyze.sh perf-system <sekunden> [--no-open] [--dwarf]
```

| Modus | Werkzeug | Ausgabe |
|---|---|---|
| `memcheck`, `helgrind`, `drd` | valgrind | `<modus>.<pid>.log` + Zusammenfassung |
| `callgrind` | valgrind → kcachegrind | `callgrind.out.<pid>` |
| `perf` | perf record → hotspot | `perf.data` |
| `perf-system` | perf record -a → hotspot | `perf.data` (alle CPUs, alle Prozesse) |
| `c2c` | perf c2c | `perf.data`, `c2c-report.txt` |
| `san` | ASan/TSan-Binary direkt | `asan.<pid>`, `tsan.<pid>`, `ubsan.<pid>` |

- Die Ergebnisse landen in `build/reports/<modus>-<zeitstempel>/`.
- `--no-open` unterdrückt den Start von hotspot/kcachegrind; ohne Display wird der Viewer-Befehl
  nur ausgegeben.
- **In VS Code:** *Terminal → Run Task → `analyze: …`*. Die Tasks arbeiten auf dem *Launch
  Target* der CMake-Statusleiste im aktiven Preset und fragen nach den Programmargumenten.

Schutzmechanismen: Sanitizer-Binaries werden für valgrind abgelehnt. perf und callgrind warnen
bei Nicht-`profile`-Binaries. Bei zu hohem `perf_event_paranoid` und bei Seitengröße ≠ 4 KB
(valgrind) bricht das Skript mit Hinweis ab.

---

## 5. Suppressions

**Regel: Unterdrückt wird nur Drittcode (MVS-SDK, X11/xcb, glibc-Interna) und der dokumentierte
Payload-Race.** Alles andere ist ein echter Befund und wird behoben, nicht unterdrückt.

| Eintrag | Datei | Grund |
|---|---|---|
| `leak:/MVS/lib/` | lsan.supp | Das SDK hält interne Objekte bis Prozessende |
| `called_from_lib:libMvCameraControl.so`, `race:/MVS/lib/` | tsan.supp | SDK-interne Threads (u. a. `localtime`/`tzset`), nicht instrumentiert |
| `race:test_ringbuffer_mt.cpp` | tsan.supp | §11.1 in STATUS: Die MT-Tests provozieren den Overwrite-Payload-Race absichtlich |
| `*intact*` (Helgrind:Race, drd:ConflictingAccess) | hik.supp | dasselbe, zusätzlich fehlende `std::atomic`-Modellierung bei helgrind/drd |
| `drd:CondErr` in `hik::RingBuffer*` | hik.supp | bewusstes `{ lock_guard } notify_all()` (Benachrichtigen außerhalb des Mutex) |
| `obj:*/MVS/lib/*` (Leak, Race, PthAPIerror, CondErr) | hik.supp | SDK |
| `fun:_dl_close*` (LockOrder), `fun:xcb_*` (CondErr) | hik.supp | glibc bzw. libxcb |

Die SDK-Muster enthalten nur den Pfadteil `/MVS/lib/`. Sie greifen damit für `lib/64` (x86) wie
für `lib/aarch64` (Pi) und für alle SDK-Bibliotheken, auch die nachgeladenen
`libMvProducer*.so` / `*.cti`.

**Neue Einträge erstellen:**
- valgrind: `valgrind --tool=<tool> --gen-suppressions=all …` liefert fertige Blöcke (mit
  C++-Namen in der codierten Form, die valgrind abgleicht). Kürzen, per `...` und Wildcards
  verallgemeinern, kommentieren.
- TSan/LSan: `race:`/`leak:`-Zeilen passen auf Funktionsname, Datei oder Modulpfad
  (Teilstring, `*` erlaubt).

**Hinweis für eigene Consumer:** Wer im Modus `FullPolicy::Overwrite` Payload liest, löst unter
TSan denselben Race aus wie die MT-Tests. Die Abhilfe ist nicht eine weitere Suppression, sondern
`stillValid()` prüfen (dann gezielt für diese Funktion unterdrücken) oder `FullPolicy::Wait`
verwenden.

---

## 6. Mehrprozess-Betrieb

Der Ring Buffer soll später von **mehreren Prozessen** auf verschiedenen Kernen über Shared Memory
genutzt werden. Was die Werkzeuge dann leisten:

| Werkzeug | Mehrere Prozesse |
|---|---|
| ASan / UBSan / LSan | pro Prozess. `analyze.sh san` schreibt `asan.<pid>` usw. |
| TSan, helgrind, drd | **nur innerhalb eines Prozesses.** Ein Race *zwischen* zwei Prozessen über das Shared-Memory-Segment sehen sie grundsätzlich nicht |
| valgrind | `--trace-children=yes` und eine Logdatei pro PID (`%p`); schon heute sichtbar, weil das SDK Hilfsprozesse startet |
| perf | `perf` folgt Kindprozessen; `perf-system` erfasst unabhängig gestartete Prozesse auf allen CPUs |
| perf c2c | genau das richtige Werkzeug für Cache-Line-Konflikte zwischen Prozessen auf verschiedenen Kernen |

**Wie man die Lücke bei Races zwischen Prozessen schließt:**
- Das Protokoll ist in beiden Fällen dasselbe. Die Multithread-Tests (`[mt]`) unter TSan prüfen
  es stellvertretend.
- Die Atomics in der Region müssen **lock-free** sein, sonst funktionieren sie nicht über
  Prozessgrenzen. Das sichern die `static_assert(…::is_always_lock_free)` in `UC2_RingBuff.cpp` ab.
- `std::mutex`/`std::condition_variable` sind nicht prozessübergreifend. Sie liegen deshalb
  außerhalb der Region und müssen beim Umstieg durch prozessgeteilte Primitive (z. B. `futex`,
  `pthread_*` mit `PTHREAD_PROCESS_SHARED`) ersetzt werden.
- Beim Umstieg auf `shm_open`/`mmap` erkennt ASan kein Überschreiben *innerhalb* des gemappten
  Segments, nur über seine Grenzen hinaus. Die Offset- und Größenprüfungen im Code bleiben deshalb
  wichtig.

**Typischer Profiling-Lauf mit zwei Prozessen:**
```bash
sudo sysctl kernel.perf_event_paranoid=0
taskset -c 2 build/profile/…/producer &      # Prozesse gezielt auf Kerne legen
taskset -c 6 build/profile/…/consumer &
tools/analyze.sh perf-system 10              # 10 s alle CPUs → hotspot
```
Für False Sharing zwischen den beiden: `perf c2c record -a -- sleep 10` (bzw.
`tools/analyze.sh c2c` auf einen Startprozess, der die anderen forkt).

---

## 7. Raspberry Pi 5

Die Presets und alle Flags sind portabel (GCC ≥ 12, CMake ≥ 3.25, Preset-Version 6). Das SDK wird
unter `lib/64`, `lib/aarch64` oder `lib/arm64` gesucht.

**Pakete:**
```bash
sudo apt install build-essential cmake ninja-build libx11-dev valgrind linux-perf hotspot kcachegrind
```
(libasan/libtsan/libubsan kommen mit dem GCC-Paket.)

| Punkt | Pi 5 |
|---|---|
| ASan/UBSan/LSan, TSan | grundsätzlich ja; siehe Seitengröße |
| valgrind | **nur mit 4-KB-Seiten-Kernel.** Der Standard-Kernel `kernel_2712.img` nutzt 16-KB-Seiten; `analyze.sh` bricht dann mit Hinweis ab |
| perf + hotspot | ja (Cortex-A76-PMU) |
| perf c2c | **nein**: braucht Arm SPE, das der A76 nicht hat. Ersatz: `perf stat -e l1d_cache_refill,l2d_cache_refill,l1d_cache -- <binary>` oder `valgrind --tool=cachegrind` |
| Cache-Line | 64 B, wie im Code angenommen |

**4-KB-Kernel aktivieren** (für valgrind, und falls TSan/ASan mit 16-KB-Seiten Probleme machen): in
`/boot/firmware/config.txt` die Zeile `kernel=kernel8.img` ergänzen und neu starten.
`getconf PAGESIZE` muss dann `4096` zeigen.

**Profile vom Pi am PC ansehen:** Auf dem Pi `perf archive build/reports/perf-…/perf.data`, beide
Dateien kopieren, am PC `hotspot --sysroot <entpacktes Archiv> perf.data`.

**Erster Lauf auf dem Pi:** Schritt für Schritt in [ARM_Checkliste.md](ARM_Checkliste.md), von
der Systemprüfung über die Speicherordnung bis zur Kamera, mit Ergebnistabelle. Ein erstes Profil
danach:
```bash
tools/analyze.sh perf --no-open build/profile/Examples/RingBuffExample /tmp/rec
```
