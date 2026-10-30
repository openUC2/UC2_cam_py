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
```

- **Erwartet:** `aarch64` und `arm64`.
- **Seitengröße:**
  - Der Standard-Kernel des Pi 5 (`kernel_2712.img`) nutzt 16-KB-Seiten, `kernel8.img` 4-KB-Seiten.
  - Bauen und Testen funktionieren mit beiden. valgrind braucht 4 KB
    ([Tooling.md §7](Tooling.md#7-raspberry-pi-5)).
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

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

- **Erwartet:** `100% tests passed` in beiden Läufen. Die vier Hardware-Tests stehen auf
  „Skipped“.
- **Bekannt, kein ARM-Problem:** „stopStreaming finishes although the producer keeps writing“
  scheitert unter TSan auch am PC immer wieder: In etwa einem Viertel der Läufe hat der
  Schreib-Thread nach 20 ms noch kein Bild gespeichert (STATUS §11). Tritt nur dieser Fehler auf,
  den Test einzeln wiederholen:

  ```bash
  ctest --preset tsan -R "stopStreaming finishes"
  ```

- **Sonst:**
  - Bricht ein Sanitizer schon beim Start ab, etwa mit „unexpected memory mapping“ oder
    „unsupported VMA range“, liegt das am Speicherlayout des Kernels, nicht am Code.
  - Ausweg: den 4-KB-Kernel aktivieren ([Tooling.md §7](Tooling.md#7-raspberry-pi-5)) und den
    Schritt wiederholen. Bitte in der Ergebnistabelle vermerken.
- **valgrind** geht nur mit 4-KB-Seiten, siehe [Tooling.md §2 und §7](Tooling.md).

## 9. Mit HikRobot-Kamera

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
  git rev-parse --short HEAD; cat External/hik/MVS/VERSION; } 2>/dev/null
```

Diese Ausgabe und die ausgefüllte Tabelle zurückmelden. Damit lässt sich STATUS §11.2 Punkt 6
(„ARM-Lauf“) schließen.

| § | Punkt | Ergebnis (ok / Fehler / übersprungen) | Bemerkung |
|---|---|---|---|
| 1 | System, Seitengröße | | |
| 3 | SDK `lib/aarch64` | | |
| 4 | Import, `HAVE_MVS` | | |
| 5 | `uv run c_tests` | | |
| 6 | Speicherordnung, 200 Runden | | |
| 6 | dasselbe unter Last | | |
| 6 | Gegenprobe `v2.1.0` | | |
| 7 | `py_tests -m "not hardware"` | | |
| 8 | ASan | | |
| 8 | TSan | | |
| 9 | Hardware-Tests (Python, C++) | | |
| 9 | Aufnahme: Auflösung, Bildrate, `dropped` | | |
| 9 | Vorschaufenster | | |
| 10 | OME-TIFF-Export | | |
