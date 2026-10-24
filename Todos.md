
# Todos and next steps
- Currently MIT Licensed -> should that stay?
- ~~Logging~~ (erledigt 2026-10-18: C++-Meldungen gehen über eine Queue an Python `logging`, siehe STATUS §13)
    - ~~Set up package for proper logging (using python logging and feed log functions in c - parts -> done via import)~~
    - ~~replace log message~~
- Triggering
    - implement trigger option for the camera
- ~~Ring Buffering~~ (erledigt; seit 2026-10-18 mit der Kamera verbunden, siehe STATUS §13)
    - ~~Implement Ring buffer~~
- Connect everything to newswitch

- Review units of exposure time and gain
    - Gain in dB, Exp time in us?

- Test for different plattforms (ARM, IOS)
    - ARM (aarch64): das SDK (MVS 5.1.1) wird von `uv run fetch_mvs` nach `External/hik/MVS/lib/aarch64`
      eingespielt und von CMake auf ARM ausgewählt, ist aber noch nirgends gebaut oder getestet.
      Auf dem Raspberry Pi: `uv run fetch_mvs && uv run all_tests`, dann `uv run py_tests -m hardware`.

- Windows-Unterstützung: Paket `External/hik/MVS_Win_STD_5.1.1_261015.zip` liegt bereit, wird aber noch
  nicht genutzt (`fetch_mvs`, CMake-Suche, DT_RPATH/`dladdr`, `/proc/self/maps`-Test und `ImageWindow`
  sind Linux-spezifisch).

- SDK 4.9 (MVS 5.1.1) an der HikRobot-Kamera prüfen (bisher nur ohne Kamera getestet):
  `uv run py_tests -m hardware`, `uv run c_tests`, `uv run main`. Dabei mit `LD_DEBUG=libs` prüfen, dass auch
  die beim Öffnen einer Kamera nachgeladenen SDK-Teile aus `uc2_cam_py/_mvs` kommen.

- Lizenz: vor dem Veröffentlichen von Wheels (enthalten die HikRobot-Bibliotheken in `uc2_cam_py/_mvs`)
  klären, ob HikRobot die Weitergabe erlaubt.

- Threading
    - ~~currently "waitForFrame" blocks the caling thread~~ (erledigt: Aufnahme läuft im C++-Thread, Python liest aus dem Ring Buffer)
    - Pin reading thread to core

- Add Pixel sizes for hik Cam (if not given automatically)
An der Kamera prüfen: pytest -m hardware und UC2_DISPLAY_SECONDS=30 pytest -m "display and hardware". Das konnte ich hier nicht testen:
HikRobot-Knotennamen und Einheiten,
Binning-Werte (ich nehme an, der Enum-Wert ist der Binning-Faktor),
die Sensorgröße,
die Umstellung gepackter Pixelformate,
die Auswahl per Seriennummer.


## Framegrabber und captureApp
- Ja, beide werden noch genutzt, und keine der Dateien lässt sich ohne Umbau entfernen.

CaptureApp.cpp: die Aufnahme-Sitzung für Terminal-Programm und Python
Die Datei stellt listDevices() und runCapture(CaptureOptions) bereit.

Nutzer	Wofür
Examples/HikStandalone.cpp:107	Das Terminal-Programm macht nur die Argumente zu CaptureOptions und ruft hik::runCapture() auf. Die eigentliche Arbeit steckt in CaptureApp.
bindings/hik_py.cpp:424 / :439	Python-Funktionen list_devices() und run_connection_test()
python/uc2_cam_py/__main__.py:28	python -m uc2_cam_py ruft run_connection_test() auf
Python-Tests (tests/test_grabber.py, tests/test_hik.py)	benutzen list_devices() bzw. prüfen, dass beide Funktionen existieren
C++-Tests (test_captureoptions.cpp, test_sdk_smoke.cpp, test_framegrabber.cpp)	Standardwerte von CaptureOptions, listDevices()
FrameGrabber.cpp: die Aufnahme-Schnittstelle für Python
Die Klasse öffnet eine Kamera, startet die Aufnahme und liefert Frames ohne Kopie per waitForFrame(). Außerdem setzt sie ROI, Belichtung, Gain und Framerate.

Nutzer	Wofür
bindings/hik_py.cpp:178 ff.	Python-Klasse _hik.FrameGrabber, fast vollständig gebunden
python/uc2_cam_py/grabber.py:34	Die Python-Klasse Grabber baut darauf auf; auch exportiert in uc2_cam_py/__init__.py
C++-Tests (test_framegrabber.cpp, test_pixelinfo.cpp)	Grabber-Verhalten, Pixelformat-Informationen
Die C++-Programme benutzen FrameGrabber nicht: HikStandAlone geht über CaptureApp direkt auf MvsCamera, und RingBuffExample braucht keine Kamera.

Wie die Teile zusammenhängen (Schichtung laut STATUS §1):


Examples/HikStandalone.cpp ─┐
                            ├─→ CaptureApp ─→ MvsCamera ─→ MVS-SDK
bindings/hik_py ────────────┴─→ FrameGrabber ──┘
Was mir dabei aufgefallen ist:

Nur noch Python nutzt FrameGrabber. Es wird trotzdem in hikcam_core gebaut und von den C++-Tests abgedeckt. Das passt so, weil der Core die gemeinsame Basis für C++ und Python ist.
Ring Buffer noch nicht angebunden: Laut STATUS ist FrameGrabber der Punkt, an dem der Ring Buffer später angeschlossen wird.
Offene Befunde in STATUS §11.3 betreffen genau diese beiden Dateien:
CaptureApp.cpp:158-163: Der getchar()-Thread hängt, wenn kein Terminal da ist (P2).
FrameGrabber.cpp:266-268: setRoi() ignoriert den Rückgabewert von start() (P3).
CaptureApp.cpp: vier fprintf-Aufrufe statt der Log-Funktionen aus hikLog.h (P4).