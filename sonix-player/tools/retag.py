#!/usr/bin/env python3
"""Rinomina i tag di lingua nel sorgente, solo dove sono davvero tag.

    tools/retag.py plan  <mappa.json>     dice cosa toccherebbe, senza scrivere
    tools/retag.py apply <mappa.json>     scrive

Il punto di tutto questo e' che un tag non si puo' rinominare con una
sostituzione di testo. "album" e' un tag dell'interfaccia in music.c, ma in
metadata.c e' il nome di un campo, in library.c una colonna SQL e in json.h una
chiave. Sostituire la stringa ovunque compare rompe tre cose per aggiustarne
una.

Quindi si riusa il cammino che estrae i tag -- tools/extract_strings.py -- e si
riscrive SOLO alle posizioni che quel cammino riconosce: l'argomento di tr(),
l'argomento giusto di una funzione che traduce da se', il campo etichetta di una
struct, le tabelle statiche che arrivano a un sink attraverso un indice.

strip_comments() lascia la lunghezza invariata, quindi le posizioni trovate sul
testo ripulito valgono anche sull'originale.
"""
import json
import os
import re
import sys
import glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_strings as E  # noqa: E402


# Una definizione che tiene un tag: #define NOME "tag", oppure
# static const char *const NOME = "tag".
DEFINE_RE = re.compile(
    r'^[ \t]*(?:#[ \t]*define[ \t]+(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]+'
    r'|static[ \t]+const[ \t]+char[ \t]*\*[^=\n]*?\b(?P<name2>[A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*)'
    r'(?P<lit>"[a-z0-9_]+")[ \t]*;?[ \t]*$', re.M)

# Coppie (file, nome) che hanno la forma di un tag e non lo sono.
NOT_A_TAG = {
    ("bookmarks.c", "SECTION"),  # una sezione di device_config.ini
}


def split_arg_spans(text):
    """Come split_args(), ma torna le posizioni invece del testo."""
    spans, depth, start, i, n = [], 0, 0, 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            spans.append((start, i))
            start = i + 1
        i += 1
    spans.append((start, n))
    return spans


def tag_spans(path, src):
    """Le posizioni (inizio, fine, contenuto, da quale passata) dei letterali
    che in questo file sono tag. Le stesse quattro passate di collect()."""
    out = []

    def take(fragment_start, fragment, why):
        for s, e, body in E.literals(fragment):
            out.append((fragment_start + s, fragment_start + e, body, why))

    # 1. tr("...")
    for m in re.finditer(r"\btr\s*\(", src):
        span = E.matching(src, m.end() - 1)
        if span:
            take(span[0], src[span[0]:span[1]], "tr")

    # 2. gli argomenti che una funzione traduce da se'
    sinks = tag_spans.sinks
    for name, positions in sinks.items():
        for m in re.finditer(r"\b%s\s*\(" % re.escape(name), src):
            span = E.matching(src, m.end() - 1)
            if not span:
                continue
            if src[span[1] + 1:].lstrip().startswith("{"):
                continue  # la definizione, non una chiamata
            args = split_arg_spans(src[span[0]:span[1]])
            for p in positions:
                if p < len(args):
                    a0, a1 = args[p]
                    take(span[0] + a0, src[span[0] + a0:span[0] + a1], "sink:" + name)

    # 3. il campo etichetta di una struct che chi la disegna traduce
    for struct in E.LABEL_STRUCTS:
        for m in re.finditer(r"\b%s\b" % re.escape(struct), src):
            span = E.matching(src, m.end(), "{", "}")
            if not span or span[0] - m.end() > 40:
                continue
            inner = src[span[0]:span[1]]
            rows, i = [], 0
            if re.match(r"\s*\{", inner):
                while True:
                    sub = E.matching(inner, i, "{", "}")
                    if not sub:
                        break
                    rows.append((sub[0], sub[1]))
                    i = sub[1] + 1
            else:
                rows = [(0, len(inner))]
            for r0, r1 in rows:
                first = split_arg_spans(inner[r0:r1])[0]
                take(span[0] + r0 + first[0], inner[r0 + first[0]:r0 + first[1]], "struct:" + struct)

    # 4. le tabelle statiche che arrivano a un sink per indice
    for table in E.TABLES:
        for m in re.finditer(r"\b%s\s*\[[^\]]*\]\s*=" % re.escape(table), src):
            span = E.matching(src, m.end(), "{", "}")
            if span:
                take(span[0], src[span[0]:span[1]], "table:" + table)

    # 5. il tag scritto una volta sola dietro un nome
    #
    # collect() lo risolve attraverso macro_strings()/const_strings() e da
    # li' non torna indietro, quindi la definizione va riscritta qui. Ce n'e'
    # una che NON e' un tag e ha lo stesso aspetto: #define SECTION
    # "bookmarks" in bookmarks.c e' il nome di una sezione del file di
    # configurazione, e rinominarla perderebbe i segnalibri di tutti.
    for m in DEFINE_RE.finditer(src):
        name = m.group("name") or m.group("name2")
        if (os.path.basename(path), name) in NOT_A_TAG:
            continue
        take(m.start("lit"), src[m.start("lit"):m.end("lit")], "define:" + name)

    return out


def sources():
    return sorted(p for p in glob.glob("src/gui/**/*.c", recursive=True)
                  + glob.glob("src/system/**/*.c", recursive=True)
                  if not p.endswith("icons.c"))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("plan", "apply"):
        print(__doc__)
        return 2
    action = sys.argv[1]
    mapping = json.load(open(sys.argv[2], encoding="utf-8"))

    paths = sources()
    stripped = {p: E.strip_comments(open(p, encoding="utf-8", errors="replace").read()) for p in paths}
    tag_spans.sinks = E.find_sinks(stripped)

    touched = 0
    files = 0
    unknown = {}
    for p in paths:
        original = open(p, encoding="utf-8", errors="replace").read()
        src = stripped[p]
        assert len(src) == len(original), p
        # Lo stesso letterale puo' essere trovato da due passate: gui_notify_popup(tr("x"))
        # lo vede sia come argomento di tr() sia come argomento del sink. Si
        # tiene per posizione, non per posizione-e-motivo, o la seconda
        # sostituzione riscriverebbe il nome gia' nuovo.
        spans = sorted({(s, e, body) for s, e, body, _ in tag_spans(p, src)})
        edits = []
        for s, e, body in spans:
            if body in mapping:
                edits.append((s, e, body, mapping[body], ""))
            elif re.fullmatch(r"[a-z0-9_]+", body) and len(body) > 1:
                unknown.setdefault(body, []).append(os.path.basename(p))
        if not edits:
            continue
        # dal fondo, cosi' le posizioni davanti restano valide
        edits.sort(key=lambda x: -x[0])
        text = original
        for s, e, old, new, why in edits:
            assert text[s:e] == '"' + old + '"', (p, s, text[s:e], old)
            text = text[:s] + '"' + new + '"' + text[e:]
        touched += len(edits)
        files += 1
        if action == "apply":
            open(p, "w", encoding="utf-8").write(text)

    print("%s: %d sostituzioni in %d file" % (action, touched, files))
    if unknown:
        print("\nletterali in posizione di tag ma non nella mappa (%d):" % len(unknown))
        for k in sorted(unknown)[:40]:
            print("   %-38s %s" % (k, unknown[k][0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
