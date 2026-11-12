# Checkliste: erster Lauf auf Linux ARM (Raspberry Pi 5)

`uc2_cam_py` ist für aarch64 vorbereitet, aber bisher nur auf x86 gebaut und getestet worden.
Diese Checkliste führt durch den ersten Lauf auf einem Raspberry Pi 5 mit **Raspberry Pi OS
64-Bit** (Bookworm oder Trixie). Auf anderen aarch64-Linux-Systemen gilt sie sinngemäß, die
Paketnamen können abweichen.

Der wichtigste Punkt ist [§6 Speicherordnung](#6-speicherordnung-kern-der-prüfung). x86 ordnet
Speicherzugriffe streng, aarch64 nicht. Ab Version 2.1.1 sichern Fences den Seqlock des Ring
Buffers dagegen ab ([RingBuff_REF.md §1](RingBuff_REF.md#1-designvertrag)). Ob das auf echter
ARM-Hardware hält, kann nur ein Lauf dort zeigen.

Jeder Punkt hat einen **Befehl**, das **erwartete** Ergebnis und was bei einer Abweichung **sonst**
zu tun ist. Am Ende steht die [Ergebnistabelle](#11-ergebnis) zum Ausfüllen und Zurückmelden.

**Zeitbedarf:** etwa eine Stunde, das meiste davon Kompilieren. Die Hardware-Schritte in §9
brauchen eine HikRobot-Kamera; ohne Kamera fallen sie weg.

---

## 1. System

```bash
uname -m                        # aarch64
dpkg --print-architecture       # arm64
getconf PAGESIZE                # 16384 oder 4096
gcc --version | head -1         # ≥ 12
cmake --version | head -1       # ≥ 3.25
vcgencmd pmic_read_adc EXT5V_V  # Eingangsspannung, ≈ 5,0–5,1 V
od -An -tu4 --endian=big /proc/device-tree/chosen/power/max_current   # 5000
```

- **Erwartet:** `aarch64` und `arm64`.
- **Netzteil:**
  - `max_current` ist der Strom in mA, den die Firmware dem Netzteil zutraut. Erwartet ist
    `5000`, das liefert das offizielle 27-W-Netzteil (5,1 V / 5 A).
  - `3000` heißt 3 A: Für Bauen und Testen ohne Last reicht das meist.
  - `900` heißt: Das Netzteil kann keine höhere Leistung aushandeln (kein USB Power Delivery, oder
    ein Kabel von USB-A auf USB-C). Damit schaltet der Pi unter Volllast ab. Er ist dann nicht mehr
    erreichbar, und die LED leuchtet dauerhaft rot. So geschehen beim ersten Lauf in §6 mit
    `stress-ng`.
  - Unter 5000 dürfen die USB-Anschlüsse zusammen nur 600 mA abgeben. Das betrifft §9.
  - **Sonst:** Vor §6 mit Last und vor §9 das offizielle Netzteil anschließen und die Werte erneut
    prüfen.
- **Seitengröße:**
  - Der Standard-Kernel des Pi 5 (`kernel_2712.img`) nutzt 16-KB-Seiten, `kernel8.img` 4-KB-Seiten.
  - Bauen und Testen funktionieren mit beiden. Nur TSan und valgrind brauchen 4 KB (§8,
    [Tooling.md §7](Tooling.md#7-raspberry-pi-5)).
  - Den Wert für die Ergebnistabelle notieren.
- **Sonst:** `armhf` bei `dpkg` heißt 32-Bit-Userland, auch wenn der Kernel 64-Bit ist. Das wird
  nicht unterstützt: Raspberry Pi OS **64-Bit** installieren.

## 2. Pakete und uv

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git libx11-dev
curl -LsSf https://astral.sh/uv/install.sh | sh     # danach eine neue Shell öffnen
uv --version
```

- **Python:** Welche Version das System mitbringt, spielt keine Rolle (Bookworm 3.11, Trixie
  3.13). Das Projekt braucht 3.12 (`.python-version`), und `uv` lädt diese Version in §4 selbst.
- **Optional, für §6 und §8:** `sudo apt install stress-ng valgrind`.

## 3. Quellcode und MVS-SDK

```bash
git clone git@github.com:openUC2/UC2_cam_py.git
cd UC2_cam_py
git checkout v2.1.1             # oder der zu prüfende Commit
cp <Download-Ordner>/MVS-5.1.1_Linux_aarch64_20261016.zip External/hik/
uv run fetch_mvs
ls External/hik/MVS/lib/aarch64/libMvCameraControl.so*
```

- **Erwartet:**
  - `fetch_mvs` meldet `MVS-…_Linux_aarch64_….zip -> External/hik/MVS (aarch64)`.
  - Danach gibt es `External/hik/MVS/lib/aarch64/`.
- **Hinweis zur Seitengröße:** Die aarch64-Bibliotheken des SDK 5.1.1 sind auf 64 KiB
  ausgerichtet. Sie laden damit auch mit 16-KB-Seiten; am PC ist das mit `readelf -lW` für alle 27
  Bibliotheken geprüft.
- **Sonst:**
  - Meldet `fetch_mvs`, es gebe kein Paket: Der Dateiname muss unverändert
    `MVS-<Version>_Linux_aarch64_<Datum>.zip` lauten ([External/hik/README.md](../External/hik/README.md)).
  - **Ohne SDK** weitermachen geht auch: Dann gibt es nur die simulierte Kamera, und §9 entfällt.
    Alles andere ist trotzdem aussagekräftig.

## 4. Python-Paket

```bash
uv sync
uv run python -c "import uc2_cam_py as u; print(u.__version__, u._hik.HAVE_MVS, u.vendors())"
```

- **Erwartet:** `2.1.1 True ['HikRobot', 'Simulated']` und keine ERROR-Zeile.
- **Ohne SDK erwartet:** `2.1.1 False ['Simulated']` und eine ERROR-Zeile, die den Grund nennt.
- **Sonst:**
  - Kompilierfehler: die Ausgabe von `uv sync -v` sichern.
  - `HAVE_MVS` ist `False`, obwohl das SDK da ist:
    `uv run python -c "import uc2_cam_py as u; print(u._hik.MVS_MISSING_REASON)"` nennt den Grund.

## 5. C++-Build und Tests (Debug)

```bash
uv run c_tests
```

- **Erwartet:**
  - Beim Konfigurieren meldet CMake `Using MVS library: …/External/hik/MVS/lib/aarch64/libMvCameraControl.so`.
  - Am Ende steht `100% tests passed`. Die vier Hardware-Tests stehen ohne Kamera auf „Skipped“.
- **Lock-freie Atomics:** Die `static_assert`s auf lock-freie 32- und 64-Bit-Atomics in
  `UC2_RingBuff.cpp` greifen schon beim Kompilieren. Läuft der Build durch, sind sie erfüllt.
- **Sonst:** die komplette Ausgabe sichern. `c_tests` zeigt fehlgeschlagene Tests bereits mit ihrer
  Ausgabe (`--output-on-failure`).

## 6. Speicherordnung (Kern der Prüfung)

Geprüft wird, was der Ring Buffer verspricht: Ein Leser, der nach dem Lesen `stillValid()` mit
`true` beantwortet bekommt, hat nie eine Mischung aus zwei Bildern gesehen. Dazu laufen drei Tests
im optimierten Build:
- „a frame that still validates is never torn“
- „readSlot and readLatest are safe from several threads at once“
- „a frame from another process that still validates is never torn“

```bash
cmake --preset profile && cmake --build --preset profile
ctest --preset profile -R "never torn|readSlot and readLatest" --repeat until-fail:200 --output-on-failure
```

- **Erwartet:** keine Fehlermeldung. Am PC dauern 200 Runden etwa 1,5 Minuten, auf dem Pi
  entsprechend länger.
- **Mit Last (optional):** Unter Last werden die Threads öfter unterbrochen und wandern zwischen
  den Kernen. Das erhöht die Chance, dass eine Umordnung sichtbar wird.

  ```bash
  stress-ng --cpu 4 --timeout 15m &
  ctest --preset profile -R "never torn|readSlot and readLatest" --repeat until-fail:200 --output-on-failure
  ```

- **Sonst:** Ein `FAILED` bei `corrupt.load() == 0` bzw. `corrupt == 0` ist genau der Fehler, um
  den es hier geht. Bitte die Ausgabe und `git rev-parse --short HEAD` zurückmelden.

- **Falls hier der RPI einfriert**, also keine neuen Zeilen mehr kommen und nur noch Stromlos-Machen
  hilft:
  - **Vorbeugen:**
    - Ein Netzteil mit 5 V / 5 A (das offizielle 27-W-Netzteil) und eine aktive Kühlung
      verwenden. Unter `stress-ng` laufen alle vier Kerne auf Volllast.
    - Ctrl+C beendet `stress-ng` nicht. Es läuft im Hintergrund weiter, bis seine 15 Minuten um
      sind, und jeder neue Start legt vier weitere Lastprozesse dazu. Nach dem Lauf und vor jedem
      neuen Versuch deshalb `pkill stress-ng` ausführen.
    - An den `ctest`-Aufruf `--timeout 120` anhängen. Die Presets setzen kein Timeout. Ohne diese
      Option liefert ein hängender Test bis zu 25 Minuten lang keine Zeile und sieht dann aus wie
      ein eingefrorener Pi.
  - **Dass es gerade #105 trifft, sagt wenig.** Der Test braucht über 95 % der Laufzeit der drei
    Tests, ein Einfrieren fällt also fast immer zufällig in ihn.
  - **Eingrenzen:**
    1. Die Logs des abgestürzten Laufs ansehen, direkt nach dem nächsten Hochfahren:
       ```bash
       journalctl -b -1 -k --no-pager | grep -iE 'voltage|thermal|throttl|oom|panic|bug|watchdog' | tail -20
       journalctl -b -1 --no-pager | tail -30
       ```
       `Undervoltage detected!` heißt: Das Netzteil reicht nicht. Liefert `-b -1` nichts,
       speichert das System seine Logs nicht über einen Neustart hinweg.
    2. Beim nächsten Hänger vom PC aus prüfen, ob der Pi noch erreichbar ist: `ping <pi-adresse>`
       und `ssh <user>@<pi-adresse>`. Ist er erreichbar, hängt nur der Test. Dann bitte diese
       Ausgaben zurückmelden (`gdb` gegebenenfalls mit `sudo apt install gdb` installieren):
       ```bash
       ps -eo pid,stat,pcpu,etime,args | grep -E 'hikcam|stress' | grep -v grep
       vcgencmd measure_temp; vcgencmd get_throttled
       sudo gdb -p <PID von hikcam_core_tests> -batch -ex 'thread apply all bt' 2>&1 | tail -60
       ```
    3. Die Tests ohne Last laufen lassen, also den ersten `ctest`-Befehl dieses Abschnitts.
    4. Die Last ohne Tests laufen lassen und in einer zweiten Shell mitlesen:
       ```bash
       stress-ng --cpu 4 --timeout 15m &
       watch -n 2 'vcgencmd measure_temp; vcgencmd get_throttled'
       ```
       `get_throttled` sollte `0x0` zeigen. Die einzelnen Bits bedeuten:
       - `0x1`: Unterspannung jetzt; `0x10000`: Unterspannung seit dem Start
       - `0x4`: Takt wird gerade gedrosselt
       - `0x8`: Temperaturgrenze erreicht
  - **Auswertung:**
    - Friert Schritt 4 allein ein, oder melden Schritt 1 bzw. 4 Unterspannung oder Drosselung,
      liegt es an Netzteil oder Kühlung, nicht am Paket.
    - Läuft Schritt 3 durch und friert der Pi nur mit Last ein, gilt dasselbe.
    - Ist der Pi erreichbar und hängt nur der Test, ist es ein Fehler im Paket. Dann bitte die
      Ausgaben aus Schritt 2 zurückmelden.
    - Friert auch Schritt 3 ein und ist der Pi nicht mehr erreichbar, liegt es möglicherweise am
      Kernel. Dann bitte die Logs aus Schritt 1 zurückmelden und den 4-KB-Kernel probieren
      (siehe §8).

**Gegenprobe (optional):** Derselbe Lauf auf dem Stand vor dem Fix, `v2.1.0`, in einem eigenen
Worktree:

```bash
git worktree add ../uc2_v2.1.0 v2.1.0
cd ../uc2_v2.1.0
cmake --preset profile -DHIK_MVS=OFF && cmake --build --preset profile
ctest --preset profile -R "never torn|readSlot and readLatest" --repeat until-fail:500
cd - && git worktree remove --force ../uc2_v2.1.0
```

Wie das Ergebnis zu lesen ist:
- **Schlägt `v2.1.0` fehl und 2.1.1 nicht**, finden die Tests den Fehler, und der Fix wirkt.
- **Bleibt auch `v2.1.0` grün**, sagt das nichts aus: Die Umordnung ist selten und lässt sich
  nicht erzwingen.

## 7. Python-Tests ohne Kamera

```bash
uv run py_tests -m "not hardware"
```

- **Erwartet:** alle bestanden, einige übersprungen. Die `display`-Tests werden ohne Bildschirm
  übersprungen.
- **Was das mit abdeckt:** Die Kamera läuft standardmäßig in einem eigenen Prozess
  (`uc2_cam_worker`). Die Tests prüfen also auch memfd, Versiegelung und den Socket zwischen den
  Prozessen auf ARM.
- **Sonst:** die Ausgabe der fehlgeschlagenen Tests sichern.

## 8. Sanitizer

**ASan** läuft mit dem Standard-Kernel:

```bash
cmake --preset asan && cmake --build --preset asan -j2 && ctest --preset asan
```

**TSan braucht auf dem Pi 5 den 4-KB-Kernel.**
- Mit dem Standard-Kernel scheitert schon `cmake --build --preset tsan`, und zwar beim Schritt
  `Linking CXX executable core/tests/hikcam_core_tests`:

  ```
  FATAL: ThreadSanitizer: unsupported VMA range
  FATAL: Found 47 - Supported 39, 42 and 48
  CMake Error at …/CatchAddTests.cmake:70 (message): Error running test executable
  ```

- Warum das so ist:
  - Der Standard-Kernel (`kernel_2712.img`, 16-KB-Seiten) nutzt einen virtuellen Adressraum von
    47 Bit. Die TSan-Laufzeit von GCC 14 kennt auf aarch64 nur 39, 42 und 48 Bit, deshalb bricht
    jedes TSan-Programm sofort ab.
  - Schon der Build scheitert, weil CMake das frisch gelinkte Testprogramm einmal startet, um die
    Testliste zu erstellen (`catch_discover_tests`).
  - Das liegt am Kernel, nicht am Code.

Ablauf mit dem 4-KB-Kernel (`kernel8.img`, 39 Bit):

```bash
tail -5 /boot/firmware/config.txt                 # der letzte Abschnitt sollte [all] sein
echo 'kernel=kernel8.img' | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

```bash
getconf PAGESIZE                                  # 4096
cmake --preset tsan && cmake --build --preset tsan -j2 && ctest --preset tsan
```

**valgrind** braucht ebenfalls 4-KB-Seiten. Am besten läuft es gleich hier mit, siehe
[Tooling.md §2 und §7](Tooling.md).

**Zurück zum Standard-Kernel:** die Zeile `kernel=kernel8.img` wieder aus
`/boot/firmware/config.txt` entfernen und neu starten. Den Standard-Kernel nutzen die Anwender,
deshalb gehören die übrigen Schritte der Checkliste auf ihn.

`-j2` lässt nur zwei Compiler gleichzeitig laufen. Sanitizer-Builds brauchen viel Arbeitsspeicher,
und ein Build auf allen vier Kernen belastet den Pi wie `stress-ng`. Mit einem schwachen Netzteil
kann er dabei abschalten (§1).

- **Erwartet:** `100% tests passed` in beiden Läufen. Die vier Hardware-Tests stehen auf
  „Skipped“.
- **Ab 2.1.2 behoben:** „stopStreaming finishes although the producer keeps writing“ scheiterte
  bis 2.1.1 unter TSan in etwa einem Viertel der Läufe, am PC wie am Pi. Das war kein
  Timing-Problem des Tests, sondern ein echter Fehler im Wait-Modus: Meldete sich ein Consumer
  an, während der Producer schrieb, konnte der Producer bis `waitTimeoutMs` stehen bleiben
  (CHANGELOG 2.1.2). Tritt der Fehler mit 2.1.2 noch auf, bitte melden, mit der Ausgabe von:

  ```bash
  ctest --preset tsan -R "stopStreaming finishes" --output-on-failure
  ```

- **Sonst:**
  - Bricht ASan mit dem Standard-Kernel beim Start ab, etwa mit „unexpected memory mapping“,
    liegt das ebenfalls am Speicherlayout des Kernels. Dann ASan wie TSan mit `kernel8.img`
    laufen lassen und das in der Ergebnistabelle vermerken.
  - Meldet der Linker `undefined reference` auf praktisch alle Catch2-Funktionen
    (`Catch::AssertionHandler…`, `Catch::StringRef…`), ist der Build-Baum beschädigt. Das kommt
    typischerweise von einem harten Ausfall des Pi, nachdem er Dateien geschrieben hatte (§1,
    Netzteil). `--clean-first` reicht dann nicht. Den Baum ganz löschen und neu bauen; CMake lädt
    Catch2 dabei erneut herunter, es braucht also Netz:

    ```bash
    rm -rf build/tsan                             # bzw. build/asan
    cmake --preset tsan && cmake --build --preset tsan -j2
    ```

## 9. Mit HikRobot-Kamera

**Stromversorgung zuerst:** Eine USB3-Kamera braucht das 5-A-Netzteil, `max_current` muss in §1
also `5000` zeigen. Erkennt der Pi ein schwächeres Netzteil, dürfen alle USB-Anschlüsse zusammen
nur 600 mA abgeben. Die Kamera bekommt dann zu wenig Strom: Sie bricht ab, verliert Bilder oder
wird gar nicht erkannt. Die Grenze mit `usb_max_current_enable=1` aufzuheben hilft dabei nicht,
denn das Netzteil liefert den Strom trotzdem nicht. Eine GigE-Kamera mit eigener Versorgung (PoE
oder Netzteil) betrifft das nicht.

Einmal je Rechner, mit root: HikRobots Skripte für USB3-Kameras. Für GigE-Kameras sind es
`set_rp_filter.sh` und `set_socket_buffer_size.sh`. Die Skripte vor dem Ausführen lesen.

```bash
sudo External/hik/MVS/scripts/set_usb_priority.sh
sudo External/hik/MVS/scripts/set_usbfs_memory_size.sh
```

Danach die Kamera neu einstecken und die Tests sowie die Live-Ansicht starten:

```bash
uv run pytest -m hardware       # die Python-Tests mit echter Kamera
uv run c_tests                  # jetzt laufen auch die vier Hardware-Tests
uv run main                     # Kamera wählen, Live-Bild; Enter beendet
```

**Aufnahme bei voller Auflösung**, auf eine SSD/NVMe. Eine SD-Karte ist für Rohdaten zu langsam.

```bash
uv run python - <<'EOF'
import logging, time, uc2_cam_py
logging.basicConfig(level=logging.INFO)
with uc2_cam_py.create_camera(store_image_data=True, image_file_path="/mnt/ssd/uc2") as cam:
    cam.start(); time.sleep(10); cam.stop()
EOF
```

- **Erwartet:**
  - Die Hardware-Tests sind bestanden.
  - Die Live-Ansicht läuft flüssig.
  - Die Log-Zeile `Recording to disk stopped: … (N images saved, 0 dropped)` erscheint.
- **Sonst:**
  - `dropped > 0` heißt, der Datenträger kommt nicht mit. Bildrate, Auflösung und Datenträger
    notieren.
  - Bricht die Kamera ab oder fehlen Bilder in der Live-Ansicht: Prüfen, ob die USB-Skripte oben
    gelaufen sind, und `lost_packets` im Log ansehen (README, „What's the info in
    lost_packets?“).
- **Vorschaufenster:** `uv run pytest -m display` am Desktop des Pi. Unter Wayland läuft das
  Fenster über XWayland.

## 10. OME-TIFF-Export

```bash
uv run python Examples/export_recordings.py --demo
uv run python Examples/export_recordings.py /mnt/ssd/uc2        # die Aufnahmen aus §9, falls vorhanden
```

- **Erwartet:** je Aufnahme eine `.ome.tif`. Dazu gibt das Skript Form und dtype der Bilder, die
  Kamera und die ersten Frame-Header aus.

## 11. Ergebnis

Systemdaten einsammeln:

```bash
{ date -I; tr -d '\0' </proc/device-tree/model; echo; . /etc/os-release; echo "$PRETTY_NAME"
  uname -r; getconf PAGESIZE; gcc --version | head -1; cmake --version | head -1
  vcgencmd pmic_read_adc EXT5V_V; od -An -tu4 --endian=big /proc/device-tree/chosen/power/max_current
  git rev-parse --short HEAD; cat External/hik/MVS/VERSION; } 2>/dev/null
```

Diese Ausgabe und die ausgefüllte Tabelle zurückmelden. Damit lässt sich STATUS §11.2 Punkt 6
(„ARM-Lauf“) schließen.

| § | Punkt | Ergebnis (ok / Fehler / übersprungen) | Bemerkung |
|---|---|---|---|
| 1 | System, Seitengröße, Netzteil (`max_current`) | | |
| 3 | SDK `lib/aarch64` | | |
| 4 | Import, `HAVE_MVS` | | |
| 5 | `uv run c_tests` | | |
| 6 | Speicherordnung, 200 Runden | | |
| 6 | dasselbe unter Last | | |
| 6 | Gegenprobe `v2.1.0` | | |
| 7 | `py_tests -m "not hardware"` | | |
| 8 | ASan | | |
| 8 | TSan (mit `kernel8.img`) | | |
| 9 | Hardware-Tests (Python, C++) | | |
| 9 | Aufnahme: Auflösung, Bildrate, `dropped` | | |
| 9 | Vorschaufenster | | |
| 10 | OME-TIFF-Export | | |
