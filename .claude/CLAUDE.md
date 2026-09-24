# CLAUDE.md

## Arbeitsweise — verbindlich

**Immer zuerst planen.** Auch wenn im Prompt nicht ausdrücklich danach gefragt wird: erst in
den Planungsmodus gehen und einen Umsetzungsplan vorlegen. Implementiert wird **erst nach
ausdrücklicher Freigabe** durch den Nutzer. Reine Fragen, Recherche und Lesezugriffe sind
davon nicht betroffen — die Regel gilt für Änderungen am Projekt.

**Das Arbeitsverzeichnis `UC2_HIKpy` nicht verlassen.** `cd` nur innerhalb des Projekts.
Diese Einschränkung lässt sich als Berechtigungsregel nicht ausdrücken und steht deshalb hier.

Welche Befehle ohne Rückfrage laufen dürfen und welche gesperrt sind, steht — technisch
erzwungen statt nur befolgt — in **[settings.json](settings.json)** unter
`permissions.allow` / `permissions.deny`.

---

## Projektdokumentation

Der gesamte Inhalt steht in **[.claude/STATUS.md](STATUS.md)** — diese Datei hier ist nur der
Verweis darauf, weil sie beim Session-Start automatisch geladen wird.

**Vor nicht-trivialer Arbeit lesen:**

- **§1 Projekt und Aufbau** — Verzeichnisse, Schichtung
- **§2 Konventionen** — C++17, `namespace hik`, `hikLog.h` statt `printf`, `-Wall -Wextra`
- **§3 Fallstricke** — vier Dinge, die beim ersten Kontakt überraschen
- **§4 Build- und Testbefehle** — die zwei Konfigurationswege, beide müssen funktionieren
- **§11 Offene Punkte** — was bewusst nicht implementiert ist, plus die Review-Befunde

§5–§12 tragen den aktuellen Arbeitsstand (Ring Buffer, CTest-Integration).
