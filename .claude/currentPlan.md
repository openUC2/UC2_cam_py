<!-- Stand 2026-11-13: Freigegeben 2026-11-12 und umgesetzt, nicht committet. Ergebnis und die
     Abweichung beim neuen Test (findet den Fehler ohne Fix nur selten, bleibt trotzdem):
     STATUS, oberster Eintrag. Branch ARM_Fence (HEAD 30135e7).
     Der vorige Inhalt dieser Datei (Plan „Kamera im eigenen Prozess“, umgesetzt) steht in git
     unter Commit 7679220. -->

# Plan: Die Kamera bleibt stehen, wenn sich ein Consumer anmeldet (Wait-Modus) — Version 2.1.2

## 1. Fehlerbeschreibung

### Symptom

- Test #91 „stopStreaming finishes although the producer keeps writing“
  ([test_ringbuffer_stream.cpp:515-561](../core/tests/test_ringbuffer_stream.cpp#L515-L561))
  scheitert sporadisch:
  - im `tsan`-Build 22–28 von 100 Läufen, am PC und auf dem Pi 5
  - im `debug`-Build etwa 1 von 100
- Die Meldung lautet immer `test_ringbuffer_stream.cpp:558: REQUIRE_FALSE(records.empty())`.
- Im Log steht dabei:

  ```
  FileStreamSink: closed …uc2rb after 0 frames (0 dropped)
  RingBuffer: recording stopped after 4653 frames
  ```

- Ablauf des Tests:
  - Ring mit 8 Slots, `FullPolicy::Wait`, `waitTimeoutMs = 5000`.
  - Ein Producer-Thread schreibt so schnell er kann.
  - Dann `startStreaming()`, 20 ms warten, `stopStreaming()`.
  - Erwartet wird, dass in diesen 20 ms mindestens ein Bild gespeichert wurde.

### Frühere, falsche Diagnose (berichtigen!)

- Bisher hieß es, der Schreib-Thread sei nach 20 ms noch nicht dazu gekommen, ein Bild zu
  speichern, also ein Timing-Problem des Tests. So steht es in STATUS §11.2 Punkt 7 und in
  `Docs/ARM_Checkliste.md` §8.
- Das ist falsch:
  - `FileStreamSink::onSessionEnd()` setzt `m_stopAt = framesWritten()`.
  - `writerLoop()` speichert vor dem Ende alles bis `m_stopAt`
    ([UC2_RingBuff.cpp:1890-1925](../core/src/UC2_RingBuff.cpp#L1890-L1925),
    [:1927-2009](../core/src/UC2_RingBuff.cpp#L1927-L2009)).
  - „0 frames“ heißt deshalb: **Der Producer hat zwischen Streaming-Start und Stopp kein einziges
    Bild geschrieben.**
- Der damals vorgeschlagene Test-Umbau, nämlich auf `streamFramesSaved() > 0` zu warten, hätte
  den Fehler **verdeckt**: Nach 5 s Wait-Timeout schreibt der Producer weiter, und der Test wäre
  grün, nur langsam. Test #91 bleibt deshalb **unverändert**. Er hat den Fehler gefunden.

### Messung (2026-11-12, x86, GCC 16, TSan; Kopie des Codes mit Log-Zeilen im Scratchpad)

- In allen Läufen mit „0 frames“ gilt `ack (aus registerConsumer) = m_cursor = m_stopAt`, z. B.
  29 / 29 / 29, 28 / 28 / 28, 35 / 35 / 35.
- In grünen Läufen schreibt der Producer in denselben 20 ms rund 2400 Bilder.
- Gegenprobe: In der Kopie bekam `registerConsumer()` nach dem Setzen von `ackSequence` ein
  Wecken (`{ lock_guard lk(m_mtx); } m_slotFreed.notify_all();`). Danach **0 Fehlschläge in 100
  TSan-Läufen**.
- Vorsicht beim Nachmessen: Ein Log von `m_cursor` in `onSessionEnd()` ist selbst ein Data Race,
  denn der Writer ändert `m_cursor` ohne Lock. TSan lässt dann auch gute Läufe scheitern.

### Ursache 1 (beobachtet): veralteter Startwert und kein Wecken in `registerConsumer()`

[UC2_RingBuff.cpp:1332-1362](../core/src/UC2_RingBuff.cpp#L1332-L1362):

```cpp
if (c.active.compare_exchange_strong(expected, 1, ...))        // (1) zählt ab jetzt als aktiv
{
    c.ackSequence.store(m_control->committedCount.load(...)); // (2) erst jetzt der Startwert
    return i;                                                  // (3) kein notify
}
```

1. Zwischen (1) und (2) sieht der Producer einen **aktiven** Consumer mit **veraltetem**
   `ackSequence`. Bei einem frischen Ring ist das 0, sonst der Wert eines früheren Consumers.
   `minAckSequence()` ([:1120-1132](../core/src/UC2_RingBuff.cpp#L1120-L1132)) zählt jedes
   `active != 0`.
2. Die Wait-Prüfung des Producers in `applyFullPolicy()`
   ([:1134-1171](../core/src/UC2_RingBuff.cpp#L1134-L1171)) verlangt
   `minAckSequence() > outgoing`. Mit dem veralteten Wert ist das falsch, und der Producer
   schläft in `m_slotFreed.wait_for(…, waitTimeoutMs)`.
3. `registerConsumer()` weckt danach nicht. `acknowledge()` (:1377-1398) und
   `unregisterConsumer()` (:1364-1375) tun es beide.
4. Der FileStreamSink-Writer wartet auf ein neues Bild. Weil keines kommt, bestätigt er nichts und
   weckt den Producer auch nicht. Beide warten, bis `stopStreaming()` den Consumer abmeldet
   (`unregisterConsumer()` weckt) oder `waitTimeoutMs` abläuft.
- Der Producer schläft also bis zu `waitTimeoutMs`. Der Standard ist 1000 ms
  ([UC2_RingBuff.h:356](../core/include/UC2_RingBuff.h#L356)), im Test 5000 ms. Danach überschreibt
  er mit WARN „still holds unacknowledged frame“, und `waitTimeouts()` steigt.

### Ursache 2 (nicht beobachtet, aber möglich): Cursor des Writers hinter seiner Bestätigung

`FileStreamSink::onSessionStart()` ([:1864-1876](../core/src/UC2_RingBuff.cpp#L1864-L1876))
meldet sich zuerst an (`ack = committedCount = T1`). Erst danach setzt es
`m_cursor = framesWritten()` (T2 ≥ T1).
- Der Producer darf höchstens 8 Bilder (`slotCount`) über `ack` hinaus schreiben.
- Ist zwischen T1 und T2 der Ring voll geworden (T2 = T1 + 8), wartet der Producer auf die
  Bestätigung von T1. Der Writer wartet auf Bild T2 und liest T1 nie.
- Beide warten dann bis zum Timeout.
- In der Messung war T2 − T1 immer 0 oder 1. Möglich wird es, wenn der Thread genau dort
  verdrängt wird.

### Wer betroffen ist

- Nur Ringe mit `FullPolicy::Wait`.
- Die Kameras in uc2_cam_py nutzen `Overwrite`
  ([CameraBase.cpp:307](../core/src/CameraBase.cpp#L307)). Die Python-Schnittstelle ist also
  **nicht betroffen**.
- Betroffen sind C++-Nutzer der Klasse mit `Wait`, die einen Consumer anmelden oder
  `startStreaming()` aufrufen, während der Producer läuft.
- Kein ARM-Problem; es tritt auf x86 genauso auf.

## 2. Änderungen

### 2.1 `registerConsumer()`: den Startwert setzen, bevor der Eintrag zählt

In [core/src/UC2_RingBuff.cpp](../core/src/UC2_RingBuff.cpp):
- Drei Zustände für `UC2_Consumer::active`:
  - `0` = frei
  - `kConsumerClaimed = 2` = wird gerade angemeldet und zählt noch nicht
  - `kConsumerActive = 1` = aktiv
- Die beiden Konstanten kommen in den anonymen Namespace oben in der Datei.

```cpp
if (c.active.compare_exchange_strong(expected, kConsumerClaimed, acq_rel, acquire))
{
    // Start value BEFORE the entry counts: a producer must never gate on an
    // active consumer whose cursor is still the stale one of a previous user.
    c.ackSequence.store(m_control->committedCount.load(std::memory_order_acquire),
                        std::memory_order_relaxed);
    c.active.store(kConsumerActive, std::memory_order_release);
    return i;
}
```

- `minAckSequence()`: `if (c.active.load(acquire) == kConsumerActive)` statt `!= 0`. Dank
  release/acquire auf `active` sieht der Producer dann immer den frischen `ackSequence`.
- `unregisterConsumer()` bleibt (`store(0)` + notify).
- **Ein `notify` ist nicht nötig.** Ein neuer Consumer kann die Wait-Bedingung nur verschärfen,
  nie lockern. Ein schlafender Producer schläft dann wegen eines anderen Consumers, und dessen
  Bestätigung weckt ihn. In der Scratch-Kopie wurde mit notify gemessen (Variante „Wecken“). Sie
  ist ebenfalls korrekt, lässt das Fenster aber offen. Die gewählte Variante schließt es.
- `active` gibt es nur an diesen drei Stellen (:1126, :1348, :1370); vorher mit `grep -n
  'active\.' core/src/UC2_RingBuff.cpp` prüfen.
- `layoutVersion` bleibt gleich. Die Tabelle liegt zwar in der geteilten Region, aber ein
  angehängter Leser liest sie nie, weil `registerConsumer()` auf angehängten Ringen abgelehnt wird.
  Haupt- und Kamera-Prozess prüfen außerdem ohnehin die Versionsgleichheit.

### 2.2 `FileStreamSink::onSessionStart()`: Bestätigung an den Cursor angleichen

Nach dem Block, der `m_cursor = m_owner.framesWritten()` setzt
([:1871-1875](../core/src/UC2_RingBuff.cpp#L1871-L1875)), und **außerhalb** von `m_mtx`:

```cpp
// Gate the producer from where we start reading, not from where we registered:
// otherwise a full ring in between leaves it waiting for a frame we never read.
if (m_consumer != kAnonymousConsumer && cursor > 0)
{
    m_owner.acknowledge(m_consumer, cursor - 1);
}
```

- `cursor` ist eine lokale Kopie aus dem Block.
- `acknowledge()` bewegt nur vorwärts und weckt den Producer.
- Außerhalb des Sink-Locks, weil `acknowledge()` den `m_mtx` des Rings nimmt. Keine neue
  Lock-Reihenfolge.
- Die Anmeldung muss dafür vor dem Cursor bleiben, sonst würde der Producer Bilder
  überschreiben, die der Writer noch speichern will.

### 2.3 Kommentare und Header

- [UC2_RingBuff.h:197](../core/include/UC2_RingBuff.h#L197): Kommentar zu `active` →
  `///< 0 free, 1 active, 2 being registered (not counted yet)`. Dabei die **Zeilenzahl nicht
  ändern**: `Docs/RingBuff_REF.md` hat 28 Zeilenverweise auf den Header.
- Der Doxygen-Kommentar von `registerConsumer()` (:570) darf nur geändert werden, wenn die
  Zeilenzahl gleich bleibt.

## 3. Tests

- **Test #91 bleibt unverändert.** Er ist der statistische Nachweis.
- **Neu in [core/tests/test_ringbuffer_mt.cpp](../core/tests/test_ringbuffer_mt.cpp):**
  „registering a consumer never stalls a Wait-mode producer“ (`[ring][mt]`).

  ```cpp
  cfg.slotCount = 8; cfg.policy = Wait; cfg.waitTimeoutMs = 5000;   // 4×4×1, configure
  std::thread producer([&]{ while (!done) rb.writeFrame(src.data(), src.size()); });
  int stalls = 0;
  for (int i = 0; i < 300; ++i)
  {
      const uint64_t before = rb.framesWritten();
      const uint32_t id = rb.registerConsumer();
      // Without any acknowledge the producer may still fill the ring: up to slotCount
      // frames past the cursor the registration set, and that cursor is >= before.
      const auto deadline = steady_clock::now() + 500ms;
      while (rb.framesWritten() < before + 8 && steady_clock::now() < deadline) yield();
      if (rb.framesWritten() < before + 8) { ++stalls; }
      rb.unregisterConsumer(id);    // wakes a stalled producer for the next round
  }
  done = true; producer.join();
  CHECK(stalls == 0);               // CHECK only after join(): a REQUIRE with the thread
  CHECK(rb.waitTimeouts() == 0);    // still running would std::terminate the test binary
  ```

  - Warum ein Stillstand erkannt wird: Ohne Fehler darf der Producer bis `ack + 8 ≥ before + 8`
    schreiben, die Bedingung ist nach Mikrosekunden erfüllt. Mit dem Fehler bleibt er bei etwa
    `ack` stehen; das wird nach 500 ms als Stillstand gezählt.
  - Leere Slots (`kFlagWritten` fehlt) prüft der Producer nicht. Die ersten Runden können also
    nichts Falsches melden.
  - Wie zuverlässig das ist: Je Anmeldung trat der Fehler unter TSan in etwa 25 % der Fälle auf,
    im Debug-Build in etwa 1 %. Bei 300 Runden wird er im Debug-Build zu etwa 95 % gefunden,
    unter TSan praktisch sicher.
  - Laufzeit: ohne Fehler wenige ms, mit Fehler 500 ms je Treffer.
  - `REQUIRE` nur vor dem Start des Threads verwenden.
- **Nachweis, dass der neue Test den Fehler findet:** Eine Kopie des Stands **ohne** Fix in den
  Scratchpad legen (`git archive HEAD | tar -x -C <scratch>`), den neuen Test dort hinzufügen,
  TSan-Build mit `-DHIK_MVS=OFF` bauen. Erwartet: `stalls > 0`. Danach mit Fix: `stalls == 0`.

## 4. Doku, Version, Status

- `Docs/RingBuff_REF.md`:
  - §4 `UC2_Consumer`: die Zeile zu `active` (bisher „≠ 0, solange der Eintrag belegt ist“)
    beschreibt jetzt die drei Zustände.
  - §7.8 `registerConsumer()`: „Der Startwert wird gesetzt, bevor der Eintrag zählt.“
  - Die **7 Zeilenverweise** auf `UC2_RingBuff.cpp` (`#L741`, `#L40-L59`, `#L972`, `#L1028`,
    `#L1109`, `#L1071`, `#L998`) verschieben sich durch die neuen Zeilen. Nach der Änderung über
    den Inhalt neu suchen und anpassen; die Prüfschleife steht in STATUS, Eintrag 2026-10-30.
- `Docs/ARM_Checkliste.md` §8: Der Punkt „Bekannt, kein ARM-Problem: stopStreaming finishes …“
  wird ersetzt. Neuer Inhalt: Ab 2.1.2 behoben, war ein echter Fehler (Wait-Modus) und kein
  Timing-Problem. Tritt #91 mit 2.1.2 noch auf, bitte melden.
- `.claude/STATUS.md`:
  - Neuer oberster Eintrag (Implementiert / Committet).
  - §11.2 Punkt 7 als erledigt markieren, mit der richtigen Ursache.
- `VERSION` → **2.1.2** (PATCH: Fehlerfix ohne Schnittstellenänderung). Kopfzeile von
  `Docs/UC2_cam_py_interface.md` → 2.1.2.
- `CHANGELOG.md` `## [2.1.2] - <Datum>`, „Fixed“: Im Wait-Modus konnte der Producer bis zu
  `waitTimeoutMs` stehen bleiben, wenn sich ein Consumer anmeldete oder das Streaming startete,
  während er lief. Die Kameras von uc2_cam_py (Overwrite) waren nicht betroffen.

## 5. Verifikation

- `uv run all_tests`: C++ und Python grün. Die Version meldet 2.1.2.
- TSan, mit den Suppressions des Presets:
  - Test #91 200-mal: **0 Fehlschläge** (vorher 22–28 von 100):

    ```bash
    f=0; for i in $(seq 200); do ctest --preset tsan -R "stopStreaming finishes" >/dev/null 2>&1 || f=$((f+1)); done; echo $f
    ```

  - Dasselbe für den neuen Test (`-R "registering a consumer"`).
- `debug`: Test #91 200-mal, 0 Fehlschläge (vorher etwa 1 von 100).
- `asan`: `ctest --preset asan` grün.
- `profile`: die Torn-Tests (`-R "never torn|readSlot and readLatest"`) 50 Runden, damit klar
  ist, dass nichts anderes betroffen ist.
- Ruff und Pyright bleiben unberührt (kein Python-Code).

## 6. Rahmen

- Laut `.claude/CLAUDE.md` wird erst nach ausdrücklicher Freigabe dieses Plans umgesetzt.
- `UC2_cam_py` nicht verlassen; temporäre Dateien gehören in den Scratchpad.
- Für mich gesperrt (`settings.json`): `git commit`, `push`, `pull`, `checkout`, `switch`,
  `tag -f`, `tag -d`. Erlaubt ist `git tag -a`.
- Ablauf am Ende:
  1. Der Nutzer committet.
  2. Ich setze den Tag `v2.1.2` mit `git tag -a v2.1.2 -m "uc2_cam_py 2.1.2" <commit>`.
  3. Der Nutzer pusht.
  - Liegt der Commit nicht auf `main`, auf die Merge-Art achten: Bei Squash oder Rebase zeigt
    der Tag auf einen Commit außerhalb von `main`.
- Auf dem Pi wiederholt der Nutzer danach Checkliste §8 (TSan mit `kernel8.img`).
