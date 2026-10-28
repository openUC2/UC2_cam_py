# STATUS — UC2_cam_py

**Stand:** 2026-10-29 · Branch `main` · Basis-Commit `166e1de` = Tag `v2.0.0` (Änderungen noch nicht committet)
**Letzte Arbeitsschritte:** OME-TIFF-Export `uc2_cam_py.exporter`, Version 2.1.0 (oberster
Eintrag). Davor: 2.0.0 (`ImgData.data` immer 3D), 1.0.1 (Build ohne bzw. mit unbrauchbarem
MVS-SDK), Versionierung (1.0.0), Flush-Fehler, Rauschen im simulierten Bild.

Dieses Dokument ist die einzige Projektdokumentation für Claude-Sessions: §1–§4 sind die
Dauerinformation (Aufbau, Konventionen, Fallstricke, Buildbefehle), §5–§13 der aktuelle
Arbeitsstand. Vor nicht-trivialer Arbeit §1–§4 und §11 lesen.

## Umgesetzte Pläne (neueste zuerst)

### OME-TIFF-Export `uc2_cam_py.exporter` — Version 2.1.0 — 2026-10-29

**Implementiert:** ja · **Committet:** nein

- **Plan:** `~/.claude/plans/steady-coalescing-wand.md`, freigegeben 2026-10-29 mit Zusatz
  Beispielprogramm.
  - Vom Nutzer entschieden:
    - `export_as_ome_tiff` gibt ein **geöffnetes `tifffile.TiffFile`** zurück.
    - Der Frame-Header steht **zweifach** (OME `Plane` + Annotation **und** TIFF-Tag je Seite).
    - `tifffile` ist eine **feste Abhängigkeit**.
    - Die Version geht gleich auf **2.1.0**.
  - Annahme: „`uc2_cam_py.exportert`“ war ein Tippfehler → `uc2_cam_py.exporter`.
- **`_uc2rb.py` (privat):**
  - numpy-Strukturtypen `SESSION_HEADER` (8448 B) und `FRAME_HEADER` (64 B) mit festen Offsets
    wie in C++.
  - `read_session_header()` → `ImgMetaData`; `ValueError` bei falscher Magic, Version oder
    Headergröße (z. B. alter 512-B-Header).
  - `frame_records()`: erster Durchgang nur über die Köpfe; ein abgeschnittener letzter
    Datensatz wird mit WARNING weggelassen.
  - `read_frame()` → `(H, W, C)`.
- **`exporter.py`:**
  - `list_raw_files(folder=None)`: nicht rekursiv, absolut, sortiert. Bei `None` der
    Standardordner (fehlt er, dann `[]`). Fehlt ein angegebener Ordner, `FileNotFoundError`;
    eine Datei ergibt `NotADirectoryError`.
  - `export_as_ome_tiff(path, *, output_path=None)`:
    - schreibt `<name>.ome.tif` (über `.part` + `replace`) und gibt es geöffnet zurück
    - ein `TiffWriter.write()` je Bild, weil tifffile die `extratags` einer zusammenhängenden
      Serie nur auf deren erste Seite schreibt
    - eigenes OME-XML auf Seite 0, `metadata=None`; BigTIFF über ~4 GB; tifffile erst in der
      Funktion importiert (`import uc2_cam_py` lädt es nicht)
  - **Tags:** 65400 `FRAME_HEADER_TAG` je Seite, 65401 `SESSION_HEADER_TAG` auf Seite 0.
  - **OME-XML:**
    - Session als `MapAnnotation` (`openuc2.org/uc2_cam_py/session`)
    - je Bild `Plane` (`DeltaT` relativ zu `startMonoNs`) mit `MapAnnotation`
      (`…/frame`: `sequence`, Zeiten, `slot`, `flags`)
    - `AcquisitionDate`; `PhysicalSizeX/Y` = Pitch × Binning aus dem Kommentar
- **Tests `tests/test_exporter.py` (15):**
  - `list_raw_files` (Ordner, Standardordner, Fehler).
  - Leser gegen C++ (`read_session_header` == `cam.get_image_info()`).
  - Export echter Aufnahmen Mono16/Mono8 (Pixel, beide Header-Orte, Planes, `DeltaT`,
    PhysicalSize) sowie `output_path`/Ersetzen.
  - Künstliche Dateien: uint8/uint16/float64, RGB `(T, H, W, 3)`, Lücken in `sequence`,
    abgeschnittener Datensatz, falsche Eingaben (nichts halb Geschriebenes bleibt liegen).
  - Beispielprogramm `--demo` als Subprozess.
- **Beispiel `Examples/export_recordings.py`:** listet, exportiert und liest Session- und
  Frame-Header sowie ein Bild; `--demo` nimmt erst mit Simulated auf.
- **Doku:** Interface-Doku §9.1 (neu, mit Inhaltsverzeichnis), README („Exporting recordings“,
  Baum, Abhängigkeiten), CHANGELOG `[2.1.0]`, `VERSION` 2.1.0, `pyproject` (`tifffile>=2023.1.23`,
  installiert 2026.10.15) + `uv.lock`.
- **Verifikation:**
  - Das OME-XML (Mono, RGB, echte Aufnahme) ist **gültig** gegen das offizielle XSD
    `OME/2016-06/ome.xsd`: geprüft mit `xmlschema` über `uvx`, das XSD frisch von
    openmicroscopy.org.
  - Export 1920×1200, 104 Bilder, 457 MiB: 0,21 s (Plattencache warm), Spitzenspeicher +12 MiB.
  - `uv run all_tests` → C++ 138/138, Python 207 passed / 22 skipped; ruff sauber (auch
    `Examples/`), Pyright 0 Fehler.
- **Offen (Nutzer):**
  - Eine Export-Datei in Fiji öffnen (Bio-Formats, „Display OME-XML metadata“).
  - Tag `v2.1.0` nach dem Commit.

### `ImgData.data` immer 3D `(H, W, C)` — Version 2.0.0 — 2026-10-28

**Implementiert:** ja · **Committet:** ja (`166e1de`, Tag `v2.0.0`)

- **Plan:** `~/.claude/plans/steady-coalescing-wand.md`, freigegeben 2026-10-28.
  - Vom Nutzer entschieden: Der Bruch der Schnittstelle führt gleich zu **2.0.0**.
  - Wer `img.data[y, x]` als Zahl oder `imshow(img.data)` nutzte, muss umstellen auf
    `img.data[..., 0]`.
- **Binding `imageArray()`:** einzige Stelle, an der die Form entsteht. Immer
  `shape = {H, W, C}`, `strides = {W·C·item, C·item, item}`; die Fallunterscheidung für einen
  Kanal ist weg. Weiterhin kopierfrei und schreibgeschützt.
- **`_preview.to_display()`:** nimmt `(H, W)` und `(H, W, C)`. Es dünnt jetzt **vor** dem
  Kanalmittel aus (1920×1200 `(H, W, 1)`: 0,9 ms statt 2,9 ms mit der alten Reihenfolge).
- **Doku:**
  - `ImgData`-Docstring, Interface-Doku §6 und §8 (Tabelle der Pixelformate), README-Beispiel.
  - CHANGELOG `[2.0.0] - 2026-10-28` „Changed“ (breaking) mit Umsteigehinweis.
  - `VERSION` 2.0.0, Kopfzeile der Interface-Doku.
- **Tests:**
  - Formen auf `(…, 1)` angepasst: `test_images`, `test_settings`, `test_process`,
    `test_display`. `_centroid` und der Rausch-Test arbeiten auf `data[..., 0]`.
  - Neu: `test_data_is_always_three_dimensional` (Mono8/Mono16 × Prozess/in-process: Form,
    Strides, `data[..., 0]` teilt den Speicher und ist schreibgeschützt).
  - Neu: `test_to_display_shows_one_channel_like_a_2d_image`.
  - RGB liefert keine Testkamera.
- **Verifikation:**
  - `__version__` und `uc2_cam_worker --version` melden 2.0.0; Beispiel
    `(200, 400, 1) uint16 strides (800, 2, 2)`.
  - `uv run all_tests` → C++ 138/138, Python 192 passed / 22 skipped.
  - Display-Tests 2 passed; ruff sauber, Pyright 0 Fehler.
- **Tag:** `v2.0.0` (annotiert) auf `166e1de` gesetzt; das Pushen (`git push origin v2.0.0`)
  macht der Nutzer.

### Version 1.0.0 → 1.0.1 — 2026-10-27

**Implementiert:** ja · **Committet:** ja (`3437793`, Tag `v1.0.1`)

- **Plan:** `~/.claude/plans/steady-coalescing-wand.md`, freigegeben 2026-10-27.
- **Inhalt von 1.0.1:**
  - Build ohne MVS-SDK (`e6ca7ad`)
  - unbrauchbares `MVS_ROOT` → Build ohne HikRobot (Eintrag darunter, mit diesem Eintrag
    zusammen committen)
- **PATCH, nicht MINOR:** Ein Build, der vorher abbrach, gelingt jetzt. Die öffentliche
  Schnittstelle bleibt unverändert, neu sind nur interne `_hik`-Attribute. Im CHANGELOG stehen
  die Einträge deshalb unter „Fixed“.
- **Geändert:**
  - `VERSION` → `1.0.1`
  - `CHANGELOG.md`: „Unreleased“ → `[1.0.1] - 2026-10-27`; „Unreleased“ ist jetzt leer
  - Kopfzeile von `Docs/UC2_cam_py_interface.md`
  - Beispielwerte „1.0.0“ und die Hinweise „vor 1.0.0“ sind bewusst unverändert.
- **Verifikation:**
  - `__version__`, `_hik.__version__`, Metadaten und `uc2_cam_worker --version` melden 1.0.1.
  - `uv run all_tests`: CMake konfigurierte von selbst neu; C++ 138/138, Python 187 passed /
    22 skipped.
  - Das Wheel heißt `uc2_cam_py-1.0.1-cp312-…`.
- **Tag:** `v1.0.1` (annotiert) auf `3437793` gesetzt; das Pushen (`git push origin v1.0.1`)
  macht der Nutzer.

### Unbrauchbares `MVS_ROOT` → ohne HikRobot bauen; SDK-Prüfung beim Konfigurieren — 2026-10-27

**Implementiert:** ja · **Committet:** ja (`3437793`)

- **Plan:** `~/.claude/plans/steady-coalescing-wand.md`, freigegeben 2026-10-27.
  - Anlass: Ein äußeres Paket setzt `MVS_ROOT` per `config-settings-package`, und der Build
    scheitert.
  - Nachgestellt: Ein gesetztes `MVS_ROOT` ohne SDK ergab FATAL_ERROR (Regel aus dem Eintrag
    darunter). Gegen das vollständige `/opt/MVS` hier baute es fehlerfrei.
  - Vom Nutzer entschieden: Fehlende **und** unpassende SDKs werden abgefangen.
- **CMake (`core/CMakeLists.txt`):**
  - Ein gesetztes `MVS_ROOT` ist keine Pflicht mehr, nur noch `HIK_MVS=ON`.
  - Die WARNING nennt die Quelle und gibt einen passenden Hinweis.
  - `HIK_MVS_MISSING_REASON` (CACHE INTERNAL, einzeilig, `"`, `\` und `;` ersetzt, ≤ 400 Zeichen)
    geht als Definition an `_hik`.
- **Neu `core/cmake/MvsProbe.cmake`, `hik_mvs_probe()`:** zwei `try_compile`-Proben im Unterordner
  `mvs_probe` des Build-Baums.
  1. Die vier SDK-Quellen werden als STATIC_LIBRARY übersetzt.
  2. Ein Mini-Programm (`MV_CC_Initialize`/`GetSDKVersion`) wird gegen die Bibliothek gelinkt,
     mit `-rpath`.
  - Grund im Fehlerfall: die erste Zeile mit `error:`/`Fehler:`/`undefined reference`/….
  - Gecacht über eine SHA1-Signatur (SDK-Pfad, Zeitstempel von Header und Bibliothek, Compiler
    und Flags, SHA1 der Quellen, Header und von `MvsProbe.cmake` selbst). Unverändert kostet das
    0,3 s pro Configure ohne Prüfung; nach einer Änderung ~0,5 s (ccache).
- **Python:**
  - `_hik.MVS_MISSING_REASON` (mit SDK `""`).
  - `hikrobot.no_mvs_message()` ersetzt die Konstante `NO_MVS_MESSAGE`. Es nennt den Grund und
    als Abhilfe auch `MVS_ROOT`.
- **Tests:** Der Monkeypatch-Test prüft den Grund in Exception und Log; der Test ohne SDK prüft
  den Grund auf stderr; neu ist `test_a_build_with_the_sdk_has_no_missing_reason`.
- **Verifikation:**
  - Zwei künstliche SDKs:
    - `build/fake-mvs-oldheader` (nur `MV_CC_GetSDKVersion`): WARNING, Grund „fatal error:
      MvErrorDefine.h: …nicht gefunden“, Build und 119/119; mit `HIK_MVS=ON` FATAL_ERROR.
    - `build/fake-mvs-badlib` (echte Header, Stub-Bibliothek): WARNING, „undefined reference
      to `MV_CC_Initialize`“.
  - Wheels mit `-C cmake.define.MVS_ROOT=`:
    - `/opt/MVS`: mit SDK, 33 Dateien unter `_mvs/`.
    - `/nonexistent/MVS` und das unpassende SDK: gebaut, ohne `_mvs/`; der Import-ERROR nennt
      den Grund.
  - pytest gegen das Wheel mit unpassendem SDK: 181 passed / 25 skipped.
  - Mit SDK: `uv run all_tests` → C++ 138/138, Python 187 passed / 22 skipped; `build/core`
    134/134; `build/core-nomvs` 119/119; TSan und ASan 138/138; ruff sauber, Pyright 0 Fehler.
  - Den frischen Checkout (`build/fresh-checkout`) habe ich nicht neu gebaut. Sein Pfad
    „bundle fehlt“ ist derselbe wie beim fehlenden `MVS_ROOT`.
- **Doku:** README „Requirements“ (Prüfung, `HIK_MVS`, Beispiel `config-settings-package`),
  `External/hik/README.md`, CHANGELOG „Unreleased“.
- **Wegwerf-Verzeichnisse unter `build/`:** `fake-mvs-oldheader`, `fake-mvs-badlib`,
  `probe-*`, `probe-wheel-*`.

### Build ohne MVS-SDK: dann nur „Simulated“, mit ERROR beim Import — 2026-10-27

**Implementiert:** ja · **Committet:** ja (`e6ca7ad`)

- **Überholt** durch den Eintrag darüber: Ein ausdrücklich gesetztes `-DMVS_ROOT` ist **keine**
  Pflicht mehr.

- **Plan:** `~/.claude/plans/steady-coalescing-wand.md` (überschreibt den Versionierungsplan),
  freigegeben 2026-10-27.
  - Vom Nutzer entschieden: `create_camera()` mit dem Standard-Hersteller HikRobot wirft ohne
    SDK `RuntimeError` und weicht **nicht** auf Simulated aus.
  - Vorher brach der Build ohne `External/hik/MVS` mit FATAL_ERROR ab. Ein frischer Checkout
    war so nicht baubar, denn das SDK ist nicht in Git.
- **CMake:**
  - `HIK_MVS` = `AUTO` (Standard) | `ON` | `OFF` in `core/CMakeLists.txt`.
    - AUTO: Fehlt das SDK, gibt es eine WARNING statt des Abbruchs.
    - ON und ein ausdrücklich gesetztes `-DMVS_ROOT`: Das SDK ist Pflicht.
  - Ergebnis `HIK_HAVE_MVS`: als PUBLIC-Definition `HIK_HAVE_MVS=1/0`, dazu im Cache für die
    Wurzel. `HIKCAMTEST_MVS_*` werden ohne SDK geleert.
  - Ohne SDK entfallen: `MvsCamera/MvsError/CaptureApp/HikCamera.cpp`, Include, Bibliothek,
    DT_RPATH, die rpaths, `HikStandAlone` und die Installation von `_mvs/`.
  - `CMAKE_CONFIGURE_DEPENDS` auf `External/hik`: Ein später geholtes SDK löst das
    Neu-Konfigurieren selbst aus (geprüft mit `touch External/hik`).
- **C++:**
  - Neuer SDK-freier Header `DeviceSummary.h`; `DeviceInfo : DeviceSummary` (+ `raw`).
  - `CamWorker`: Ohne SDK wirft HikRobot `runtime_error` „…built without the MVS SDK“.
  - Bindings:
    - `_hik.HAVE_MVS`; Python-`DeviceInfo` = `DeviceSummary`
    - `list_devices` → `[]`, `initialize` → False, `sdk_path` → `""`
    - `run_connection_test` → RuntimeError; keine Klasse `HikCamera`
- **Python:**
  - `Camera._unavailable()`; `vendors()` und `list_cameras()` lassen nicht verfügbare
    Hersteller aus.
  - `hikrobot.NO_MVS_MESSAGE`: von `_create_backend` geworfen und von `create_camera` als
    „Opening a HikRobot camera failed: …“ geloggt.
  - `__init__`: ERROR beim Import (ohne Logging-Konfiguration über lastResort auf stderr).
- **Tests:**
  - Neu: `test_without_the_mvs_sdk_only_the_simulated_camera_is_available` (monkeypatch
    `HAVE_MVS=False`, läuft in jedem Build).
  - Neu: `test_a_build_without_the_sdk_says_so_…` (nur ohne SDK, Import-ERROR per Subprozess).
  - C++ „opening errors…“ mit einem Zweig ohne SDK.
  - SDK-gebunden (`needs_mvs`/`#if`): die 2 SDK-Pfad-Tests, der `_discover`-Fehlertest, die
    MVS-Testdateien.
  - **Abweichung vom Plan:** In `test_camera_helpers.cpp` sind **vier** Tests geklammert, nicht
    nur der PFNC-Vergleich. Auch die `pixelLayout`/`unpackedAlternatives`-Tests verwenden
    SDK-Codes (`PixelType_Gvsp_BayerGB10` u. a.), für die es keine Projektkonstanten gibt.
    Sie laufen im Build mit SDK.
- **Verifikation:**
  - Mit SDK: `uv run all_tests` → C++ 138/138, Python 186 passed / 22 skipped;
    `build/core` 134/134; `ctest --preset tsan`/`asan` 138/138; ruff sauber, Pyright 0 Fehler;
    das Wheel enthält 37 Dateien unter `_mvs/`.
  - Core ohne SDK (`build/core-nomvs`, `-DHIK_MVS=OFF`): keine Warnung, 119/119; der Worker
    hat kein `libMvCameraControl` und keinen RPATH.
  - Frischer Checkout ohne SDK:
    - `git ls-files -co --exclude-standard` → `build/fresh-checkout`, also mit den neuen,
      noch nicht committeten Dateien.
    - `uv build` → WARNING, Wheel ohne `_mvs/`.
    - venv `build/nomvs-venv`: Der Import zeigt den ERROR auf stderr, `vendors() ==
      ["Simulated"]`.
    - pytest gegen dieses Wheel: 181 passed / 24 skipped (ohne `test_devtasks.py`); die drei
      Tests für „ohne SDK“ sind grün.
  - Wegwerf-Verzeichnisse unter `build/`: `fresh-checkout`, `nomvs-dist`, `nomvs-venv`,
    `core-nomvs`.
- **Doku:** README „Requirements“, `External/hik/README.md`, Interface-Doku §8 HikRobot
  „Availability“ und `vendors()`, CHANGELOG „Unreleased“ (nächstes Release 1.1.0).

### Versionierung eingeführt: SemVer, eine Quelle `VERSION`, Version 1.0.0 — 2026-10-27

**Implementiert:** ja · **Committet:** ja (`005b393`, gemergt als `4b91fdd`)

- **Plan:** `~/.claude/plans/steady-coalescing-wand.md`, freigegeben 2026-10-27. Vom Nutzer
  entschieden:
  - SemVer `MAJOR.MINOR.PATCH`, Start bei **1.0.0** (statt 1.0.0.0).
  - Umfang: die Version in jeder Aufnahme, eine Versionsprüfung des Kamera-Prozesses,
    `CHANGELOG.md` und ein Git-Tag.
- **Vorher:** `0.2.0` stand von Hand an vier Stellen. CMake und C++ kannten keine Version, und
  es gab keine Tags.
- **Eine Quelle:** Die Datei `VERSION` (`1.0.0`).
  - `core/cmake/UC2Version.cmake` liest sie vor `project()` (in beiden Konfigurationswegen),
    prüft das Format und setzt `UC2_CAM_VERSION`. Über `CMAKE_CONFIGURE_DEPENDS` konfiguriert
    ein Build nach einer Änderung von selbst neu.
  - `pyproject.toml`: `dynamic = ["version"]` mit `scikit_build_core.metadata.regex` auf
    `VERSION`; `VERSION` steht auch in den uv-cache-keys. In `uv.lock` entfällt dadurch die
    feste Version des Projekts (bei dynamischer Version erwartet).
- **C++:** `hik::version()` (`UC2Version.h`, `src/Version.cpp`). Nur `Version.cpp` bekommt die
  Definition, ein Versionswechsel übersetzt also nur diese Datei.
- **Python:** `_hik.__version__`; `uc2_cam_py.__version__` wird daraus übernommen, also die
  Version des geladenen C++-Codes.
- **`uc2_cam_worker --version`** gibt `uc2_cam_worker 1.0.0` aus.
- **Aufnahmen:**
  - `UC2_SessionHeader::softwareVersion` (`char[32]`, Offset 8328, aus `reserved2`, das von 120
    auf 88 B schrumpft). Größe 8448 sowie `formatVersion` und `layoutVersion` bleiben gleich;
    ältere Dateien liefern `""`.
  - Die Version steht auch in der JSON-Begleitdatei (`softwareVersion`) und in
    `ImgMetaData.software_version` (neues Pflichtfeld nach `comment`).
- **Versionsprüfung:**
  - Das Hallo trägt jetzt die Softwareversion; `kProtocolVersion` 1 → 2.
  - `RemoteCamera` vergleicht sie mit `hik::version()`. Bei Abweichung oder fehlender Version
    beendet sie den Prozess und wirft `runtime_error` (in Python `RuntimeError`) mit beiden
    Versionen und dem Hinweis „reinstall uc2_cam_py“.
  - Dabei setzt sie `m_closing`, damit keine zweite, irreführende ERR „camera lost“ erscheint.
    Gefunden bei der Gegenprobe mit einem echten 1.0.1-Worker.
- **Tests:**
  - C++: Version im Kopf (`[ring]`, `[stream]`, auch in der JSON-Datei), Hallo mit Version
    (`RawWorker`), neuer Test „a camera process of another version is refused“.
  - Der Test-Helfer `core/tests/fake_worker.cpp` (Target `uc2_cam_fake_worker`) wird über
    `UC2_FAKE_WORKER_VERSION`/`UC2_FAKE_WORKER_PIDFILE` gesteuert. Geprüft werden: falsche
    Version, fehlende Version, kein übrig gebliebener Prozess, keine ERR.
  - Python: `__version__` = `VERSION` = `_hik.__version__` = Paket-Metadaten; neu:
    `uc2_cam_worker --version`; `software_version` in `get_image_info()`, im Dateikopf und in
    der JSON-Datei; `test_api_surface` angepasst.
- **Doku:**
  - `CHANGELOG.md` (neu, Keep a Changelog) und README-Abschnitt „Versioning“ (Regeln, Quelle,
    Release-Ablauf); `VERSION`/`CHANGELOG.md` im README-Baum.
  - Interface-Doku: 1.0.0, `software_version`, Versionsprüfung in §7.9.
  - `RingBuff_REF.md`: Kopftabelle, Sidecar.
- **Verifikation:**
  - `uv run all_tests` → C++ 138/138, Python 185 passed / 21 skipped.
  - `build/core` 134/134; `ctest --preset tsan` und `--preset asan` jeweils 138/138.
  - Ruff sauber, Pyright 0 Fehler. Das Wheel heißt `uc2_cam_py-1.0.0-cp312-…`, `METADATA`
    `Version: 1.0.0`.
  - Probe „eine Quelle“: `VERSION` auf 1.0.1 → CMake konfiguriert selbst neu, übersetzt wird
    nur `Version.cpp`, Python, `_hik`, Metadaten und Worker melden 1.0.1; danach zurück auf
    1.0.0.
  - Gegenprobe: Ein echter 1.0.1-Worker gegen die 1.0.0-Bibliothek ergibt einen `RuntimeError`
    mit klarer Meldung.
- **Hinweis TSan:** Wer die Testbinary direkt unter TSan startet, braucht `TSAN_OPTIONS` mit
  `tools/sanitizers/tsan.supp` wie im Preset. Sonst meldet der absichtlich „reißende“
  Seqlock-Test `test_ringbuffer_mt.cpp` Races.
- **Tag:** `v1.0.0` zeigt auf `4b91fdd` (Merge in `main`, `VERSION` = 1.0.0). Der Nutzer hat
  ihn von `1be0de2` dorthin versetzt.

### Flush-Marke wird nie verworfen · Offset und Poisson-Rauschen im simulierten Bild — 2026-10-27

**Implementiert:** ja · **Committet:** ja (`bda6bf3`)

- **Plan:** `~/.claude/plans/nested-wandering-jellyfish.md`, freigegeben 2026-10-27. Vom Nutzer
  entschieden: Das Rauschen folgt der Poisson-Statistik über die Normalnäherung N(λ, λ); der
  Offset beträgt 10 % der Spot-Höhe, und die Spot-Höhe sinkt, damit nichts abgeschnitten wird.
- **Flush (§11.3 P3):**
  - Ursache: `flush()` reihte seine Marke über `_emit_log()` ein. `queueSink` verwirft jede
    Meldung, solange die Queue `kLogQueueMax` = 10 000 Einträge hat.
  - Fix: Das neue Binding `_hik._emit_flush_marker(text)` hängt die Marke direkt an die
    `LogQueue`, vorbei am Limit und am Level-Filter. `_logging.flush()` benutzt es.
  - Neuer Test `test_flush_survives_a_full_log_queue` (deterministisch: Der Forwarder hängt,
    die Queue läuft über). Er ist vor dem Fix fehlgeschlagen und danach grün. Die 25er-Schleife
    über `tests/test_logging.py` hatte danach 0 Fehlschläge.
- **Simulation:**
  - `sim::renderNoisySpot(…, offset, seed)` in `SimPattern.h/.cpp`: λ = Offset + Spotwert,
    gezogen aus N(λ, λ) (`std::mt19937` + `normal_distribution<float>`), gerundet und auf den
    Wertebereich begrenzt. Die Gauß-Profile teilt sie über einen internen Helfer mit
    `renderSpot()`, das rauschfrei bleibt (`Examples/RingBuffExample.cpp`, exakte Tests).
  - `SimCamera::run()`: Spot-Höhe Mono16 54 000 (vorher 60 000), Mono8 180 (vorher 240),
    Offset 10 % davon (`kOffsetRatio`). Seed = Bildnummer der Sitzung, die Bilder sind also
    reproduzierbar. `spotPositionFor()` bleibt exakt.
  - Doku: `SimCamera.h`, `SimPattern.h`, `simulated.py`, `Docs/UC2_cam_py_interface.md` §8.
- **Tests:**
  - Neuer Header `core/tests/test_image_helpers.h`: `spotCentroid()` (Hintergrund = Median,
    Gewicht = max(v − Hintergrund − 0,2·(Max − Hintergrund), 0)) und `backgroundStats()`.
    Er ersetzt die lokalen `centroid()` in `test_simcamera.cpp` und `test_worker.cpp`.
  - `tests/test_images.py`: `_centroid` genauso; `test_view_survives_close` vergleicht nach
    `stop()` + `close()` mit einer Kopie statt `max() == 60000`.
  - Neu: C++ `renderNoisySpot adds an offset with shot noise` (Mittelwert und Varianz ≈ Offset
    für Mono16 und Mono8, Schwerpunkt, Seed, Begrenzung) und `the simulated image has an offset
    with shot noise`. Python `test_simulated_image_has_an_offset_with_shot_noise[Mono16|Mono8]`.
  - **Abweichung vom Plan:** Der Python-Test misst die Spot-Höhe als Mittelwert der 3×3 Pixel an
    der Spot-Position, nicht als Max − Median. Bei Mono8 (σ = 14 an der Spitze) läge das
    Maximum etwa 12 % zu hoch und damit außerhalb der ±10 %.
- **Verifikation:**
  - `uv run all_tests` → C++ 136/136, Python 184 passed / 21 skipped.
  - `build/core` 132/132. TSan und ASan/UBSan jeweils `[sim],[worker]` grün.
  - Ruff sauber, Pyright 0 Fehler.
  - `UC2_DISPLAY_SECONDS=10 uv run py_tests -m display`: 2 passed.
  - Gemessen bei 1920×1200 (x86): Mono16 und Mono8 halten 10 und 30 fps, im Kamera-Prozess wie
    im Hauptprozess.
- **Offen (Nutzer):** `uv run main` mit Debugfenster von Hand ansehen (Rauschen sichtbar?);
  Laufzeit auf dem Pi 5 (Display-Test bei 1920×1200).

### Jede Kamera in einem eigenen Prozess (`uc2_cam_worker`), Ring Buffer in Shared Memory — 2026-10-27

**Implementiert:** ja · **Committet:** ja (`7679220`)

- **Plan:** `.claude/currentPlan.md`, freigegeben 2026-10-25. Vom Nutzer entschieden:
  `separate_process` ist Standard und abschaltbar; die CPU-Affinität kommt später (→ Todos.md).
  Begründung: Linux verteilt die C++-Threads schon heute (sie halten den GIL nicht). Der Prozess
  bringt Abschottung vom Closed-Source-SDK, Kontrolle über *alle* Threads einer Kamera
  (SDK-intern inklusive) und Logs je Kamera.
- **Ring Buffer, Layout-Version 2:**
  - `Config::sharedMemory` → `memfd_create` + `fallocate` (Speichermangel als Fehler in
    `configure()`, nicht als SIGBUS) + Seals SHRINK|GROW|SEAL + `MAP_SHARED`; `sharedFd()`.
  - `RingBuffer::attach(fd)` prüft eine private Header-Kopie gegen Siegel und `fstat`-Größe.
    Danach kommen Geometrie, `payloadAt(slot)` (nicht mehr aus `rec.payloadOffset`) und
    `sessionHeader()` nur aus dieser Kopie; `nbytes` wird geklemmt. Ein angehängter Ring liest nur.
  - `waitForFrame()` per futex auf `UC2_Control::frameSignal`/`frameWaiters`, ersetzt
    `m_frameReady`; die Größe bleibt 576 B.
  - **Kein prozessübergreifender Mutex**; das weicht vom alten Plan `PTHREAD_PROCESS_SHARED` ab.
- **Protokoll** `CamIpc.h/.cpp`:
  - `SOCK_SEQPACKET`, 16-B-Kopf plus getaggte Felder, jeder Lesezugriff grenzgeprüft, ≤ 64 KiB.
  - fd per `SCM_RIGHTS` (`MSG_CMSG_CLOEXEC`); überzählige oder unerwartete fds werden geschlossen.
- **Kamera-Prozess** `CamWorker.cpp` + `core/worker/uc2_cam_worker.cpp`:
  - Ablauf: Hello → Open-Request → Befehle nacheinander.
  - **Abweichung vom Plan:** Hersteller und Optionen kommen im Open-Request, nicht per argv,
    damit Fehler als typisierte Antwort zurückkommen (ValueError/IndexError/RuntimeError wie im
    Prozess).
  - Vor jeder Antwort folgt ein Ring-Event, falls `ring()` gewechselt hat.
  - Logs gehen als Events: vom Befehls-Thread blockierend (also vor der Antwort), von anderen
    Threads `MSG_DONTWAIT`.
  - Ende bei Close, Socket-EOF, SIGTERM/SIGHUP; die Kamera wird sauber geschlossen, die
    `.uc2rb`-Datei ist vollständig. SIGINT wird ignoriert.
  - Name `uc2cam-<serial>`.
- **`RemoteCamera`** `RemoteCamera.h/.cpp`:
  - Start per `posix_spawn`: Das Kind-Ende liegt auf fd 3, sonst wird nichts vererbt;
    Signalmaske und -defaults werden zurückgesetzt.
  - Ein Empfangs-Thread; Timeouts 5 s/30 s; der Zustand (running, lastRestarted) steckt in jeder
    Antwort.
  - „Verloren“ bei EOF oder Timeout (beim Timeout zusätzlich SIGKILL): einmal ERR mit Exit-Code
    oder Signal.
  - Lesen läuft lokal am angehängten Ring. `close()`: `waitpid` 2 s, sonst SIGKILL.
- **Kleinere Erweiterungen:**
  - `hikLog`: `ScopedLogSource`/`currentLogSource()` (thread_local).
  - `CameraBase`: `BufferConfig::sharedMemory`, `streamStats()`.
  - `SimCamera`: `pixelFormatByName()`.
- **Bindings:**
  - Die Kamera-Tabelle ist jetzt die Vorlage `bindCameraApi<Cam>()` für `CameraBase` **und**
    `RemoteCamera`; alle Geräteaufrufe laufen ohne GIL.
  - `_hik.RemoteCamera(...)` mit `pid`, `is_lost`, `log_source` und den Sim-Extras.
  - `_pop_log` liefert `(level, message, source)`.
- **Python:**
  - `create_camera(..., separate_process=None)` → `defaults.SEPARATE_PROCESS` (True);
    `_process.py`.
  - Die Simulated-Optionen sind typgeprüft (TypeError auf beiden Wegen).
  - `Camera.process_id`; eine verlorene Kamera → `_require_open()` wirft RuntimeError.
  - `_logging` ordnet Meldungen nach `source` dem Logger ihrer Kamera zu. Das erledigt §13
    „mehrere Kameras → letzter Logger“ für den Prozess-Weg.
- **CMake:** `uc2_cam_worker` in `core/CMakeLists.txt` (beide Konfigurationswege), installiert
  nach `uc2_cam_py/` mit `INSTALL_RPATH $ORIGIN/_mvs/lib/<arch>`; Tests per `UC2_CAM_WORKER_PATH`.
- **Tests:**
  - Catch2 +25: `[shm]` 8, `[shm][mt]` 3 (fork; Kind per SIGKILL mitten im Frame), `[ipc]` 7,
    `[worker]` 7.
  - pytest +30: `test_process.py` 8, `test_hik.py` +1 (`/proc/<worker>/maps`), Fälle in
    `test_init`/`test_api_surface`. Die Fixture `camera` läuft über **Simulated (Prozess)**,
    **Simulated-in-process** und HikRobot.
- **Verifikation:**
  - `uv run all_tests` → C++ 134/134, Python 181 passed / 21 skipped.
  - `build/core` 130/130 · TSan 134/134 · ASan 134/134 (`build/asan` frisch konfiguriert, stammte
    noch aus `UC2_HIKpy`) · `[worker],[shm]` 5× hintereinander unter TSan.
  - Das Wheel enthält `uc2_cam_py/uc2_cam_worker` (`rwxr-xr-x`, DT_RPATH `$ORIGIN/_mvs/lib/64`).
  - `uv run main`: Debugfenster, Prozess `uc2cam-SIM-0001` sichtbar, nach dem Ende weg.
  - Ruff sauber. Pyright: Paket 0 Fehler, nur mit `uvx pyright --pythonpath .venv/bin/python`
    (ohne findet es die venv nicht); die Testdateien haben wie auf HEAD Fehler aus dem
    `dict(**SIM_OPTIONS)`-Muster.
- **Gefunden, nicht behoben (vorbestehend, außerhalb des Plans; behoben 2026-10-27, Eintrag darüber):**
  `test_logging.py::test_no_deadlock_when_stopping_under_log_load` scheitert sporadisch an
  `assert _logging.flush()` (5/25 Läufe). Die Flush-Marke wird verworfen, wenn die Log-Queue
  (10 000) gerade voll ist. Nachbau im selben Prozess: 2/25, das Problem ist also älter.
  Fix-Vorschlag: die Marke an der Grenze vorbei einreihen oder bei voller Queue die älteste
  Meldung verwerfen.
- **Offen (Nutzer):**
  - HikRobot im Kamera-Prozess an der Hardware (`uv run py_tests -m hardware`, GigE und USB3,
    Enumeration im Hauptprozess bei geöffneter Kamera), aarch64.
  - VS Code „Debug C++“ erreicht den Kamera-Code nicht mehr (→ README, Todos.md).

### HikRobot-SDK im Projekt (`External/hik/MVS`), unabhängig von `/opt/MVS`; SDK 4.7 → 4.9 — 2026-10-25

**Implementiert:** ja · **Committet:** ja (`1be0de2`)

- **Pakete und Import:** Der Nutzer hat MVS 5.1.1 (Linux x86_64, aarch64, Windows) als Zips in
  `External/hik/` gelegt. `uv run fetch_mvs` (`tools/devtasks/src/uc2_devtasks/mvs.py`) entpackt
  Zip → `.tar.gz` → `MVS.tar.gz` als Stream mit `tarfile`-Filter `"data"` und übernimmt nur
  `include/`, `lib/64`, `lib/aarch64`, `lib/CLProtocol/<arch>`, `license/` und HikRobots
  USB/GigE-Setup-Skripte (`scripts/`) nach `External/hik/MVS/` (92 MiB, `VERSION`). Nicht in Git
  (öffentliches Repo, keine Weitergabe-Erlaubnis in den Lizenzdateien); eingecheckt ist nur
  `External/hik/README.md`. Windows wird ignoriert (→ Todos.md).
- **CMake:** Quelle nur `External/hik/MVS` oder ausdrücklich `-DMVS_ROOT` — kein Rückfall auf
  `/opt/MVS`/`MVCAM_SDK_PATH` (auf Wunsch des Nutzers). Lib-Ordner nach Prozessor;
  `MVS_CAMERA_CONTROL_LIB` wird bei jedem Configure neu gesucht (alte Bäume schalten ohne
  `--fresh` um). `hikcam_core` gibt `LINKER:--disable-new-dtags` weiter (DT_RPATH).
  Wurzel-CMake installiert die Laufzeit nach `uc2_cam_py/_mvs/` — Symlinks als Kopie unter ihrem
  Namen, versionierte Ziele weggelassen (Wheel kann keine Symlinks) —, `_hik` hat
  `INSTALL_RPATH $ORIGIN/_mvs/lib/<arch>`. `pyproject.toml` cache-keys + `External/hik/MVS/VERSION`.
- **Machbarkeitsprüfung** (LD_DEBUG/strace mit eigenem ctypes-Probe): SDK 4.9 lädt seine
  Plug-ins immer neben `libMvCameraControl` (unabhängig von `LD_LIBRARY_PATH`/`MVCAM_*`), liest
  aber `ALLUSERSPROFILE` schon beim Laden; ohne ihn legt es den Cache unter `/GenICam/xml/cache`.
- **Laufzeit:** `MvsCamera.cpp` bestimmt das SDK per `dladdr(&MV_CC_Initialize)`, setzt
  `MVCAM_*` überschreibend, lädt den Renderer aus dem Architekturordner; Binding `_hik.sdk_path()`.
  `_sdk_env.py` (erster Import in `__init__.py`, mit `# isort: split`) setzt `ALLUSERSPROFILE` →
  `~/.cache/uc2_cam_py` und `MVCAM_*` → `_mvs`, **auch gegen** die Werte des MVS-Installers.
- **SDK 4.7.0.3 → 4.9.0.3:** alle 25 benutzten API-Funktionen vorhanden; baut ohne Warnung.
- Verifikation: `uv run all_tests` normal **und** mit sauberer Umgebung → C++ 109/109, Python 151
  passed / 21 skipped · `build/core` 105/105 · TSan 109/109 · `/proc/self/maps` ohne `/opt/MVS`,
  alle 10 Plug-ins aus `_mvs` · CMake-Fehlermeldung bei fehlendem SDK geprüft.
  **Offen (Nutzer):** Kamera mit SDK 4.9, aarch64 auf dem Pi.

### Speichern auf Platte zur Laufzeit umschalten — `cam.store_image_data = True/False` — 2026-10-25

**Implementiert:** ja · **Committet:** ja (`f37014b`)

- **Python:** `store_image_data` ist jetzt eine Property mit Setter (Entscheidung des Nutzers).
  Während der Aufnahme **nahtlos**: Ring, Sequenz, Live-Bild und Session laufen weiter;
  `True` öffnet eine neue `.uc2rb`-Datei ab dem nächsten Bild, `False` schreibt bis zum
  aktuellen Bild fertig und schließt sie. Ohne Aufnahme gilt der Wert ab `start()`. Einschalten
  prüft die Platte (`_check_disk()`, aus `_check_resources` herausgelöst) → `OSError`, Wert
  bleibt `False`; kein bool → `TypeError`; geschlossen → `RuntimeError`.
- **C++ `RingBuffer`:** `startStreaming(dir)` / `stopStreaming()` / `isStreaming()`. Der
  eingebaute `FileStreamSink` hängt nicht mehr in `m_sinks` (die Liste darf sich während einer
  Session nicht ändern), sondern lebt von `configure()` bis zum Destruktor und ist für den
  Producer über `std::atomic<FileStreamSink*> m_liveStream` erreichbar; `m_streamMtx`
  serialisiert Streaming mit `start/stopRecording()`.
- **C++ `FileStreamSink`:** Ende beim Stopp festhalten (`m_stopAt`) — sonst holt der Writer bei
  laufendem Producer nie auf. Dateiname vom Öffnungszeitpunkt, Zähler pro Datei, `filePath()`
  unter Sperre, `setDirectory()`.
- **C++ `CameraBase::setStoragePath(dir)`** (leer = aus), schaltet außerhalb von `m_mtx` um,
  damit `readLatest()` während `fsync` nicht blockiert; Binding `set_storage_path` ohne GIL.
- Tests: Catch2 +6 (`[ring][stream]` ein/aus, zweite Datei, früh gesetztes Verzeichnis,
  Fehlerfall; `[ring][stream][mt]` Stopp bei laufendem Producer; `[camera][sim][stream]` ×2),
  pytest +4 (`test_session.py`).
- **TSan:** Der MT-Test lief zuerst mit `Overwrite` und löste den dokumentierten, absichtlichen
  Payload-Wettlauf (§3, §11.1) aus, der nur für `test_ringbuffer_mt.cpp` unterdrückt ist → auf
  `Wait` umgestellt; danach 109/109 unter TSan, dreimal hintereinander. `build/tsan` dafür wie
  `debug` neu konfiguriert (`--fresh`, alter `catch2-subbuild` gelöscht). `asan`/`profile`
  stammen weiter aus `UC2_HIKpy`.
- Verifikation: `uv run c_tests` 109/109 · `build/core` 105/105 · `uv run py_tests` 150 passed,
  21 skipped · pyright 0 Fehler · Ruff sauber · von Hand: 1 s Speichern bei 10 fps → 10 Bilder,
  Dateigröße exakt `8448 + 10·(64 + 4 608 000)`, Aufnahme lief durch.

### Kurzbefehle `uv run main | py_tests | c_tests | all_tests`, venv repariert — 2026-10-23

**Implementiert:** ja · **Committet:** ja (`0555f9f`)

- uv 0.12 hat keinen Task-Runner; `uv run <name>` startet ein Skript aus `.venv/bin`. Die vier
  Befehle sind deshalb Konsolen-Skripte des Entwickler-Pakets `tools/devtasks`
  (`uc2-cam-py-devtasks`, Backend `uv_build`), eingebunden über die `dev`-Gruppe und
  `[tool.uv.sources]` (editable). Nicht in `[project.scripts]` von `uc2_cam_py`, sonst kämen sie
  ins Wheel. `main` zeigt direkt auf `uc2_cam_py.__main__:main`.
- `py_tests`/`c_tests` reichen Argumente an pytest/ctest durch; `all_tests` nimmt keine.
  `c_tests` konfiguriert `build/debug` bei Bedarf und baut vor dem Testen.
- **venv neu aufgebaut** (`rm -rf .venv && uv sync`): Seit der Umbenennung `UC2_HIKpy` →
  `UC2_cam_py` zeigten alle Shebangs in `.venv/bin` und `activate` auf den alten Pfad; `uv run
  pytest` ging nicht. Die Erweiterung ist danach ein Release-Build (Debug-Build für gdb: VS-Code-
  Task „Debug-Build (editable reinstall)“). README hat einen Troubleshooting-Punkt dazu.
- Test `tests/test_devtasks.py` (Skripte registriert, Projektwurzel, CMake-Cache lesen).
- Verifikation: `uv run main` (leere Eingabe → 0) · `uv run py_tests` 146 passed, 20 skipped ·
  `uv run c_tests` 103/103 · `uv run c_tests -R waitForFrame` 3/3 · `uv run all_tests` → 0;
  mit `PYTEST_ADDOPTS="-k gibt_es_nicht"` → „Python FAILED (exit 5)“, Exit 1 · Ruff sauber.

### Interaktives `python -m uc2_cam_py`, `list_cameras()`, Debugfenster wartet auf Bilder — 2026-10-23

**Implementiert:** ja · **Committet:** ja (`0555f9f`)

- **CLI** (`__main__.py`): `list_cameras()` → Tabelle `Index Name Vendor Connection Serial`
  (stdout), Index eingeben → `create_camera(vendor, serial, store_image_data=False,
  debug_window=True, start_on_init=True)`; Ende mit Enter (Daemon-Thread liest stdin), Ctrl+C
  oder Schließen des Fensters. Leere Eingabe/EOF beendet ohne Kamera. Alte Optionen
  `-i/-n/-s` entfallen; `run_connection_test()` bleibt als Funktion.
- **`list_cameras() -> list[CameraInfo]`** (öffentlich, `camera.py`): fragt jede registrierte
  Herstellerklasse über `Camera._discover()` ab (Standard `[]`). HikRobot aus
  `_hik.list_devices()`, Simulated genau ein Eintrag (Konstanten `MODEL`/`SERIAL` in
  `simulated.py`, `connection_type=""`). Reihenfolge = `vendors()` (HikRobot, dann Simulated).
  Fehler eines Herstellers → WARNING, die anderen werden trotzdem gelistet. **Ein neuer
  Hersteller implementiert nur `_discover()` in seiner Klasse.**
- **C++ `RingBuffer::waitForFrame(afterSequence, timeoutMs)`** auf dem schon vorhandenen
  `m_frameReady` (wurde in `commitWrite()` signalisiert, aber nie abgewartet);
  `CameraBase::waitForFrame` (ohne Ring: schläft `timeoutMs`), Binding `wait_for_frame` ohne GIL.
- **DebugWindow** wartet mit `wait_for_frame(last, 50)` statt fest zu pollen: neues Bild →
  sofort gezeichnet, höchstens 60/s (bei schnelleren Kameras das neueste); Fenster-Events
  mindestens alle 50 ms. Neue Property `is_open`.
- Tests: Catch2 +3 (`[ring]`, `[ring][mt]`, `[camera][sim]`), pytest +6 (`test_main.py`,
  `list_cameras`, `wait_for_frame`; Display-Test prüft, dass bei 10 fps jedes Bild gezeichnet wird).
- Nebenbei: `.vscode/launch.json` „Debug C++“ rief die CLI mit `-i 0 -n 20` auf → jetzt ohne
  Argumente. `build/debug` stammte noch aus `UC2_HIKpy` (CMake-Cache mit altem Pfad) →
  `cmake --preset debug --fresh` und `build/debug/_deps/catch2-subbuild` gelöscht; die Presets
  `asan`/`tsan`/`profile` sind vermutlich genauso betroffen (nicht angefasst).
- Verifikation: `ctest --preset debug` 103/103 (4 Hardware übersprungen) · `ctest --test-dir
  build/core` 99/99 · pytest `-m "not hardware"` 143 passed · Ruff/pyright sauber · CLI von
  Hand mit und ohne `DISPLAY`. Hardware und CLI an der HikRobot-Kamera prüft der Nutzer.

### `create_camera(..., debug_window=True)` — einziger Weg zum Vorschaufenster — 2026-10-21

**Implementiert:** ja · **Committet:** ja (`ccfb0e9`)

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

**Implementiert:** ja · **Committet:** ja (`ccfb0e9`)

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
core/include, core/src   C++-Core (namespace hik) — die eigentliche Logik; dazu RemoteCamera,
                         CamWorker (Kamera-Prozess) und CamIpc (Protokoll)
core/worker/             uc2_cam_worker: main() des Kamera-Prozesses (beide Konfigurationswege)
bindings/hik_py.cpp      pybind11-Modul `_hik`
python/uc2_cam_py/       Python-Paket: camera.py (Camera, create_camera), data.py (ImgData,
                         ImgMetaData), hikrobot.py/simulated.py, defaults.py,
                         _preview.py (Fenster für debug_window), _logging.py (Log-Forwarder),
                         _process.py (startet den Kamera-Prozess),
                         exporter.py (OME-TIFF-Export) + _uc2rb.py (Leser für .uc2rb),
                         _resources.py, _hik.pyi
core/tests/              Catch2-v3-Tests
tests/                   pytest-Tests
Examples/                Programme auf Basis des Core: HikStandalone.cpp (CLI, braucht Kamera),
                         RingBuffExample.cpp (ohne Kamera); nur Top-Level-Build, HIK_BUILD_EXAMPLES.
                         Dazu export_recordings.py (Python, Export-Beispiel, --demo ohne Kamera)
Docs/                    Referenzdokumentation (RingBuff_REF.md, Tooling.md)
VERSION                  die Versionsnummer (SemVer), einzige Quelle; CHANGELOG.md je Version
CMakePresets.json        Build-Varianten debug | profile | asan | tsan → build/<preset>
core/cmake/              HikTooling.cmake: Schalter HIK_SANITIZERS, HIK_PROFILING;
                         UC2Version.cmake: liest VERSION → UC2_CAM_VERSION
tools/                   analyze.sh (valgrind/perf), Suppressions für Sanitizer und valgrind,
                         devtasks/ (uv run main | py_tests | c_tests | all_tests | fetch_mvs)
External/hik/            HikRobot-Pakete (Zips) und daraus das SDK in MVS/ — beides nicht in Git,
                         nur README.md; installiert landet die Laufzeit in uc2_cam_py/_mvs/
.claude/STATUS.md        dieses Dokument
```

Schichtung — nicht kurzschließen:

```
python/uc2_cam_py (Camera) ─→ bindings/hik_py (_hik) ─┬─→ RemoteCamera ══ Socket ══ uc2_cam_worker ─→ CameraBase …
   (separate_process=True, Standard)                   │        └─→ RingBuffer::attach (memfd, nur lesen)
   (separate_process=False)                            └─→ CameraBase ─┬─→ HikCamera ─→ MvsCamera ─→ MVS-SDK
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
| **Die Umgebung eines MVS-Installers zieht nach `/opt/MVS`.** `~/.bashrc`/`~/.profile` exportieren `LD_LIBRARY_PATH=/opt/MVS/lib/...`, `MVCAM_*` und `ALLUSERSPROFILE=/opt/MVS/MVFG`. Gegenmittel: `--disable-new-dtags` (DT_RPATH vor `LD_LIBRARY_PATH`) an allem, was `hikcam_core` linkt; `python/uc2_cam_py/_sdk_env.py` setzt die Variablen **vor** dem Import von `_hik` — `ALLUSERSPROFILE` (GenICam-Cache) liest das SDK schon beim Laden der Bibliothek, aus C++ ist es dafür zu spät. Prüfen: `tests/test_hik.py::test_the_sdk_comes_from_the_package_not_from_an_installation`. | oben, Eintrag 2026-10-25 |
| **ROI/Binning während der Aufnahme starten die Session neu.** Der Ring Buffer friert seine Geometrie ein; `CameraBase` stoppt, ändert, startet neu → neuer Ring, neue `.uc2rb`-Datei. Jeder `start()` legt ohnehin einen neuen Ring an. | §13 |
| **Catch2 definiert `INFO`/`WARN` als Makros.** In Tests `hikLog.h` vor Catch2 einbinden und `(hik::INFO)(...)` schreiben, sonst zerlegt der Präprozessor Definition bzw. Aufruf. | `core/tests/test_log.cpp` |
| **Der Payload-Datenwettlauf im Ring Buffer ist Absicht.** Im `FullPolicy::Overwrite` kann ein Consumer Bytes lesen, die der Producer gerade überschreibt; `ReadHandle::stillValid()` ist der Weg, das zu erfahren. TSan, helgrind und drd melden es; für die MT-Tests ist es in `tools/sanitizers/tsan.supp` bzw. `tools/valgrind/hik.supp` gezielt unterdrückt (Docs/Tooling.md §5). `FullPolicy::Wait` vermeidet es ganz. | §11.1 |
| **Der Speicherblock des Ring Buffers muss zeigerfrei, vtable-frei und POD bleiben** — darauf beruht, dass die Anwendung ihn aus dem Kamera-Prozess abbildet. `static_assert`s in `UC2_RingBuff.cpp` sichern das ab. Jede Layoutänderung erhöht `kLayoutVersion` (heute 2), sonst weist `attach()` sie nicht ab. | §7.1, Docs/RingBuff_REF.md §12 |
| **Der Kamera-Code läuft standardmäßig in einem anderen Prozess.** Breakpoints in `CameraBase`/`HikCamera`/Ring-Producer/Writer trifft gdb unter `python -m uc2_cam_py` nicht mehr — `separate_process=False` (oder `defaults.SEPARATE_PROCESS = False`) bzw. `gdb -p <process_id>`. Ebenso läuft ein SDK-Absturz dort, nicht in Python. | README „Debugging“ |
| **Jeder fd der Anwendung zum Kamera-Prozess muss CLOEXEC sein** (Socket, memfd, empfangene fds über `MSG_CMSG_CLOEXEC`). Erbt ein zweiter Kamera-Prozess das Socket-Ende des ersten, sieht der erste nie EOF, wenn die Anwendung stirbt. **Kein `PR_SET_PDEATHSIG`:** Es feuert, wenn der *Thread* endet, der gestartet hat — eine Kamera aus einem kurzlebigen Python-Thread würde sterben. Das Ende der Anwendung erkennt der Kamera-Prozess am Socket-EOF. | `RemoteCamera::spawn()`, `CamWorker.h` |

---

## 4. Build- und Testbefehle

Kurzbefehle (Entwickler-Paket `tools/devtasks`, nur in der `dev`-Gruppe, nicht im Wheel):

```bash
uv run main                       # Kamera wählen, Live-Bild im Debugfenster (= python -m uc2_cam_py)
uv run py_tests [pytest-Args]     # 229 Tests, ~20 s (Hardware überspringt sich ohne Kamera)
uv run c_tests [ctest-Args]       # build/debug ggf. konfigurieren, bauen, ctest --preset debug
uv run all_tests                  # beides; beide laufen immer, Exit-Code ≠ 0 bei einem Fehlschlag
```

`c_tests` baut mit der CMake aus `build/debug/CMakeCache.txt` (`CMAKE_COMMAND`, heute
`/usr/bin/cmake` 4.3), nicht mit der aus der venv (4.4), die unter `uv run` zuerst im PATH steht.

Es gibt zwei Konfigurationswege, und **beide müssen funktionsfähig bleiben**:

```bash
# Top-Level über CMakePresets.json — das, was die IDE benutzt (cmake.useCMakePresets=always).
# Vier Presets, je ein Baum build/<preset>: debug (Standard, mit Python-Modul),
# profile (-O2 -g + Frame Pointer), asan (ASan+UBSan+LSan), tsan. Details: Docs/Tooling.md.
cmake --preset debug && cmake --build --preset debug
ctest --preset debug              # 138 Tests; ebenso --preset asan / tsan / profile

# Nur der Core, ohne Python-Modul und ohne Programme — schnellere Iteration auf der C++-Seite.
# Sanitizer/Profiling hier per -DHIK_SANITIZERS="address;undefined"|thread bzw. -DHIK_PROFILING=ON.
cmake -S core -B build/core -G Ninja -DHIK_BUILD_TESTS=ON
cmake --build build/core -j8
ctest --test-dir build/core       # 134 Tests (ohne [hardware])

# Ohne MVS-SDK (wie ein frischer Checkout): nur Simulated, 119 Tests. HIK_MVS=AUTO|ON|OFF.
cmake -S core -B build/core-nomvs -G Ninja -DHIK_BUILD_TESTS=ON -DHIK_MVS=OFF

# Analyse (valgrind, perf/hotspot, callgrind/kcachegrind, c2c): tools/analyze.sh, siehe Docs/Tooling.md.

# Python-Paket (editable). Nach C++-Änderungen nötig: Python importiert eine *Kopie*
# der .so in site-packages, ein reiner CMake-Rebuild reicht nicht. Ebenso nach dem Löschen
# oder Umbenennen einer .py-Datei: die Editable-Installation hält eine feste Dateiliste.
uv pip install -e . --no-build-isolation
uv run py_tests -q -m "not hardware"                     # ohne Kamera
uv run py_tests -q -m hardware                           # dieselben Fälle an der HikRobot-Kamera
UC2_DISPLAY_SECONDS=30 uv run py_tests -m display        # Live-Anzeige im Debugfenster
uvx ruff check python/ tests/ tools/                     # Lint, Konfiguration in pyproject.toml

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

**Ohne das MVS-SDK lässt sich nicht einmal konfigurieren.** Es liegt im Projekt unter
`External/hik/MVS` (nicht in Git): HikRobot-Zips nach `External/hik/`, dann `uv run fetch_mvs`
(siehe `External/hik/README.md`). `/opt/MVS` und `$MVCAM_SDK_PATH` werden **nicht** gelesen;
nur `-DMVS_ROOT=/pfad` überschreibt bewusst. Fehlt `MvCameraControl.h`, bricht CMake hart ab.
Aktuell MVS 5.1.1 = SDK 4.9.0.3, x86_64 (`lib/64`) und aarch64 (`lib/aarch64`), Auswahl nach
`CMAKE_SYSTEM_PROCESSOR`.

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
| **Multi-Prozess** | Layout jetzt shm-fähig, aber **nur Heap-Allokator** ausgeliefert | shm ist später eine zusätzliche VTable-Implementierung, keine Protokolländerung — **2026-10-27 umgesetzt** (memfd, `attach()`, Layout v2) |

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
- **`std::mutex`/`std::condition_variable` liegen außerhalb des Blocks** und bremsen nur den
  Producer im Wait-Modus (der im Kamera-Prozess läuft). **Umgesetzt 2026-10-27 ohne
  prozessübergreifenden Mutex:** `waitForFrame()` schläft per futex auf
  `UC2_Control::frameSignal`, das funktioniert über Prozessgrenzen, und ein toter Prozess kann
  keine Sperre halten. Der frühere Plan `PTHREAD_PROCESS_SHARED` ist damit hinfällig.
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
rb.waitForFrame(last, 50);                    // blockiert bis Sequenz > last oder 50 ms
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
4. ~~**Multi-Prozess**~~ — **erledigt 2026-10-27** (Eintrag oben): memfd statt `shm_open`, also
   keine Namen und nichts aufzuräumen nach einem Absturz; futex statt `PTHREAD_PROCESS_SHARED`.
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
| ~~P3~~ | ~~`_logging.flush()` gibt sporadisch `False` (2 s gewartet): die Flush-Marke wird verworfen, wenn die Log-Queue voll ist (`queueSink`, 10 000 Einträge)~~ | **erledigt 2026-10-27**: `_hik._emit_flush_marker()` reiht die Marke am Limit vorbei ein; Test `test_flush_survives_a_full_log_queue` |

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
