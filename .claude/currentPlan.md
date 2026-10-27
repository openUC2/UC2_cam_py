<!-- Stand 2026-10-27: Plan umgesetzt (alle vier Etappen), noch nicht committet.
     Ergebnis und Abweichungen: .claude/STATUS.md, Eintrag 2026-10-27. -->

# Plan: Jede Kamera in einem eigenen Prozess (`uc2_cam_worker`)

## Kontext

Mehrere `uc2_cam_py`-Kameras sollen sich stabil auf die CPUs verteilen, ohne dass die
Verwaltungssoftware etwas dafür tun muss. Die Erfassung (SDK-Callback → Kopie in den Ring) und
das Speichern (`FileStreamSink`) laufen künftig in einem **eigenen Prozess je Kamera**. Der
Hauptprozess (Python, Verwaltungssoftware) steuert über einen Socket und liest die Bilder
**kopierfrei aus Shared Memory**. Gewinn: Ein Absturz des Closed-Source-SDK trifft nur seine
Kamera. Alle Threads einer Kamera, auch die SDK-internen, gehören zu einem Prozess, den der
Kernel als Ganzes verteilt und der sich später gezielt binden lässt. Logs sind je Kamera
zuordenbar. Harte Anforderung: Beim Übergang, besonders beim Lesen der Bilder, darf es **keine
Zugriffsverletzungen** geben (kein SIGSEGV/SIGBUS im Hauptprozess, gleich was der
Kamera-Prozess tut).

Der Ring Buffer ist dafür vorbereitet (STATUS §7.1: zeigerfrei, POD, `UC2_MemoryOps`).

## Entscheidungen

- **Vom Nutzer abgenommen:**
  - `create_camera(..., separate_process=True)` ist der Standard, `False` = heutiger Weg im
    selben Prozess (für gdb und als zweiter Testpfad). Gilt für HikRobot und Simulated gleich.
  - Die CPU-Affinität kommt **später**, als Punkt in `Todos.md`.
- **Mein Vorschlag:**
  - Der Kamera-Prozess ist ein **eigenes C++-Programm** (`uc2_cam_worker`), kein Python. Damit
    braucht nicht jede Kamera einen Interpreter, es gibt keinen GIL, und kein Prozess, der schon
    Threads hat, wird geforkt.
  - Shared Memory über `memfd_create` + Seals, übergeben per `SCM_RIGHTS`. Aufwecken per
    `futex`. **Kein prozessübergreifender Mutex**, also kann auch kein toter Prozess eine Sperre
    halten. Das ersetzt den Plan `PTHREAD_PROCESS_SHARED` aus §7.3.
  - Die Enumeration (`list_cameras`/`list_devices`) bleibt in v1 im Hauptprozess.

```
Hauptprozess (Python)                                 uc2_cam_worker (je Kamera, C++)
Camera ─→ _hik.RemoteCamera ══ socketpair(SEQPACKET) ══ CamWorker ─→ CameraBase(HikCamera|SimCamera)
              │  Empfangs-Thread: Antworten, Logs, Ring-fd           ├─→ RingBuffer (memfd) ─→ FileStreamSink
              └─→ RingBuffer::attach(fd)  ── dieselben Seiten (MAP_SHARED) ──┘
                  readLatest/readSlot/waitForFrame lokal, ohne IPC
```

---

## Etappe 1 — Ring Buffer in Shared Memory (nur C++, noch kein zweiter Prozess)

Dateien: `core/include/UC2_RingBuff.h`, `core/src/UC2_RingBuff.cpp`, `core/include/CameraBase.h`,
`core/src/CameraBase.cpp`, neu `core/tests/test_ringbuffer_shm.cpp`, `core/tests/CMakeLists.txt`.

1. **`RingBuffer::Config::sharedMemory`** (Standard `false`). `configure()` belegt die Region
   dann so:
   - `memfd_create(MFD_CLOEXEC | MFD_ALLOW_SEALING)`
   - `ftruncate` + **`fallocate`**: Die Seiten werden sofort reserviert. Speichermangel zeigt
     sich dann als Fehler in `configure()` und nicht später als SIGBUS beim Seitenfehler.
   - Seals `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`: Niemand kann das Segment verkleinern,
     also gibt es kein SIGBUS beim Leser.
   - `mmap(MAP_SHARED)`.

   Umgesetzt als zweite interne `UC2_MemoryOps`-Implementierung neben `heapMemoryOps()`, mit
   neuem Getter `int sharedFd() const` (-1 bei Heap). `CameraBase::BufferConfig::sharedMemory`
   reicht den Schalter in `startLocked()` durch.
2. **`static std::shared_ptr<RingBuffer> RingBuffer::attach(int fd)`**: Leser-Rolle auf einer
   fremden Region.
   - `fstat` + `mmap`.
   - Die Prüfung muss erfolgreich sein, sonst `nullptr` + ERR, nie ein Absturz:
     - Magic, `layoutVersion == 2`, `headerBytes`
     - `slotCount` in [2, kMaxSlots]
     - `frameBytes == w·h·c·item` (mit `mulChecked`)
     - `slotStride` ≥ frameBytes und ausgerichtet
     - die aus dem Header berechnete Regionsgröße == `fstat`-Größe
     - Seals vorhanden (`F_GET_SEALS`)
   - In dieser Rolle sind `beginWrite`, `startRecording`, `startStreaming` und
     `registerConsumer` mit ERR abgewiesen. Lesen funktioniert.
   - Der Destruktor gibt die Abbildung frei: Die Lebensdauer des `shared_ptr` ist die
     Lebensdauer der Abbildung.
3. **Der Leser vertraut keinem Wert aus der geteilten Region, der Adressen bestimmt:**
   - `payloadAt()` rechnet ab sofort mit `m_payloadOffset + slot·m_slotStride` aus der eigenen,
     geprüften Geometrie (heute `rec.payloadOffset`, [UC2_RingBuff.cpp:617](core/src/UC2_RingBuff.cpp#L617)).
   - `acquireSlot()` begrenzt `nbytes` auf `frameBytes`.
4. **`waitForFrame()` per futex** auf einem neuen 32-Bit-Zähler `frameSignal` plus
   `frameWaiters` in `UC2_Control` (anstelle von `reserved0`/`pad0`; die Größe bleibt 64 B, die
   `static_assert`s bleiben). Producer: Zähler erhöhen, `FUTEX_WAKE` nur bei Wartenden.
   Ersetzt `m_frameReady` für beide Rollen, also ein einziger Mechanismus.
   `kLayoutVersion` 1 → 2.
5. Tests `[shm]`:
   - Lesen über `attach` liefert dieselben Bytes.
   - Manipulierte Header werden abgewiesen (Magic, Version, Größe, fehlende Seals).
   - `ftruncate` auf den fd scheitert mit EPERM.
   - Das angehängte Bild überlebt die Zerstörung des Producer-Rings.

   Tests `[shm][mt]`:
   - `fork()` vor jedem Thread: Das Kind schreibt Muster-Frames, der Elternprozess prüft sie
     mit `stillValid()`.
   - SIGKILL ans Kind mitten im Lauf: Der Elternprozess liest weiter, kein SIGBUS.
   - `waitForFrame` wacht prozessübergreifend auf.

## Etappe 2 — Kamera-Prozess und Protokoll (C++)

Neu:
- `core/include/CamIpc.h` + `core/src/CamIpc.cpp`: Nachrichten, Senden/Empfangen,
  `SCM_RIGHTS`.
- `core/include/RemoteCamera.h` + `core/src/RemoteCamera.cpp`
- `core/src/CamWorker.cpp`: `int runWorker(int fd, const WorkerArgs&)`
- `core/worker/uc2_cam_worker.cpp`: nur `main()`.

CMake:
- `core/CMakeLists.txt`: Target `uc2_cam_worker`, `-Wall -Wextra`, BUILD_RPATH = MVS-Lib-Ordner.
  Damit gibt es den Worker in **beiden** Konfigurationswegen.
- Root-`CMakeLists.txt`: `install(TARGETS uc2_cam_worker DESTINATION uc2_cam_py)` mit
  `INSTALL_RPATH $ORIGIN/_mvs/lib/<arch>`. DT_RPATH kommt über `hikcam_core` INTERFACE.

**Protokoll**
- `socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC)`. Das ist wichtig: Sonst erbt jeder
  weitere Kamera-Prozess die Socket-Enden der anderen und erkennt dann nie, dass der
  Hauptprozess weg ist.
- Gestartet wird per **`posix_spawn`**, nicht `fork`: Der Python-Prozess hat Threads.
  `adddup2` legt das Kind-Ende auf fd 3, es wird nichts weiter vererbt. Senden mit
  `MSG_NOSIGNAL`.
- Nachricht: Kopf {Magic, Protokollversion, Art (Request/Response/Event), Op, Id} plus typisierte
  Felder (u32/i64/f64/str). Der Leser prüft alle Grenzen; eine fehlerhafte Nachricht wird
  geloggt und abgewiesen, nie ein Absturz. Beim Start tauschen beide Seiten ein Hello mit der
  Protokollversion aus.
- Requests bilden die `CameraBase`-API 1:1 ab, dazu die Sim-Extras `spot_position`,
  `exposure_auto`, `gain_auto`.
- Events vom Worker:
  - `Log(level, text)`
  - `Ring(generation, fd)` mit fd per `SCM_RIGHTS`
  - `Fatal`
- **Reihenfolge:** Logs und Ring-Event eines Befehls gehen über denselben Socket **vor** dessen
  Antwort. Nach `start()` oder `set_roi()` hat der Hauptprozess also den neuen Ring, und
  `_logging.flush()` behält seine Bedeutung.
- Öffnen: Die Argumente (`--vendor --serial --connection-type --index` + Sim-Optionen)
  beantwortet der Worker mit `Opened(Identität)` oder `Error(Art, Text)`. Die Art wird wieder
  zu `invalid_argument`/`out_of_range`/`runtime_error`, also wirft Python wie heute
  ValueError/IndexError/RuntimeError.

**`RemoteCamera` (Hauptprozess)**
- Dieselben Methodensignaturen wie `CameraBase`, aber **nicht** davon abgeleitet: `start`/`read`
  von `CameraBase` sind nicht virtuell und besitzen Gerät und Ring.
- Ein Empfangs-Thread:
  - stellt Antworten dem wartenden Aufruf zu (Id → Promise),
  - gibt Logs mit Quellen-Id an `hik::logMessage` weiter,
  - beantwortet ein Ring-Event mit `RingBuffer::attach(fd)` und tauscht `m_ring` unter
    Mutex. Alte Ringe leben in bestehenden `Image`s weiter.
- `readLatest`, `readSlot`, `waitForFrame`, `sessionHeader` und `frames_written` laufen
  **lokal** auf dem angehängten Ring, ohne IPC je Bild.
- Aufrufe haben einen Timeout: 5 s, für start/stop/Geometrie/close 30 s. Bei Timeout oder EOF
  ist die Kamera **„verloren“**: einmal ERR mit Exit-Code oder Signal, danach liefern alle
  Aufrufe false/None, und `isRunning` ist false. **Bilder, die schon gelesen wurden, bleiben
  lesbar.**
- `close()`: Close-Request, `waitpid` mit 2 s, sonst SIGKILL + `waitpid`. Der Destruktor ruft
  `close()` auf, es bleiben also keine Zombies zurück.

**Worker**
- Ein Befehls-Thread führt die Requests auf `CameraBase` aus (`BufferConfig.sharedMemory = true`).
  Hat sich nach einem Befehl `ring()` geändert, schickt er ein Ring-Event.
- Log-Senke → Socket. Vom Befehls-Thread blockierend; von SDK-Callback und Writer
  `MSG_DONTWAIT`. Verworfene Nachrichten werden gezählt und gemeldet.
- **Lebensdauer:** Socket-EOF bedeutet, der Hauptprozess ist weg. Dann `cam->close()` (die
  `.uc2rb`-Datei wird sauber abgeschlossen) und Ende. Auf SIGTERM folgt derselbe Weg.
- **Kein `PR_SET_PDEATHSIG`:** Das Signal feuert schon, wenn der *Thread* endet, der den
  Prozess gestartet hat.
- **SIGINT wird ignoriert:** Ctrl+C trifft die ganze Prozessgruppe, schließen soll aber der
  Hauptprozess.
- `prctl(PR_SET_NAME, "uc2cam-<serial>")`, damit die Prozesse in top/htop erkennbar sind.

Tests `[worker]` (`core/tests/test_worker.cpp`, Pfad zum Worker per `$<TARGET_FILE:uc2_cam_worker>`):
- **Simulated-Zyklus:** öffnen, Bereiche und Setter, start, `waitForFrame`, Bildinhalt
  (Spot-Position), `setRoi` → neue Ring-Generation, das alte Bild bleibt lesbar, stop, close,
  der Prozess ist eingesammelt.
- **Fehler:** dieselben Ausnahmetypen; ein fehlendes Worker-Programm gibt `runtime_error`.
- **Robustheit:**
  - SIGKILL während der Aufnahme: Aufrufe scheitern sauber, das gehaltene Bild bleibt lesbar.
  - Schließt der Hauptprozess seinen Socket, endet der Worker, und die `.uc2rb`-Datei ist
    vollständig (Größe = 8448 + n·(64 + Frame)).
  - Fehlerhafte Nachrichten in beide Richtungen werden abgewiesen, kein Absturz.

## Etappe 3 — Python-Anbindung

Dateien: `bindings/hik_py.cpp`, `python/uc2_cam_py/{camera,hikrobot,simulated,_logging,defaults}.py`,
`_hik.pyi`, `tests/conftest.py`, neu `tests/test_process.py`, `tests/test_api_surface.py`, `tests/test_hik.py`.

1. **Bindings:** Die Kamera-Tabelle wird zur Vorlage `bindCameraApi<T>()` für `CameraBase` und
   `RemoteCamera`, ohne Duplikat. `rangeOrNone`/`valueOrNone`/`readImage` werden
   wiederverwendet; `ImageHandle` bleibt unverändert (hält `CameraBase::Image`, damit den
   angehängten Ring und damit die Abbildung).
   `_hik.RemoteCamera(worker_path, vendor, connection_type, serial, index, options)` ohne GIL,
   dazu `pid`.
2. **Logs je Kamera:**
   - Jeder Eintrag der `LogQueue` bekommt eine `source` (0 = Core im selben Prozess, n =
     `RemoteCamera`).
   - `_pop_log` liefert `(level, message, source)`.
   - `_logging` ordnet `source` dem Logger der Kamera zu (registrieren beim Erzeugen,
     abmelden bei `close`). Eine unbekannte Quelle geht wie heute an den aktiven Logger.

   Das erledigt den offenen Punkt aus §13 für den Prozess-Weg.
3. **Neue Parameter:**
   - `create_camera(..., separate_process: bool = True)` mit bool-Prüfung wie die anderen
     Schalter, dazu `defaults.SEPARATE_PROCESS = True`.
   - Die Hersteller-Klassen prüfen die Optionen wie bisher und erzeugen dann entweder
     `_hik.HikCamera`/`SimCamera` oder `_hik.RemoteCamera(_worker_path(), cls.VENDOR, ...)`.
   - `_worker_path()` sucht neben `_hik` wie `_sdk_env.bundled_sdk()`; fehlt das Programm,
     gibt es RuntimeError mit Hinweis.
   - Die SDK-Umgebung aus `_sdk_env` liegt schon in `os.environ` und wird an den Worker vererbt.
4. Neue Property `Camera.process_id -> int | None` (None im selben Prozess).
5. Ist der Worker verloren, laufen die bestehenden Fehlerwege (Setter → RuntimeError
   „refused“). `get_last_image` liefert das letzte Bild aus der alten Abbildung.

Tests:
- Die Fixture `camera` wird parametrisiert über **Simulated-Prozess**, **Simulated im selben
  Prozess** und HikRobot-Prozess (hardware). So läuft die ganze Schnittstellen-Suite über
  beide Wege.
- `test_process.py`:
  - Zwei Kameras laufen unter zwei verschiedenen PIDs, beide ≠ `os.getpid()`.
  - Nach `close()` ist der Prozess weg.
  - `kill -9` an den Worker: kein Absturz, `np.sum(img.data)` funktioniert, ERROR im richtigen
    Logger.
  - Die Logs von Kamera A und B landen in Logger A und B.
  - Interpreter-Ende ohne `close` (Subprozess mit `os._exit`): Der Worker ist nach 2 s weg,
    die `.uc2rb`-Datei ist vollständig.
  - SIGINT an die Prozessgruppe beendet den Worker nicht vor `close`.
- `test_hik.py`: Die SDK-aus-dem-Paket-Prüfung zusätzlich auf `/proc/<worker-pid>/maps`.
- `test_api_surface.py`: Signatur mit `separate_process`.

## Etappe 4 — Doku und Todos

- **`.claude/STATUS.md`:** neuer Eintrag oben (Überschrift, Datum, implementiert/committet).
  Außerdem:
  - §1 Schichtung
  - §3 Fallstricke: CLOEXEC/Vererbung, PDEATHSIG, SIGINT, Layout v2
  - §7.3 futex
  - §11.2.4 erledigt
- `Docs/RingBuff_REF.md` (Layout v2, `attach`, Shared Memory), `Docs/UC2_cam_py_interface.md`
  (`separate_process`, `process_id`), README.
- **`Todos.md`:**
  - CPU-Affinität je Kamera-Prozess (`cpu_affinity=None|'auto'|[Kerne]`), erst nach Messung
    auf dem Pi 5.
  - SDK nur noch im Kamera-Prozess laden (Enumeration auslagern).

## Bewusst nicht enthalten

- CPU-Affinität (→ Todos).
- SDK-Freiheit des Hauptprozesses: Die Enumeration bleibt dort.
- Registrierte Consumer im Hauptprozess (Wait-Policy über Prozessgrenzen): `CameraBase` nutzt
  `Overwrite`, die Pins des Hauptprozesses sind nur ein Hinweis.
- Payload nur lesbar (`PROT_READ`) abbilden.
- macOS/Windows: `separate_process=True` meldet dort einen klaren Fehler.
- `run_connection_test` und `HikStandAlone` bleiben unverändert.

## Verifikation

- `uv run c_tests`, inklusive `[shm]` und `[worker]`; Nur-Core-Weg `build/core`.
  `ctest --preset tsan` und `--preset asan` für `[shm]|[worker]`.
- `uv pip install -e . --no-build-isolation` → `uv run py_tests` (beide Wege),
  `uvx ruff check python/ tests/ tools/`, `uvx pyright`.
- **Von Hand:**
  - `uv run main` mit Debugfenster.
  - `ps -o pid,psr,comm -C uc2cam-SIM-0001` zeigt den Kamera-Prozess.
  - Zwei Sim-Kameras in einer Python-Sitzung: `top -H` zeigt die Last verteilt.
  - `kill -9` an den Worker bei offenem Fenster: Python läuft weiter.
- Das gebaute Wheel enthält `uc2_cam_py/uc2_cam_worker` mit Ausführungsrecht (`unzip -Z`).
- **Nutzer (Hardware):** `uv run py_tests -m hardware`. Zu prüfen: SDK-Enumeration im
  Hauptprozess bei geöffneter Kamera im Worker, GigE und USB3.
