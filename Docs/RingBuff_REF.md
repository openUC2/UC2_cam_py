# Referenz: `UC2_RingBuff.h`

Ring Buffer für Bildframes mit **einem Producer** und **mehreren Consumern** (SPMC), inklusive
Disk-Streaming in eine Container-Datei.

| | |
|---|---|
| Header | [core/include/UC2_RingBuff.h](../core/include/UC2_RingBuff.h) |
| Implementierung | [core/src/UC2_RingBuff.cpp](../core/src/UC2_RingBuff.cpp) |
| Tests | [core/tests/test_ringbuffer.cpp](../core/tests/test_ringbuffer.cpp) |
| Namespace | `hik` |
| Sprache | C++17; die Erweiterungspunkte sind C-ABI (`extern "C"`) |

## Inhalt

1. [Designvertrag](#1-designvertrag)
2. [Speicherlayout der Region](#2-speicherlayout-der-region)
3. [Konstanten und Enums](#3-konstanten-und-enums)
4. [POD-Strukturen](#4-pod-strukturen)
5. [C-ABI-Erweiterungspunkte](#5-c-abi-erweiterungspunkte)
6. [Freie Funktionen](#6-freie-funktionen)
7. [Klasse `RingBuffer`](#7-klasse-ringbuffer)
8. [Klasse `FileStreamSink` und Container-Format](#8-klasse-filestreamsink-und-container-format)
9. [Threading, Policies und Statistik](#9-threading-policies-und-statistik)
10. [Nutzungsmuster](#10-nutzungsmuster)
11. [Abweichungen zwischen Header-Kommentar und Implementierung](#11-abweichungen-zwischen-header-kommentar-und-implementierung)

---

## 1. Designvertrag

Der gesamte Zustand des Puffers liegt in **einer zusammenhängenden Speicherregion**. Dieselbe
Region soll später in mehrere Prozesse gemappt werden können: Ein Kameraprozess schreibt, ein
Streaming-Prozess speichert, ein Analyseprozess liest. Heute wird nur ein Heap-Allocator
ausgeliefert, die Regeln gelten trotzdem schon.

| Regel | Begründung |
|---|---|
| **Keine Pointer in der Region.** Alles sind Byte-Offsets relativ zur Region-Basis. | Ein anderer Prozess mappt das Segment an eine andere Adresse. |
| **Keine C++-Objekte mit virtuellen Funktionen in der Region.** | Ein vtable-Pointer ist eine prozesslokale Code-Adresse. Deshalb sind die Erweiterungspunkte C-Structs aus Funktionszeigern *außerhalb* der Region ([§5](#5-c-abi-erweiterungspunkte)). |
| **Kein `std::string` / `std::vector` in der Region.** | Metadaten stehen in Char-Arrays fester Größe. |

**Copy Budget.** Auf dem Weg in den Puffer gibt es genau eine Kopie: Der Aufrufer kopiert von der
Quelle direkt in `WriteHandle::data()`. Lesen kopiert nichts, denn `ReadHandle::data()` zeigt direkt
in den Slot. Der Disk-Streamer schreibt mit `writev()` direkt aus dem Slot, ohne Zwischenpuffer.

**Synchronisation.** Die Korrektheit hängt nur an lock-freien Atomics. Ein hängender oder
abgestürzter Consumer kann den Puffer deshalb nicht beschädigen. Jeder Slot hat einen
**Seqlock-Zähler**: gerade = stabil, ungerade = Schreiben läuft. Ein Reader liest den Zähler vor
und nach der Nutzung und verwirft den Frame, wenn der Producer den Slot inzwischen recycelt hat
(`ReadHandle::stillValid()`). Mutex und Condition Variables liegen außerhalb der Region und dienen
nur dazu, aktives Warten zu vermeiden.

---

## 2. Speicherlayout der Region

`configure()` berechnet das Layout, fordert die Region über `UC2_MemoryOps::acquire` an (64-Byte-
Alignment), nullt sie und legt die Objekte mit Placement-new an
([UC2_RingBuff.cpp:472](../core/src/UC2_RingBuff.cpp#L472)).

```
Offset 0                  UC2_SessionHeader             8448 B
controlOffset             UC2_Control                    576 B   (64 B Kopf + 8 × UC2_Consumer)
slotsOffset               UC2_SlotRecord[slotCount]      slotCount × 64 B
payloadOffset             Payload[slotCount]             slotCount × slotStride
```

- Jeder Abschnitt ist auf `kCacheLine` (64 B) aufgerundet.
- `slotStride = alignUp(frameBytes, 64)`
- `frameBytes = width × height × channels × itemSize(depth)`. Jede Multiplikation ist auf
  Überlauf geprüft.
- Slot `i` findet seine Payload über `UC2_SlotRecord::payloadOffset` (Offset, kein Pointer).

**Grenzen** (in der Implementierung, nicht im Header):

| Größe | Grenze |
|---|---|
| Breite, Höhe | 1 … 65536 |
| Kanäle | 1 … 4096 |
| `slotCount` | 2 … 65536 |
| Gesamtgröße der Region | ≤ 16 GiB |

**Layout-Invarianten** werden per `static_assert` erzwungen
([UC2_RingBuff.cpp:31-39](../core/src/UC2_RingBuff.cpp#L31-L39)):

- Die Strukturgrößen sind fest: `UC2_SessionHeader` = 8448 B, `UC2_FrameHeader` = 64 B,
  `UC2_SlotRecord` = 64 B, `UC2_Consumer` = 64 B.
- `UC2_SessionHeader`, `UC2_SlotRecord` und `UC2_Control` sind standard layout.
- `std::atomic<uint32_t>` und `std::atomic<uint64_t>` müssen lock-free sein.

---

## 3. Konstanten und Enums

### `enum class PixelDepth : uint8_t` — [L62](../core/include/UC2_RingBuff.h#L62)

Sampletyp eines Kanalwerts. Der Enumeratorwert ist gleichzeitig die Größe in Byte.

| Wert | Zahl | `depthName()` |
|---|---|---|
| `UInt8` | 1 | `"uint8"` |
| `UInt16` | 2 | `"uint16"` |
| `Float64` | 8 | `"float64"` |

- `uint32_t itemSize(PixelDepth d)` gibt die Bytegröße eines Samples zurück (inline).
- `const char* depthName(PixelDepth d)` gibt den Kurznamen zurück, bei unbekanntem Wert
  `"unknown"`.

### `enum class FullPolicy : uint8_t` — [L77](../core/include/UC2_RingBuff.h#L77)

Legt fest, was der Producer tut, wenn der nächste Slot noch einen Frame enthält, den nicht alle
registrierten Consumer bestätigt haben.

| Wert | Verhalten |
|---|---|
| `Overwrite` (0) | Nie blockieren. Der Slot wird recycelt; wurde der alte Frame nie gelesen, zählt `framesOverwritten()` hoch. |
| `Wait` (1) | Blockieren, bis alle registrierten Consumer den alten Frame bestätigt haben und kein Reader den Slot mehr pinnt. Nach `Config::waitTimeoutMs` wird trotzdem überschrieben und `waitTimeouts()` zählt hoch. |

### `enum FrameFlag : uint32_t` — [L85](../core/include/UC2_RingBuff.h#L85)

Statusbits pro Frame in `UC2_SlotRecord::flags` und `UC2_FrameHeader::flags`.

| Bit | Wert | Bedeutung | Gesetzt von |
|---|---|---|---|
| `kFlagWriting` | `1<<0` | Schreiben läuft | `beginWrite()` |
| `kFlagWritten` | `1<<1` | Frame vollständig und lesbar | `commitWrite()`; ersetzt `kFlagWriting` |
| `kFlagRead` | `1<<2` | Mindestens ein Consumer hat gelesen | `ReadHandle::release(markRead=true)` / Destruktor |
| `kFlagSaved` | `1<<3` | Frame liegt auf der Platte | `ReadHandle::release(…, markSaved=true)` |

`beginWrite()` setzt die Flags auf `kFlagWriting` zurück; `abortWrite()` setzt sie auf 0.

### Skalare Konstanten

| Name | Wert | Bedeutung |
|---|---|---|
| `kMaxConsumers` | 8 | Maximale Zahl registrierter Consumer, die den Producer im Wait-Modus bremsen können |
| `kAnonymousConsumer` | `0xFFFFFFFF` | Consumer-ID, die den Producer nie bremst (Preview, Probing) |
| `kCacheLine` | 64 | Angenommene Cache-Line-Größe für Padding und Alignment (Cortex-A76 / x86-64) |
| `kLayoutVersion` | 1 | Version des Shared-Memory-Layouts (`UC2_SessionHeader`, `UC2_Control`, `UC2_SlotRecord`) |
| `kFormatVersion` | 1 | Version des Container-Dateiformats |
| `kAbiVersion` | 1 | Version der C-ABI-vtables `UC2_MemoryOps` / `UC2_SinkOps` |

---

## 4. POD-Strukturen

Alle Strukturen dieses Abschnitts bis auf `UC2_FrameRef` stehen unter `#pragma pack(push, 8)` und
dürfen in der Region liegen (keine Pointer, keine vtables).

### `UC2_SessionHeader` (8448 B) — [L128](../core/include/UC2_RingBuff.h#L128)

Beschreibt eine Aufnahmesitzung. `configure()` füllt ihn, `startRecording()` ergänzt die beiden
Startzeiten. Er wird unverändert an den Anfang der Container-Datei kopiert.

| Feld | Typ | Bedeutung |
|---|---|---|
| `magic` | `char[8]` | `"UC2RB\0\0\0"` |
| `formatVersion` | `uint32_t` | `kFormatVersion` |
| `headerBytes` | `uint32_t` | `sizeof(UC2_SessionHeader)` = 8448 (bis zur Vergrößerung von `comment` 512, bei unveränderter `formatVersion` 1) |
| `width`, `height` | `uint32_t` | Bildgröße in Pixeln |
| `channels` | `uint32_t` | Kanäle pro Pixel (≥ 1, nicht auf 3 begrenzt) |
| `depth` | `uint8_t` | `PixelDepth`-Wert |
| `reserved0` | `uint8_t[3]` | — |
| `frameBytes` | `uint64_t` | `width × height × channels × itemSize(depth)` |
| `slotCount` | `uint32_t` | Anzahl Slots |
| `layoutVersion` | `uint32_t` | `kLayoutVersion` |
| `startWallNs` | `int64_t` | UTC-Epoch-ns bei `startRecording()` |
| `startMonoNs` | `int64_t` | Monotonic-ns im selben Moment |
| `slotStride` | `uint64_t` | Payload-Abstand zwischen Slots (`frameBytes`, auf 64 B aufgerundet) |
| `cameraId` | `char[64]` | NUL-terminiert, max. 63 Zeichen |
| `comment` | `char[8192]` | NUL-terminiert, max. 8191 Zeichen |
| `reserved2` | `uint8_t[120]` | — |

> **Zeitanker:** Die Zeitstempel pro Frame sind monotonic, also unabhängig von NTP-Sprüngen und
> Sommerzeit. Das Paar `startWallNs`/`startMonoNs` rechnet sie in absolute Zeit um:
> `wallNs(frame) = startWallNs + (tsNs(frame) − startMonoNs)`.

### `UC2_FrameHeader` (64 B) — [L150](../core/include/UC2_RingBuff.h#L150)

Datensatzkopf vor jedem Frame in der Container-Datei.

| Feld | Typ | Bedeutung |
|---|---|---|
| `sequence` | `uint64_t` | Globaler Frame-Index, ab 0 |
| `nbytes` | `uint64_t` | Gültige Payload-Bytes nach diesem Header |
| `tsBeforeNs` | `int64_t` | Monotonic-ns vor dem Auslesen des Frames |
| `tsAfterNs` | `int64_t` | Monotonic-ns nach dem Auslesen |
| `slot` | `uint32_t` | Ring-Slot, in dem der Frame lag |
| `flags` | `uint32_t` | `FrameFlag`-Bits beim Speichern, immer inkl. `kFlagSaved` |
| `width`, `height`, `channels` | `uint32_t` | Geometrie |
| `depth` | `uint8_t` | `PixelDepth`-Wert |
| `reserved` | `uint8_t[11]` | — |

### `UC2_SlotRecord` (64 B) — [L167](../core/include/UC2_RingBuff.h#L167)

Verwaltungsdaten eines Slots in der Region. Alle veränderlichen Felder sind atomar.

| Feld | Typ | Bedeutung |
|---|---|---|
| `seq` | `atomic<uint32_t>` | Seqlock: gerade = stabil, ungerade = Schreiben läuft |
| `flags` | `atomic<uint32_t>` | `FrameFlag`-Bits |
| `pins` | `atomic<uint32_t>` | Zahl der Reader, die den Slot gerade halten |
| `sequence` | `atomic<uint64_t>` | Globaler Frame-Index im Slot |
| `nbytes` | `atomic<uint64_t>` | Gültige Payload-Bytes |
| `tsBeforeNs`, `tsAfterNs` | `atomic<int64_t>` | Monotonic-Zeitstempel |
| `payloadOffset` | `uint64_t` | Byte-Offset der Payload ab Region-Basis |

### `UC2_Consumer` (64 B) — [L183](../core/include/UC2_RingBuff.h#L183)

Bestätigungscursor eines registrierten Consumers. Er belegt eine eigene Cache-Line, damit
Consumer und Producer sich nicht gegenseitig die Cache-Line entziehen.

| Feld | Typ | Bedeutung |
|---|---|---|
| `active` | `atomic<uint32_t>` | ≠ 0, solange der Eintrag belegt ist |
| `ackSequence` | `atomic<uint64_t>` | Erste Sequenz, die **noch nicht** bestätigt ist |

### `UC2_Control` (576 B) — [L192](../core/include/UC2_RingBuff.h#L192)

Producer-Cursor, Zähler und Consumer-Tabelle.

| Feld | Bedeutung |
|---|---|
| `writeSequence` | Nächste Sequenz, die der Producer belegt |
| `committedCount` | Anzahl vollständig veröffentlichter Frames |
| `overwritten` | Frames, die ungelesen recycelt wurden |
| `waitTimeouts` | Wait-Gates, die abgelaufen sind |
| `aborted` | Über `abortWrite()` abgebrochene Schreibvorgänge |
| `rejectedReads` | Vom Seqlock abgewiesene Lesezugriffe |
| `recording` | ≠ 0 zwischen `startRecording()` und `stopRecording()` |
| `consumers[kMaxConsumers]` | Tabelle der `UC2_Consumer` |

### `UC2_FrameRef` — [L210](../core/include/UC2_RingBuff.h#L210)

Beschreibt einen veröffentlichten Frame für den Callback `UC2_SinkOps::onFrame`. Enthält einen
rohen Pointer und liegt deshalb **nie** in der Region.

| Feld | Typ | Bedeutung |
|---|---|---|
| `data` | `const void*` | Erstes Payload-Byte; zeigt direkt in den Ring-Slot |
| `nbytes` | `uint64_t` | Gültige Bytes ab `data` |
| `sequence` | `uint64_t` | Globaler Frame-Index |
| `tsBeforeNs`, `tsAfterNs` | `int64_t` | Monotonic-Zeitstempel |
| `slot` | `uint32_t` | Slot-Index |
| `width`, `height`, `channels` | `uint32_t` | Geometrie |
| `depth` | `uint8_t` | `PixelDepth`-Wert |

---

## 5. C-ABI-Erweiterungspunkte

Beide Strukturen sind bewusst C-Structs aus Funktionszeigern und keine C++-Interfaces:

- Sie überstehen die Grenze zwischen C++ und Python unverändert.
- Sie lassen sich in einer PyCapsule weiterreichen (`void*` plus Destruktor).
- `ctx` übernimmt die Rolle von `this`: Er wird an jeden Callback als erstes Argument
  zurückgegeben.

Beide tragen `abiVersion`, das gleich `kAbiVersion` sein muss. Sonst wird die Registrierung mit
einem Log-Eintrag abgelehnt.

### Funktionszeiger-Typen — [L237-L250](../core/include/UC2_RingBuff.h#L237-L250)

| Typ | Signatur | Semantik |
|---|---|---|
| `UC2_AcquireFn` | `void* (void* ctx, size_t bytes, size_t alignment)` | Speicher mit Alignment anfordern; `nullptr` bei Fehler |
| `UC2_ReleaseFn` | `void (void* ctx, void* base, size_t bytes)` | Mit `acquire` angeforderten Block freigeben |
| `UC2_SessionStartFn` | `int (void* ctx, const UC2_SessionHeader* header)` | Sitzung öffnet; 0 = Erfolg |
| `UC2_FrameFn` | `int (void* ctx, const UC2_FrameRef* frame)` | Pro veröffentlichtem Frame, **auf dem Producer-Thread**; 0 = Erfolg; darf nicht blockieren |
| `UC2_SessionEndFn` | `void (void* ctx)` | Sitzung schließt |
| `UC2_DestroyFn` | `void (void* ctx)` | Der Puffer gibt seine Referenz auf den Sink auf (Platz für einen PyCapsule-Destruktor) |

### `UC2_MemoryOps` — [L256](../core/include/UC2_RingBuff.h#L256)

Liefert die Speicherregion des Puffers.

| Feld | Pflicht | Bedeutung |
|---|---|---|
| `abiVersion` | ja | `kAbiVersion` |
| `ctx` | — | Wird an `acquire`/`release` durchgereicht |
| `acquire` | **ja** | Region anfordern |
| `release` | **ja** | Region freigeben |

Standard ist `heapMemoryOps()` (`posix_memalign`/`free`). Eine Variante mit `shm_open` + `mmap`
für mehrere Prozesse kann hier ohne weitere Änderungen eingesetzt werden. Installation über
`RingBuffer::setMemoryOps()` vor `configure()`.

### `UC2_SinkOps` — [L271](../core/include/UC2_RingBuff.h#L271)

Ein Abnehmer veröffentlichter Frames. Heute ist das der eingebaute Disk-Streamer, später z. B. ein
Python-Hook. Alle vier Callbacks sind optional; `nullptr` wird übersprungen.

| Feld | Aufgerufen … | Implementierung |
|---|---|---|
| `abiVersion` | geprüft in `addSink()` | [cpp:638](../core/src/UC2_RingBuff.cpp#L638) |
| `ctx` | an jeden Callback | — |
| `onSessionStart` | in `startRecording()`, in Registrierungsreihenfolge | [cpp:681](../core/src/UC2_RingBuff.cpp#L681) |
| `onFrame` | in `commitWrite()` → `dispatchFrame()`, nur während einer Aufnahme | [cpp:746](../core/src/UC2_RingBuff.cpp#L746) |
| `onSessionEnd` | in `stopRecording()` und beim Rückabwickeln eines fehlgeschlagenen Starts | [cpp:711](../core/src/UC2_RingBuff.cpp#L711) |
| `destroy` | in `destroySinks()` aus dem `RingBuffer`-Destruktor | [cpp:655](../core/src/UC2_RingBuff.cpp#L655) |

**Verhalten im Detail**

- **Start ist alles oder nichts.** Gibt ein `onSessionStart` ≠ 0 zurück, ruft `startRecording()`
  `onSessionEnd` für alle bereits geöffneten Sinks auf und gibt `false` zurück.
- **`onFrame` läuft auf dem Kamera-Thread** und bekommt über `UC2_FrameRef::data` direkt den
  Slot-Inhalt. Nur das Nötige kopieren, sofort zurückkehren, kein I/O. Ein Rückgabewert ≠ 0 wird
  nur auf Debug-Level geloggt und hat keine Folgen.
- **Ownership über `destroy`.** Ist `destroy` gesetzt, gibt der Puffer `ctx` beim Zerstören frei.
  Ist es `nullptr`, verwaltet der Aufrufer die Lebensdauer.
- `addSink()` ist während einer laufenden Aufnahme nicht erlaubt.

---

## 6. Freie Funktionen

| Funktion | Rückgabe |
|---|---|
| `UC2_MemoryOps heapMemoryOps()` | Heap-Allocator (`posix_memalign`/`free`, Alignment mindestens `sizeof(void*)`) |
| `int64_t monotonicNs()` | `steady_clock` in ns; für Dauern und Frame-Zeitstempel |
| `int64_t wallClockNs()` | `system_clock` in ns seit Unix-Epoch (UTC) |
| `std::string formatWallClock(int64_t wallNs)` | `"YYYY-mm-ddTHH:MM:SS.mmmZ"` (UTC, Millisekunden) |
| `const char* depthName(PixelDepth d)` | siehe [§3](#enum-class-pixeldepth--uint8_t--l62) |

---

## 7. Klasse `RingBuffer`

[L332](../core/include/UC2_RingBuff.h#L332). Weder kopierbar noch verschiebbar, weil Handles in
die Region zeigen.

### 7.1 Lebenszyklus

```
RingBuffer(cfg) ─▶ set…() ─▶ configure() ─▶ startRecording() ─▶ beginWrite/commitWrite ─▶ stopRecording() ─▶ ~RingBuffer()
                  (Geometrie)  (allokiert,                          readLatest/readSequence
                               legt Streamer an)
```

- Setter sind **nur vor** `configure()` erlaubt; danach geben sie `false` zurück und loggen.
- Schreiben und Lesen funktionieren ab `configure()`, auch ohne laufende Aufnahme. Die Aufnahme
  steuert nur, ob Sinks (und damit der Disk-Streamer) Frames bekommen.
- `startRecording()` / `stopRecording()` können mehrmals nacheinander aufgerufen werden. Jede
  Sitzung erzeugt eine neue Container-Datei.
- Der Destruktor ruft `stopRecording()` auf, beendet den Streamer-Thread, ruft `destroy` für die
  Sinks auf und gibt die Region frei.

```cpp
hik::RingBuffer::Config cfg;
cfg.storagePath = "/data/recordings";   // leer -> nur im Speicher
cfg.slotCount   = 64;
cfg.comment     = "bead tracking run 7";
cfg.policy      = hik::FullPolicy::Overwrite;

hik::RingBuffer rb(cfg);
rb.setImageSize(1920, 1200);
rb.setChannels(1);
rb.setDepth(hik::PixelDepth::UInt8);
rb.setCameraId("MV-CA023-10UM:01234");
rb.configure();          // prüft und allokiert
rb.startRecording();     // Startzeit stempeln, Sinks öffnen

hik::RingBuffer::WriteHandle w;
if (rb.beginWrite(w))
{
    std::memcpy(w.data(), frame.data, frame.nbytes);   // die eine Kopie
    rb.commitWrite(w, frame.nbytes);
}

hik::RingBuffer::ReadHandle r;
if (rb.readLatest(r)) { use(r.data(), r.nbytes()); }   // keine Kopie
```

### 7.2 `RingBuffer::Config` — [L337](../core/include/UC2_RingBuff.h#L337)

Die Geometrie gehört absichtlich nicht dazu, weil sie normalerweise erst nach dem Öffnen der
Kamera bekannt ist.

| Feld | Typ | Default | Bedeutung |
|---|---|---|---|
| `storagePath` | `std::string` | leer | Verzeichnis für die Container-Datei; leer = kein Streaming |
| `slotCount` | `uint32_t` | 16 | Anzahl Bilder im Ring (2 … 65536) |
| `comment` | `std::string` | leer | Freitext im Session-Header |
| `policy` | `FullPolicy` | `Overwrite` | Verhalten bei vollem Ring |
| `waitTimeoutMs` | `uint32_t` | 1000 | Timeout im Wait-Modus, danach wird überschrieben |

### 7.3 `RingBuffer::WriteHandle` — [L351](../core/include/UC2_RingBuff.h#L351)

Reservierung eines Slots durch den Producer. Nur verschiebbar (move-only). Wird ein gehaltenes
Handle zerstört oder per Move überschrieben, wird der Schreibvorgang abgebrochen
(`abortWrite()`). Ein halb gefüllter Slot wird also nie veröffentlicht.

| Methode | Rückgabe |
|---|---|
| `bool held() const` | `true`, solange ein Slot reserviert ist |
| `uint8_t* data() const` | Beschreibbare Payload des Slots (`nullptr`, wenn nicht gehalten) |
| `uint64_t capacity() const` | Kapazität = `frameBytes` |
| `uint64_t sequence() const` | Globaler Index, den dieser Frame bekommt |
| `uint32_t slot() const` | Slot-Index |

### 7.4 `RingBuffer::ReadHandle` — [L393](../core/include/UC2_RingBuff.h#L393)

Sicht eines Consumers auf einen Frame. Nur verschiebbar (move-only). Solange es gehalten wird,
**pinnt** es den Slot; der Destruktor gibt ihn frei wie `release(true, false)`.

`data()` zeigt direkt in den Slot. Unter `FullPolicy::Overwrite` kann der Producer den Slot
recyceln, während das Handle gehalten wird; der Pin schützt nur im Wait-Modus. Deshalb **nach**
dem Verarbeiten `stillValid()` prüfen.

| Methode | Bedeutung |
|---|---|
| `bool held() const` | `true`, solange ein Slot gepinnt ist |
| `bool stillValid() const` | Seqlock erneut prüfen: `true`, wenn der Producer den Slot seit dem Lesen nicht angefasst hat, die gelesenen Bytes also konsistent sind |
| `void release(bool markRead = true, bool markSaved = false)` | Slot freigeben. `markRead` setzt `kFlagRead` und schiebt den Cursor des Consumers weiter; `markSaved` setzt `kFlagSaved`. Flags werden nur gesetzt, wenn der Slot noch denselben Frame enthält. |
| `const uint8_t* data() const` | Payload (zeigt direkt in den Slot) |
| `uint64_t nbytes() const` | Gültige Bytes |
| `uint64_t sequence() const` | Globaler Frame-Index |
| `uint32_t slot() const` | Slot-Index |
| `int64_t tsBeforeNs() const` / `tsAfterNs() const` | Monotonic-Zeitstempel |
| `uint32_t flags() const` | `FrameFlag`-Bits zum Zeitpunkt des Lesens |

### 7.5 Konstruktion, Geometrie und Identität — [L439-L470](../core/include/UC2_RingBuff.h#L439-L470)

| Methode | Beschreibung | `false` wenn |
|---|---|---|
| `explicit RingBuffer(const Config& cfg)` | Installiert `heapMemoryOps()` und übernimmt `cfg.comment`. | — |
| `bool setImageSize(uint32_t width, uint32_t height)` | Bildgröße | bereits konfiguriert, 0 oder > 65536 |
| `bool setChannels(uint32_t channels)` | Kanäle pro Pixel (Default 1) | bereits konfiguriert, 0 oder > 4096 |
| `bool setDepth(PixelDepth depth)` | Sampletyp (Default `UInt8`) | bereits konfiguriert, unbekannter Wert |
| `bool setCameraId(const std::string& id)` | Kamerakennung; über 63 Zeichen wird mit Warnung abgeschnitten | bereits konfiguriert |
| `bool setComment(const std::string& comment)` | Überschreibt `Config::comment`; über 8191 Zeichen wird mit Warnung abgeschnitten | bereits konfiguriert |
| `bool setMemoryOps(const UC2_MemoryOps& ops)` | Eigener Allocator | bereits konfiguriert, ABI falsch, `acquire`/`release` fehlt |
| `bool configure()` | Prüft die Geometrie, allokiert die Region, legt die Slots an und erzeugt bei gesetztem `storagePath` den `FileStreamSink`. Ist der Puffer schon konfiguriert, sofort `true`. | Bildgröße fehlt, `slotCount` außerhalb 2…65536, Überlauf, > 16 GiB, Allokation fehlgeschlagen |
| `bool isConfigured() const` | `true` nach erfolgreichem `configure()` | — |

### 7.6 Aufnahme und Sinks — [L476-L489](../core/include/UC2_RingBuff.h#L476-L489)

| Methode | Beschreibung |
|---|---|
| `bool startRecording()` | Stempelt `startWallNs`/`startMonoNs` in den Header und ruft `onSessionStart` aller Sinks auf. Läuft schon eine Aufnahme, sofort `true`. `false`, wenn nicht konfiguriert oder ein Sink ablehnt (die bereits geöffneten werden dann wieder geschlossen). Ohne Sinks ebenfalls `true`. |
| `void stopRecording()` | Setzt `recording = 0` und ruft `onSessionEnd` aller Sinks auf. Der Disk-Streamer schreibt dabei ausstehende Frames noch weg (`fsync`, `close`). Ohne laufende Aufnahme ohne Wirkung. |
| `bool isRecording() const` | `true` zwischen erfolgreichem Start und Stop |
| `bool addSink(const UC2_SinkOps& ops)` | Registriert einen Sink. `false` bei falscher `abiVersion` oder laufender Aufnahme. Sinks lassen sich nicht wieder abmelden. |

### 7.7 Producer — [L501-L527](../core/include/UC2_RingBuff.h#L501-L527)

Es gibt genau **einen** Producer. Ein zweiter gleichzeitiger `beginWrite()` wird abgewiesen
(Log-Eintrag, `false`).

| Methode | Beschreibung |
|---|---|
| `bool beginWrite(WriteHandle& out)` | Reserviert den nächsten Slot (`sequence % slotCount`), wendet `Config::policy` an (kann im Wait-Modus bis `waitTimeoutMs` blockieren), nimmt `tsBeforeNs = monotonicNs()` und setzt den Seqlock auf ungerade. `false`, wenn nicht konfiguriert, `out` bereits etwas hält oder ein anderer Schreibvorgang läuft. |
| `bool beginWrite(WriteHandle& out, int64_t tsBeforeNs)` | Wie oben, mit eigenem Vorher-Zeitstempel, etwa wenn er vor dem SDK-Aufruf genommen werden muss |
| `bool commitWrite(WriteHandle& handle, uint64_t nbytes)` | Nimmt `tsAfterNs`, setzt `kFlagWritten`, stellt den Seqlock wieder auf gerade, erhöht `writeSequence`/`committedCount`, weckt wartende Reader und ruft `onFrame` aller Sinks auf. Ein zu großes `nbytes` wird mit Warnung auf `frameBytes` gekürzt. Das Handle ist danach freigegeben. |
| `bool commitWrite(WriteHandle& handle, uint64_t nbytes, int64_t tsAfterNs)` | Wie oben, mit eigenem Nachher-Zeitstempel |
| `void abortWrite(WriteHandle& handle)` | Gibt den Slot unveröffentlicht zurück (Flags = 0, Seqlock wieder gerade, `aborted`++). Die Sequenznummer wird erneut vergeben, veröffentlichte Indizes bleiben also lückenlos. Der Frame, der vorher in diesem Slot lag, ist danach nicht mehr lesbar. |
| `bool writeFrame(const void* src, uint64_t nbytes)` | Komfortfunktion: `beginWrite` + `memcpy` + `commitWrite`. `false` bei `src == nullptr` oder wenn `beginWrite` scheitert. |

### 7.8 Consumer — [L534-L562](../core/include/UC2_RingBuff.h#L534-L562)

| Methode | Beschreibung |
|---|---|
| `uint32_t registerConsumer()` | Belegt einen Eintrag in der Consumer-Tabelle. Der Cursor startet bei `committedCount`, ein spät hinzukommender Consumer bremst also nicht wegen alter Frames. Gibt `kAnonymousConsumer` zurück, wenn die Tabelle voll (8) oder der Puffer nicht konfiguriert ist. |
| `void unregisterConsumer(uint32_t consumer)` | Gibt den Eintrag frei; der Consumer bremst den Producer nicht mehr |
| `bool readLatest(ReadHandle& out, uint32_t consumer = kAnonymousConsumer)` | Liest den zuletzt veröffentlichten Frame. `false`, wenn noch nichts veröffentlicht wurde oder der Slot gerade neu beschrieben wird. |
| `bool readSlot(uint32_t slot, ReadHandle& out, uint32_t consumer = kAnonymousConsumer)` | Liest den Frame, der gerade in Slot `slot` liegt (beliebige Sequenz). `false` bei Index außerhalb des Bereichs oder ohne veröffentlichten Frame. |
| `bool readSequence(uint64_t sequence, ReadHandle& out, uint32_t consumer = kAnonymousConsumer)` | Liest den Frame mit globalem Index `sequence`, falls er noch im Ring liegt. `false`, wenn er bereits recycelt wurde. |
| `void acknowledge(uint32_t consumer, uint64_t sequence)` | Schiebt den Cursor des Consumers hinter `sequence`, ohne zu lesen; der Cursor bewegt sich nur vorwärts. Damit kann ein Consumer Frames überspringen, ohne den Producer im Wait-Modus aufzuhalten. |

**Gemeinsame Fehlerfälle aller `read…()`:** Der Puffer ist nicht konfiguriert, `out` hält bereits
einen Slot, oder der Seqlock war beim Lesen ungerade bzw. hat sich zwischen den beiden Prüfungen
geändert. Die Seqlock-Fälle zählen `rejectedReads()` hoch.

### 7.9 Metadaten und Statistik — [L566-L588](../core/include/UC2_RingBuff.h#L566-L588)

| Methode | Rückgabe |
|---|---|
| `width()`, `height()`, `channels()` | Konfigurierte Geometrie (vor `configure()`: die gesetzten Werte) |
| `PixelDepth depth()` | Sampletyp |
| `uint64_t frameBytes()` | Bytes eines Bildes (0 vor `configure()`) |
| `uint32_t slotCount()` | Anzahl Slots |
| `uint64_t regionBytes()` | Gesamtgröße der Region |
| `std::string cameraId()`, `comment()` | Kennung / Kommentar |
| `int64_t startWallNs()`, `startMonoNs()` | Startanker der Sitzung (0, falls nie gestartet) |
| `UC2_SessionHeader sessionHeader()` | Kopie des Headers (genullt vor `configure()`) |
| `uint64_t framesWritten()` | Veröffentlichte Frames (`committedCount`) |
| `uint64_t framesOverwritten()` | Frames, die ungelesen recycelt wurden |
| `uint64_t framesAborted()` | Abgebrochene Schreibvorgänge |
| `uint64_t waitTimeouts()` | Abgelaufene Wait-Gates |
| `uint64_t rejectedReads()` | Vom Seqlock abgewiesene Lesezugriffe |
| `uint32_t slotFlags(uint32_t slot)` | Aktuelle `FrameFlag`-Bits eines Slots (0, wenn außerhalb des Bereichs) |

### 7.10 Disk-Streaming — [L593-L597](../core/include/UC2_RingBuff.h#L593-L597)

Diese Methoden leiten an den eingebauten `FileStreamSink` weiter. Ohne `storagePath` liefern sie
leer, 0 bzw. `false`.

| Methode | Rückgabe |
|---|---|
| `std::string streamFilePath()` | Pfad der aktuellen Container-Datei |
| `uint64_t streamFramesSaved()` | In die Datei geschriebene Frames |
| `uint64_t streamFramesDropped()` | Frames, die recycelt wurden, bevor sie gespeichert werden konnten (inkl. zerrissener Datensätze, siehe [§8](#8-klasse-filestreamsink-und-container-format)) |
| `bool streamHasError()` | Ein Öffnen oder Schreiben ist fehlgeschlagen |

---

## 8. Klasse `FileStreamSink` und Container-Format

[L680](../core/include/UC2_RingBuff.h#L680). `configure()` legt den Streamer automatisch an,
wenn `Config::storagePath` gesetzt ist, und registriert ihn über `addSink(ops())`. Der
`RingBuffer` besitzt ihn über `std::unique_ptr`; `ops().destroy` ist deshalb `nullptr`.

### Öffentliche Schnittstelle

| Methode | Beschreibung |
|---|---|
| `FileStreamSink(RingBuffer& owner, const std::string& dirPath)` | `owner` muss den Sink überleben |
| `UC2_SinkOps ops()` | An diese Instanz gebundene vtable (`ctx = this`, statische Thunks, `destroy = nullptr`) |
| `std::string filePath()` | Container-Pfad; leer, bis eine Sitzung geöffnet wurde |
| `uint64_t framesSaved()` | Angehängte Frames |
| `uint64_t framesDropped()` | Verlorene oder zerrissene Frames |
| `bool hasError()` | Ein Öffnen oder Schreiben ist fehlgeschlagen |

### Ablauf

1. **`onSessionStart`** legt das Verzeichnis an und öffnet
   `<dir>/uc2_<cameraId>_<YYYYmmdd_HHMMSS>.uc2rb`. Der Zeitstempel ist UTC aus `startWallNs`.
   `cameraId` wird auf `[0-9A-Za-z._-]` reduziert (andere Zeichen → `_`, leer → `cam`).
   Existiert die Datei schon, wird `_001` … `_999` angehängt. Anschließend schreibt der Sink den
   Session-Header und das JSON-Sidecar, registriert sich als Consumer und startet den
   Writer-Thread. Gespeichert werden nur Frames ab diesem Zeitpunkt.
2. **`onFrame`** (Producer-Thread) weckt nur den Writer-Thread und liest den Frame nicht.
3. **Writer-Thread** liest jede Sequenz mit `readSequence()` als registrierter Consumer und
   schreibt `UC2_FrameHeader` plus Payload mit **einem** `writev()` direkt aus dem Slot. Er
   reagiert auf Signale und fragt zusätzlich alle 50 ms nach.
4. **`onSessionEnd`** stoppt den Writer, schreibt alle ausstehenden Frames, meldet den Consumer ab,
   ruft `fsync` auf und schließt die Datei.

### Datei-Layout (`.uc2rb`)

```
[UC2_SessionHeader  8448 B]
[UC2_FrameHeader      64 B][payload: nbytes]   sequence n
[UC2_FrameHeader      64 B][payload: nbytes]   sequence n+1
...
```

Hinweise für Programme, die diese Dateien lesen:

- Die Sequenznummern in der Datei können **Lücken** haben. War ein Frame schon recycelt, bevor
  der Writer ihn erreichte, wird er nicht geschrieben, als `dropped` gezählt und bestätigt.
- Ein Datensatz kann **zerrissen** sein (nur unter `Overwrite`). Hat der Producer den Slot
  während des `writev()` recycelt, steht der Datensatz trotzdem in der Datei, wird aber als
  `dropped` gezählt. Der Header-Eintrag `flags` zeigt das nicht an.
- Die Payload-Länge steht pro Datensatz in `UC2_FrameHeader::nbytes`.

### JSON-Sidecar (`.json`, gleicher Basisname)

Enthält `format` (`"uc2rb"`), `formatVersion`, `layoutVersion`, `container`, `headerBytes`,
`frameHeaderBytes`, `width`, `height`, `channels`, `dtype`, `frameBytes`, `slotCount`,
`cameraId`, `comment`, `startTimeUtc`, `startWallNs` und `startMonoNs`. Damit lässt sich eine
Aufnahme identifizieren, ohne das Binärformat zu parsen. Scheitert das Schreiben, gibt es nur eine
Warnung; die Aufnahme läuft weiter.

---

## 9. Threading, Policies und Statistik

| Kontext | Läuft auf |
|---|---|
| `beginWrite` / `commitWrite` / `abortWrite` / `writeFrame` | Producer-Thread (genau einer) |
| `UC2_SinkOps::onFrame` | Producer-Thread, synchron in `commitWrite()` |
| `read…()` / `ReadHandle` / `acknowledge()` | beliebige Consumer-Threads |
| Writer von `FileStreamSink` | eigener `std::thread` |
| Setter, `configure`, `addSink`, `start/stopRecording` | Steuer-Thread; nicht gegen gleichzeitige Aufrufe abgesichert |

**Overwrite oder Wait**

| | `Overwrite` | `Wait` |
|---|---|---|
| Producer blockiert | nie | bis alle registrierten Consumer bestätigt haben **und** `pins == 0`, höchstens `waitTimeoutMs` |
| Reader mit gehaltenem Handle | kann überschrieben werden → `stillValid()` prüfen | geschützt, bis das Timeout greift |
| Anonyme Reader (`kAnonymousConsumer`) | bremsen nie | bremsen nur über den Pin, nicht über den Cursor |
| Datenverlust sichtbar in | `framesOverwritten()`, `streamFramesDropped()` | `waitTimeouts()`, danach wie Overwrite |

`framesOverwritten()` zählt nur Frames, die **kein** Consumer gelesen hat (`kFlagRead` fehlt).

---

## 10. Nutzungsmuster

### Producer-Schleife mit SDK-Zeitstempeln

```cpp
hik::RingBuffer::WriteHandle w;
const int64_t t0 = hik::monotonicNs();          // vor dem SDK-Aufruf
if (rb.beginWrite(w, t0))
{
    if (camera.grabInto(w.data(), w.capacity(), &n))   // Platzhalter für den SDK-Aufruf
        rb.commitWrite(w, n);                   // tsAfterNs = monotonicNs()
    else
        rb.abortWrite(w);                       // oder w einfach verfallen lassen
}
```

### Registrierter Consumer ohne Lücken

```cpp
const uint32_t id = rb.registerConsumer();
uint64_t next = rb.framesWritten();
while (running)
{
    hik::RingBuffer::ReadHandle r;
    if (next < rb.framesWritten())
    {
        if (rb.readSequence(next, r, id))
        {
            process(r.data(), r.nbytes());
            if (!r.stillValid()) { /* Frame während der Verarbeitung überschrieben */ }
        }                                       // ~ReadHandle: kFlagRead + Cursor weiter
        else
        {
            rb.acknowledge(id, next);           // verloren: Producer nicht blockieren
        }
        ++next;
    }
}
rb.unregisterConsumer(id);
```

### Eigener Sink

```cpp
struct FrameCounter { std::atomic<uint64_t> n{0}; };

static int  onFrame(void* ctx, const hik::UC2_FrameRef*) { static_cast<FrameCounter*>(ctx)->n++; return 0; }
static void onDestroy(void* ctx)                          { delete static_cast<FrameCounter*>(ctx); }

hik::UC2_SinkOps ops{};
ops.abiVersion = hik::kAbiVersion;
ops.ctx        = new FrameCounter;
ops.onFrame    = &onFrame;
ops.destroy    = &onDestroy;       // der RingBuffer gibt ctx im Destruktor frei
rb.addSink(ops);                   // vor startRecording()
```

---

## 11. Abweichungen zwischen Header-Kommentar und Implementierung

Beim Abgleich mit [UC2_RingBuff.cpp](../core/src/UC2_RingBuff.cpp) aufgefallen. Die Referenz oben
beschreibt jeweils das tatsächliche Verhalten.

| Stelle im Header | Kommentar | Tatsächliches Verhalten |
|---|---|---|
| `addSink()` [L486](../core/include/UC2_RingBuff.h#L486) | „Ownership of ops.ctx passes to the buffer“ | Nur wenn `destroy` gesetzt ist. Der eingebaute Streamer hat `destroy = nullptr` und gehört dem Puffer über `m_stream`. |
| `abortWrite()` [L520](../core/include/UC2_RingBuff.h#L520) | „The write cursor is rolled back“ | `writeSequence` wird in `beginWrite()` gar nicht erhöht, also gibt es nichts zurückzurollen; das Ergebnis ist dasselbe. Nicht erwähnt: Der vorherige Frame im Slot ist danach verloren. |
| `UC2_SinkOps` [L264](../core/include/UC2_RingBuff.h#L264) | Streamer „only records the sequence number“ | `frameThunk` weckt nur den Writer-Thread; die Sequenz führt der Writer selbst mit (`m_cursor`). |
| `UC2_SessionHeader` [L121](../core/include/UC2_RingBuff.h#L121) | „Written once at configure() time“ | `startWallNs`/`startMonoNs` werden bei jedem `startRecording()` neu gesetzt. |
| `UC2_FrameHeader::sequence` [L152](../core/include/UC2_RingBuff.h#L152) | „0-based and gap-free“ | Im Ring stimmt das. In der Container-Datei kann es Lücken geben (siehe [§8](#datei-layout-uc2rb)). |
