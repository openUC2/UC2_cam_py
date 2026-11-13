# Bewertung des Pakets `uc2_cam_py` 2.1.2

**Stand:** 2026-11-13 · Commit `0708505` auf `main` · **Prüfer:** Claude Fable 5.2 (Claude Code)
**Prüfumgebung:** x86-64, Linux, GCC 16, 16 Kerne, mit HikRobot MV-CA023-10UM. Auf dem
Raspberry Pi 5 wurde für diese Bewertung nichts ausgeführt.

## Abstract

`uc2_cam_py` steuert Kameras über einen C++-Kern und stellt die Bilder in Python kopierfrei bereit.
Jede Kamera läuft in einem eigenen Prozess; ihr Ring Buffer liegt in gemeinsam genutztem Speicher.
Diese Architektur trägt die Projektziele: Die Aufnahme braucht den Python-GIL nicht, mehrere Kameras
nutzen mehrere Kerne, und ein Absturz im Kamera-Prozess erreicht die Anwendung nicht. Eigene Läufe
bestätigen das auf x86: 139 C++- und 227 Python-Tests bestehen, auch unter ASan, UBSan und TSan.
Vier simulierte Kameras und eine HikRobot-Kamera hielten ihre Bildraten, während ein Python-Thread
den GIL belegte.

Sechs Punkte bleiben offen. Am schwersten wiegt: Ein Aufruf, der nicht fristgerecht beantwortet
wird, beendet den Kamera-Prozess, auch wenn dieser nur mit einem anderen Befehl beschäftigt ist.
Zweitens hält ein vergessenes `close()` den GIL bis zu 30 Sekunden. Drittens können Aufnahmedateien
beschädigte Bilder ohne Kennzeichen enthalten. Viertens muss der Anwender die Gültigkeit
kopierfreier Bilder selbst prüfen. Fünftens läuft das Hersteller-SDK auch im Hauptprozess.
Sechstens fehlt eine dokumentierte Messung der Kernverteilung auf dem Raspberry Pi 5.

Code und Dokumentation sind sorgfältig, die Dokumentation ist stellenweise veraltet. Empfehlung:
die ersten drei Punkte vor dem Produktiveinsatz beheben, danach auf dem Pi messen.

## 1 Vorgehen

Vollständig gelesen: Ring Buffer, `CameraBase`, `RemoteCamera`, `CamWorker`, `CamIpc`, die Bindings
sowie `camera.py`, `data.py`, `_logging.py` und `_process.py`. In Auszügen: die Geräteklassen, der
Exporter, die Tests und die Doku. Ausgeführt: die vorhandenen Testsuiten, auch mit Sanitizern, und
drei eigene Messungen M1 bis M3 (§6); die Skripte lagen außerhalb des Projekts. Nicht geprüft: der
Pi 5, GigE-Kameras, Dauerbetrieb und das Innere des MVS-SDK. Aussagen mit dem Zusatz „aus dem Code“
sind abgeleitet und nicht durch einen Lauf belegt.

## 2 Zielerfüllung

| Nr. | Ziel | Urteil | Begründung |
|---|---|---|---|
| 1 | Kamera und Speichern auf die Kerne verteilen | im Entwurf erfüllt, auf dem Pi 5 nicht belegt | Je Kamera ein Prozess mit Aufnahme- und Schreib-Thread; der Kernel verteilt. M3: vier Kamera-Prozesse gleichzeitig mit je 100 % eines Kerns. Affinität, Priorität und eine dokumentierte Messung auf dem Pi fehlen (Befund 6). |
| 2 | Keine Segfaults, keine Zugriffsverletzungen | weitgehend erfüllt | Bilder halten ihren Speicher am Leben, die geteilte Region ist versiegelt, der Leser rechnet nur mit einer geprüften Kopie des Kopfes. Die Sanitizer melden nichts. Restrisiko: das SDK im Hauptprozess (Befund 5). |
| 3 | Die Kamera blockiert nicht, auch nicht durch den GIL | erfüllt | Der Aufnahme-Thread wartet auf keinen Leser und fasst weder Dateisystem noch Python an. M3: gleiche Bildraten mit und ohne belegten GIL. |
| 4 | Mehrere Kameras im externen Programm, ohne Blockaden | Aufnahme erfüllt, Steuerung eingeschränkt | Die Kameras sind voneinander unabhängig. Die Anwendung kann aber stehen bleiben oder eine Kamera verlieren (Befunde 1 und 2). |
| 5 | Keine Sorge um IPC, Thread- und Prozesssicherheit | teilweise erfüllt | IPC ist verborgen. Wissen muss der Anwender dennoch um Aufrufe aus mehreren Threads (Befund 1), die Gültigkeit kopierfreier Bilder (Befund 4) und `fork()` (Befund 7). |

## 3 Architektur

Die Anwendung spricht mit jeder Kamera über ein `RemoteCamera`-Objekt. Dahinter läuft je Kamera ein
Prozess `uc2_cam_worker` mit Gerät, Aufnahme-Thread und Plattenschreiber. Einstellungen gehen über
einen Unix-Socket. Die Bilder liegen in einem Ring Buffer in gemeinsam genutztem Speicher, den die
Anwendung nur abbildet und liest.

**Was trägt:**

- **Ein Prozess je Kamera.** Ein Mittel für die Ziele 1 bis 4: Parallelität ohne GIL, Abschottung
  des fremden SDK und die Möglichkeit, alle Threads einer Kamera gemeinsam zu steuern.
- **Ring Buffer als zeigerfreier Block mit Seqlock je Slot.** Der Aufnahme-Thread ist der einzige
  Schreiber und wartet im Overwrite-Modus auf niemanden; ein hängender oder toter Leser hält ihn
  nicht auf. Stirbt der Kamera-Prozess mitten im Bild, liest die Anwendung weiter gültigen Speicher.
- **Versiegeltes memfd mit `fallocate`.** Speichermangel zeigt sich als Fehler beim Start, nicht
  später als SIGBUS. Nach einem Absturz bleibt nichts aufzuräumen.
- **Klare Vertrauensgrenze.** `attach()` prüft eine private Kopie des Kopfes gegen die Größe der
  Abbildung; danach stammen alle Adressen und Größen aus dieser Kopie.
- **Entkoppeltes Speichern.** Der Aufnahme-Thread weckt den Schreiber nur. Dieser schreibt Kopf und
  Bild mit einem `writev()` direkt aus dem Slot.
- **Kleines, grenzgeprüftes Protokoll** mit Versionsprüfung, und **Logging ohne Kopplung an den
  GIL:** C++-Threads hängen Meldungen nur an eine Queue.

**Was begrenzt:**

- **Overwrite ist fest eingestellt** ([CameraBase.cpp:307](../core/src/CameraBase.cpp#L307)). Ein
  kopierfreies Bild kann sich deshalb unter dem Leser ändern (Befund 4); den Wait-Modus des Ring
  Buffers erreicht Python nicht.
- **Steuerung und Umfeld.** Der Steuerkanal reagiert auf Verzögerung nur mit dem Beenden des
  Prozesses (Befund 1), das SDK läuft auch im Hauptprozess (Befund 5), und die Kernverteilung ist
  nicht steuerbar (Befund 6).
- **Der Leser bildet die Region schreibbar ab,** weil er Pins, Flags und Zähler setzt. Ein
  fehlerhafter Host kann so Statistik verfälschen. Speicherfehler im Kamera-Prozess folgen daraus
  nicht, denn dieser rechnet nur mit eigenen Größen (aus dem Code).

## 4 Implementierung

Der Code ist sorgfältig: Kommentare begründen Entscheidungen, `static_assert` sichert das
Speicherlayout, Größen werden mit Überlaufprüfung berechnet, Handles geben Slots per RAII frei.
Die Speicherbarrieren des Seqlocks für aarch64 sitzen richtig: beim Schreiber nach dem ungeraden
Zähler, beim Leser vor der Nachprüfung. Acht Befunde, nach Gewicht:

**Befund 1 (hoch): Eine überschrittene Antwortfrist beendet auch einen gesunden Kamera-Prozess.**
Der Kamera-Prozess arbeitet Befehle nacheinander in einem Thread ab
([CamWorker.cpp:585](../core/src/CamWorker.cpp#L585)). Die Anwendung startet die Frist aber beim
Senden: 5 s für Abfragen, 30 s für `start`, `stop`, `close` und Geometrieänderungen. Läuft sie ab,
erhält der Prozess SIGKILL, und die Kamera gilt als verloren
([RemoteCamera.cpp:419](../core/src/RemoteCamera.cpp#L419)). Zwei Wege führen dorthin, ohne dass
etwas defekt ist:

- Ein zweiter Python-Thread fragt einen Wert ab, während `stop()` noch die Aufnahme leert und
  `fsync()` ausführt. M1 stellt das nach: Der Getter beendete den Prozess nach 5,5 s; ohne ihn
  kehrte dasselbe `stop()` nach 7,0 s regulär zurück. Der Getter meldet keinen Fehler, er liefert
  `None`. Die Doku erlaubt Aufrufe aus mehreren Threads ausdrücklich (§7.7).
- Das Leeren selbst dauert auf einem langsamen Datenträger länger als 30 s (aus dem Code).

Die Folge ist eine verlorene Kamera und möglicherweise eine abgeschnittene Aufnahme.
*Empfehlung:* Aufrufe je Kamera der Reihe nach senden, sodass die Frist erst mit der Bearbeitung
beginnt. Während des Leerens Lebenszeichen senden und erst beenden, wenn diese ausbleiben.

**Befund 2 (mittel): Ohne `close()` hält der Destruktor den GIL bis zu 30 s.** `Camera.close()` gibt
den GIL frei, der Destruktor des Bindings nicht ([hik_py.cpp:599](../bindings/hik_py.cpp#L599));
`Camera` hat kein `__del__`. Wer das Objekt ohne `close()` und ohne `with` verwirft, hält alle
Python-Threads an, bis der Kamera-Prozess geantwortet hat. M2: 30 s Stillstand gegenüber 11 ms bei
`close()`. *Empfehlung:* `Camera.__del__` ruft `close()`; dazu ein Deleter, der den GIL freigibt.

**Befund 3 (mittel): Die Aufnahmedatei kann beschädigte Datensätze enthalten.** Der Schreiber prüft
erst nach dem `writev()`, ob der Aufnahme-Thread den Slot inzwischen überschrieben hat
([UC2_RingBuff.cpp:1987](../core/src/UC2_RingBuff.cpp#L1987)). Er zählt das Bild dann als verloren,
der gemischte Datensatz bleibt aber ohne Kennzeichen in der Datei; die Doku vermerkt das in §9.1.
Nach einem Schreibfehler, etwa bei vollem Datenträger, schreibt er Bild für Bild weiter. Das
Fehlerflag liest `camera.py` zwar, verwirft es aber
([camera.py:666](../python/uc2_cam_py/camera.py#L666)). *Empfehlung:* die Dateiposition vor jedem
Datensatz merken und bei Ungültigkeit oder Fehler mit `ftruncate()` zurücksetzen; nach einem
Schreibfehler das Speichern beenden; gespeicherte und verlorene Bilder sowie den Fehlerzustand als
Eigenschaft der Kamera anbieten.

**Befund 4 (mittel): Kopierfreie Bilder verlangen vom Anwender ein Prüfprotokoll.** Bei 16 Slots
und 100 Bildern je Sekunde gehört der Speicher eines Bildes nach 160 ms einem anderen Bild. Der
Anwender muss nach der Verarbeitung `is_valid()` rufen oder vorher kopieren
([data.py:24](../python/uc2_cam_py/data.py#L24)). Das ist korrekt dokumentiert, widerspricht aber
Ziel 5. *Empfehlung:* eine sichere Variante, etwa `get_last_image(copy=True)`, die kopiert, die
Gültigkeit prüft und bei Bedarf wiederholt.

**Befund 5 (mittel): Das MVS-SDK wird auch im Hauptprozess geladen.** `import uc2_cam_py`
initialisiert es dort ([\_\_init\_\_.py:111](../python/uc2_cam_py/__init__.py#L111)), und
`list_cameras()` sucht dort. Ein Fehler des SDK in diesen Phasen trifft die Anwendung.
*Empfehlung:* die Suche in einen kurzlebigen Kamera-Prozess verlegen; das steht in `Todos.md`.

**Befund 6 (mittel): Die Kernverteilung ist nicht steuerbar und auf dem Pi 5 nicht dokumentiert
gemessen.** Siehe §6. *Empfehlung:* erst messen, dann über Affinität und Priorität entscheiden.

**Befund 7 (niedrig): `fork()` in der Anwendung ist nicht abgesichert.** Kindprozesse aus `fork()`,
dem Standard von `multiprocessing` unter Python 3.12, erben die Socket-Enden. Der Kamera-Prozess
bemerkt das Ende der Anwendung dann erst, wenn auch die Kinder beendet sind (aus dem Code).
*Empfehlung:* ein Hinweis in der Doku und ein Fork-Handler, der die Sockets im Kind schließt.

**Befund 8 (niedrig): Randfälle.** Mit `separate_process=False` hält `get_last_image()` den GIL,
solange ein paralleles `stop()` läuft ([hik_py.cpp:301](../bindings/hik_py.cpp#L301)), und
`HikCamera` hat keine eigene Sperre gegen gleichzeitige Aufrufe (aus dem Code). Dieser Modus sollte
als Debughilfe bezeichnet sein. In beiden Modi kann das X11-Fenster beim Öffnen hängen
([ImageWindow.cpp:53](../core/src/ImageWindow.cpp#L53)); der Punkt ist seit dem Review vom
2026-09-03 offen.

## 5 Dokumentation

Umfang und Qualität liegen über dem Üblichen: eine Schnittstellenspezifikation mit Verhalten,
Fehlerpolitik und bekannten Grenzen, eine Referenz zum Ring Buffer, Anleitungen für Sanitizer und
Profiling, eine Checkliste für den Pi 5, CHANGELOG und Versionsregeln. Mängel:

- **Veraltete Stellen.** `README.md:24` nennt aarch64 „not yet run on hardware“. `Tooling.md` §6
  beschreibt den Mehrprozess-Betrieb als Zukunft. STATUS §4 sagt, ohne SDK lasse sich nicht
  konfigurieren, und §5, der Ring Buffer sei nicht an Python angebunden. `Todos.md` beschreibt den
  entfernten `FrameGrabber`. Die Schnittstellen-Doku §11 nennt die HikRobot-Anbindung an Hardware
  ungeprüft; in meinem Lauf bestanden die 23 Hardware- und Anzeige-Tests an der MV-CA023-10UM.
- **Fehlende Ergebnisse.** Die Ergebnistabelle der ARM-Checkliste ist leer. Bildraten, verlorene
  Bilder und Kernauslastung auf dem Pi stehen nirgends.
- **Fehlende Hinweise für Anwender:** das Verhalten aus Befund 1, `fork()`, ein Beispiel mit
  mehreren Kameras.
- **Pflegeaufwand.** `STATUS.md` mischt auf über 1300 Zeilen Verlauf und Referenz;
  `RingBuff_REF.md` hängt an 35 Zeilenverweisen.
- **Paket und Recht.** `requires-python == 3.12.*` bindet auch die einbindende Anwendung an
  Python 3.12. `pyproject.toml` nennt MIT, eine `LICENSE`-Datei fehlt. Ob die HikRobot-Bibliotheken
  im Wheel weitergegeben werden dürfen, ist offen.

## 6 Tests und eigene Messungen

| Lauf | Ergebnis |
|---|---|
| `ctest`, Presets `debug`, `asan`, `tsan` | je 139 von 139 bestanden; unter TSan ist der beabsichtigte Wettlauf um die Bilddaten unterdrückt |
| pytest, mit Kamera | 227 bestanden, 2 übersprungen |
| **M1** Frist bei beschäftigtem Kamera-Prozess | Prozess 7 s angehalten. Nur `stop()`: Rückkehr nach 7,0 s, Kamera danach nutzbar. Dazu ein Getter aus einem zweiten Thread: SIGKILL nach 5,5 s, Kamera verloren. |
| **M2** Verwerfen ohne `close()` | Prozess angehalten. `close()`: 30,0 s, längste Lücke eines Python-Taktgebers 11 ms. `del cam`: 30,0 s, Lücke 30 010 ms. |
| **M3** Fünf Kameras, zweimal 5 s | Vier simulierte Kameras (1920 × 1200, Mono16, freilaufend): 28 bis 33 Bilder/s bei je 100 % eines Kerns. HikRobot: 41 Bilder/s bei 5 %. Mit einem Python-Thread, der den GIL belegt: 28 bis 32 und 41 Bilder/s. |

In M1 und M2 steht das Anhalten per SIGSTOP für einen Kamera-Prozess, der lange an einem Befehl
arbeitet; für den Socket ist beides dasselbe. M3 gilt für x86 mit 16 Kernen. Auf den vier Kernen des
Pi 5 teilen sich Kameras, Schreiber und Anwendung die Rechenzeit, und in M3 wanderten die Threads
häufig zwischen den Kernen. Was das dort kostet, zeigt nur eine Messung.

Die Testbasis ist stark: Nebenläufigkeit, Prozessgrenze, manipulierte Regionen und ein per SIGKILL
beendeter Schreiber sind abgedeckt. Es fehlen eine CI, ein Test mit mehreren Python-Threads gegen
einen langsamen Befehl, ein Test mit langsamem Datenträger und ein Dauerlauf.

## 7 Empfehlungen und Fazit

1. Befunde 1, 3 und 2 in dieser Reihenfolge beheben.
2. Auf dem Pi 5 messen: eine bis vier Kameras bei voller Auflösung mit Speichern auf SSD; Bildrate,
   verlorene Bilder, Auslastung je Kern. Das Ergebnis dokumentieren, dann über Affinität und
   Priorität entscheiden.
3. Befunde 4 und 5 beheben.
4. Dokumentation nachziehen, `LICENSE` ergänzen, CI einrichten.

**Fazit.** Die Architektur passt zu den Zielen. Ihr Kern – Ring Buffer, Prozessgrenze,
Speichersicherheit – ist sorgfältig umgesetzt und gut getestet, und die Aufnahme selbst blockiert
nicht. Die Schwächen liegen am Rand: im Steuerkanal, im Abbau ohne `close()` und in der
Aufnahmedatei. Sie sind mit begrenztem Aufwand behebbar. Offen bleibt der Nachweis auf der
Zielplattform.
