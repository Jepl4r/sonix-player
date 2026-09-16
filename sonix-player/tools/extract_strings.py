#!/usr/bin/env python3
"""Pulls every translatable string out of the source, in the order it appears.

Two kinds of string end up in the language files:

  * the ones wrapped where they are written, `tr("...")`;
  * the ones handed to a function that translates its own parameter. Every
    settings row, popup, confirmation, pop-over item and pill works that way --
    translating at the sink means thirty call sites do not each have to
    remember to. Those literals are NOT wrapped at the call site (wrapping
    there would translate twice), so they have to be collected from it.

The second kind is why this is not a grep. The sinks are not a fixed list: any
function, in any file, that does `tr(x)` on one of its own `const char *`
parameters is one -- `settingsrow_add`, but also the little `make_pill` that
half the settings pages keep to themselves. So they are discovered rather than
listed: every function definition is read, the ones that translate a parameter
are noted along with which parameter, and then their call sites are harvested.

Running this is what keeps Italiano.ini honest. The file is generated from the
source, so a string cannot be in the interface and missing from the
translation, nor left in the translation after it has gone from the interface.
"""
import glob
import os
import re
import sys

# Tables whose entries reach a sink but cannot be wrapped where they are
# written, because a static initialiser has to be a constant expression. These
# are matched only against their definition, never against a use.
TABLES = [
    "EQ_DEFAULT_PRESETS",
    "mseb_band_name",
    "SCREEN_OFF",
    "AUTO_OFF",
    "FILTER_NAMES",
    "SLEEP_LABELS",
    "REWIND_LABELS",
    "KEYMAP_ACTION_NAMES",
    # Le pillole della qualita' audio di Qobuz e di Tidal. Meta' di ogni nome e'
    # una parola ("Bassa", "Alta", "Alta risoluzione"), quindi vanno tradotte --
    # e non lo erano: passavano da tr(), ma non essendo qui dentro non
    # finivano nei file di lingua, e tr() di una chiave che non c'e' torna la
    # chiave. Da fuori: le uniche pillole che restavano in italiano.
    "QUALITY_NAMES",
    # Le pillole delle impostazioni di Gearboy: le palette vanno tradotte, i
    # nomi degli shader restano quelli ma passano dallo stesso sink.
    "PALETTE_NAMES",
    "SHADER_NAMES",
    # I nomi dei mesi che la pagina dei podcast usa per la data di un episodio.
    # Scritti a mano invece che con strftime, perche' quella segue la lingua del
    # SISTEMA -- sempre la stessa su questo dispositivo -- e non quella che
    # l'utente ha scelto per l'interfaccia.
    "PODCAST_MONTHS",
    # Le vetrine di Tidal: i nomi arrivano al sink come FEATURED_NAMES[i], un
    # identificativo indicizzato che il cammino delle chiamate non sa
    # risolvere. Per anni due voci su tre sono rimaste fuori dai file di
    # lingua proprio per questo.
    "FEATURED_NAMES",
    # Il lettore di EPUB. I nomi delle tre impostazioni del testo e i titoli
    # delle tre schede del menu arrivano al sink come stepper_tags[i] e
    # SECTION_TITLES[which]: un identificativo indicizzato, che il cammino delle
    # chiamate non sa risolvere. Senza queste due righe le sei voci passano da
    # tr() ma non finiscono nei file di lingua, e tr() di una chiave che non c'e'
    # torna la chiave -- cioe' "text_size" scritto sullo schermo.
    "stepper_tags",
    "SECTION_TITLES",
    # I nomi delle cose che si possono mettere sulla barra in fondo a una pagina
    # di libro: arrivano al sink come BAR_TAGS[which], stesso caso dei due qui
    # sopra.
    "BAR_TAGS",
    # E le due cose che la riga di avanzamento della stessa barra puo' misurare,
    # il libro o il capitolo: arrivano al sink come SCOPE_TAGS[scope].
    "SCOPE_TAGS",
    # I pulsanti del centro di controllo. Ogni voce porta due stringhe: la
    # chiave del file di configurazione e il tag che la pagina delle
    # impostazioni traduce, e quest'ultimo arriva al sink come
    # tr(quickpanel_button_tag(b)) -- un giro che il cammino delle chiamate non
    # sa seguire. Finora ogni tag capitava di esistere anche altrove; il primo
    # che non ce l'aveva (l'uscita USB-C) non finiva nei file di lingua.
    "button_tag",
    # I fusi orari: trentotto righe di citta' accanto all'offset. Erano scritte
    # in italiano dentro la tabella e disegnate cosi' com'erano, quindi la
    # pagina dei fusi era l'unica che restava in italiano in tutte le altre
    # lingue. Ora la tabella porta i tag e li traduce chi disegna la riga.
    "TIMEZONES",
    # Le due frasi per una lista di stazioni vuota. Arrivano a set_message(),
    # che il cammino delle chiamate riconosce come sink -- ma ci arrivano come
    # valore di ritorno di un'altra funzione, e un valore di ritorno non si
    # legge dal punto di chiamata. Nessuna delle due era in nessun file di
    # lingua: la pagina dei preferiti scriveva "no_favourite_stations".
    "STORED_EMPTY",
]

# Struct types whose `label` field is translated by whoever draws them
# (popover.c and gridpage.c both do `tr(item->label)`). Those literals sit in
# initialisers, so they are found by the shape of the initialiser rather than
# by following a call.
LABEL_STRUCTS = ["popover_item_t", "grid_entry_t"]

# The sinks that cannot be discovered by following a parameter, because the
# text goes through a buffer on the way: gui_notify_popup() strncpy's its
# argument into an event record, and only the far side of that hop calls tr().
EXTRA_SINKS = {
    "gui_notify_popup": {0},
    "gui_notify_popup_icon": {0},
}


def strip_comments(s):
    """Blanks out comments, keeping the length so offsets stay meaningful."""
    out, i, n = [], 0, len(s)
    while i < n:
        if s.startswith("//", i):
            j = s.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif s.startswith("/*", i):
            j = s.find("*/", i)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in s[i:j]))
            i = j
        elif s[i] == "'":
            # UN CHAR LITERAL, E NON E' PIGNOLERIA: dentro un '"' c'e' una
            # virgoletta doppia, e chi non salta i char literal da li' in poi
            # legge il file AL CONTRARIO -- il codice come stringhe e le
            # stringhe come codice. E' successo davvero, in radio.c: sette
            # '"' nei parser JSON, e due frasi della radio non venivano mai
            # estratte. Si copia com'e', gestendo l'escape ('\'' e '\\').
            j = i + 1
            while j < n:
                if s[j] == "\\":
                    j += 2
                    continue
                if s[j] == "'":
                    break
                j += 1
            out.append(s[i : j + 1])
            i = j + 1
        elif s[i] == '"':
            j = i + 1
            while j < n:
                if s[j] == "\\":
                    j += 2
                    continue
                if s[j] == '"':
                    break
                j += 1
            out.append(s[i : j + 1])
            i = j + 1
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def literals(text):
    """Every string literal in `text`, as (start, end, contents).

    I char literal si saltano per la stessa ragione detta in strip_comments:
    un '"' non gestito inverte stringhe e codice da li' alla fine del file.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    break
                j += 1
            i = j + 1
            continue
        if c != '"':
            i += 1
            continue
        j = i + 1
        while j < n:
            if text[j] == "\\":
                j += 2
                continue
            if text[j] == '"':
                break
            j += 1
        out.append((i, j + 1, text[i + 1 : j]))
        i = j + 1
    return out


def grouped_literals(text):
    """The distinct strings in `text`.

    C joins literals that are only separated by whitespace, which is how a long
    sentence is written across several lines -- those become one string. A `?`
    or a `:` between them means they are alternatives, and those stay separate.
    Splitting the text on `?` and `:` before finding the literals would be the
    obvious way to do this and is wrong: half the sentences in this interface
    contain a colon.
    """
    lits = literals(text)
    out, current = [], None
    prev_end = None
    for start, end, body in lits:
        if current is not None and text[prev_end:start].strip() == "":
            current += body
        else:
            if current is not None:
                out.append(current)
            current = body
        prev_end = end
    if current is not None:
        out.append(current)
    return out


def matching(text, at, opener="(", closer=")"):
    """The span inside the brackets that open at or after `at`."""
    start = text.find(opener, at)
    if start < 0:
        return None
    depth, i, n = 0, start, len(text)
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
        elif c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return (start + 1, i)
        i += 1
    return None


def split_args(text):
    """A call's arguments, split at top-level commas."""
    args, depth, start, i, n = [], 0, 0, 0, len(text)
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
            args.append(text[start:i])
            start = i + 1
        i += 1
    args.append(text[start:])
    return args


FUNC_DEF = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.M)


def find_sinks(sources):
    """Functions that call tr() on one of their own const char * parameters."""
    sinks = {}
    for src in sources.values():
        for m in FUNC_DEF.finditer(src):
            name = m.group(1)
            if name in ("if", "for", "while", "switch", "return", "sizeof", "tr"):
                continue
            args_span = matching(src, m.end() - 1)
            if not args_span:
                continue
            # A definition, not a call: the parenthesis is followed by a body.
            after = src[args_span[1] + 1 :].lstrip()
            if not after.startswith("{"):
                continue
            body_span = matching(src, args_span[1] + 1, "{", "}")
            if not body_span:
                continue
            body = src[body_span[0] : body_span[1]]

            params = split_args(src[args_span[0] : args_span[1]])
            for index, param in enumerate(params):
                pm = re.search(r"const\s+char\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*$", param.strip())
                if not pm:
                    continue
                var = pm.group(1)
                if re.search(r"\btr\s*\(\s*%s\b" % re.escape(var), body):
                    sinks.setdefault(name, set()).add(index)

    for name, positions in EXTRA_SINKS.items():
        sinks.setdefault(name, set()).update(positions)

    # A function that hands one of its own parameters straight to a sink is a
    # sink too: settingsrow_page() only calls settingsrow_title(), and
    # settingsrow_toggle() only calls make_card(). Repeated until nothing new
    # turns up, because some of those chains are three deep.
    changed = True
    while changed:
        changed = False
        for src in sources.values():
            for m in FUNC_DEF.finditer(src):
                name = m.group(1)
                args_span = matching(src, m.end() - 1)
                if not args_span:
                    continue
                if not src[args_span[1] + 1:].lstrip().startswith("{"):
                    continue
                body_span = matching(src, args_span[1] + 1, "{", "}")
                if not body_span:
                    continue
                body = src[body_span[0]:body_span[1]]
                params = split_args(src[args_span[0]:args_span[1]])

                for index, param in enumerate(params):
                    pm = re.search(r"const\s+char\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*$", param.strip())
                    if not pm or index in sinks.get(name, set()):
                        continue
                    var = pm.group(1)
                    for inner, positions in list(sinks.items()):
                        for call in re.finditer(r"\b%s\s*\(" % re.escape(inner), body):
                            span = matching(body, call.end() - 1)
                            if not span:
                                continue
                            inner_args = split_args(body[span[0]:span[1]])
                            for p in positions:
                                if p < len(inner_args) and inner_args[p].strip() == var:
                                    sinks.setdefault(name, set()).add(index)
                                    changed = True
    return sinks


DEFINE = re.compile(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*$', re.M)


CONST_STR = re.compile(
    r'\bconst\s+char\s*\*\s*(?:const\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*'
    r'((?:"(?:[^"\\]|\\.)*"\s*)+);')


def const_strings(src):
    """`static const char *const MONTHS = "Gennaio\nFebbraio\n...";`

    The same problem as the macros, one level along: the roller of month names
    is written once as a named constant and handed to a sink as an identifier,
    so there is no literal at the call site to pick up. Missing it is how the
    months stayed Italian in every other language.
    """
    out = {}
    for m in CONST_STR.finditer(src):
        out[m.group(1)] = "".join(body for _, _, body in literals(m.group(2)))
    return out


def macro_strings(src):
    """`#define AP_STARTING "Attivazione in corso..."` and friends.

    Several pages keep their longer sentences in macros so the code that draws
    them stays readable. `tr(AP_STARTING)` carries no literal of its own, so
    without this the text would be in the interface and missing from the
    language file -- which is exactly the failure this script exists to
    prevent.
    """
    out = {}
    for m in DEFINE.finditer(src):
        joined = "".join(body for _, _, body in literals(m.group(2)))
        out[m.group(1)] = joined
    return out


# The Wi-Fi transfer page is served to a browser rather than drawn by LVGL, so
# none of the machinery above can see it. Its translatable text is marked in
# the template instead, and the player substitutes at the moment it writes the
# page out (install_page in wifitransfer.c). Same keys, same files: a word that
# appears on both the screen and the page is translated once.
WEB_PAGE = "web/index.html"
WEB_MARK = re.compile(r"\{\{(.*?)\}\}", re.S)


def web_strings(root):
    path = os.path.join(root, WEB_PAGE)
    if not os.path.exists(path):
        return []
    text = open(path, encoding="utf-8", errors="replace").read()
    out, seen = [], set()
    for m in WEB_MARK.finditer(text):
        body = m.group(1)
        # "js|" says how the translation is escaped once it is in the page, not
        # what is being translated: the key is the same either way, so a word
        # used in both places is translated once.
        if body.startswith("js|"):
            body = body[3:]
        if body not in seen:
            seen.add(body)
            out.append(body)
    return out


collect_root = ["."]


def collect(paths):
    sources = {p: strip_comments(open(p, encoding="utf-8", errors="replace").read()) for p in paths}
    sinks = find_sinks(sources)

    found, seen = [], set()
    unresolved = set()

    def add(s, path):
        if s in seen:
            return
        # Nothing to translate in a string that is only formats, escapes and
        # symbols. The escapes have to go before the letter test or a bare
        # "\n" passes it on the strength of its own n.
        bare = re.sub(r"\\.", "", s)
        bare = re.sub(r"%[-+ #0-9.]*[a-zA-Z]", "", bare)
        if len(s) < 2 or not re.search(r"[A-Za-z\xc0-\xff]", bare):
            return
        seen.add(s)
        found.append((s, path))

    for path in paths:
        src = sources[path]
        named = macro_strings(src)
        named.update(const_strings(src))

        def texts(fragment, where=""):
            """The strings in a call argument: literals, or a name standing for one."""
            found_here = grouped_literals(fragment)
            if found_here:
                return found_here
            name = fragment.strip()
            if name in named:
                return [named[name]]
            # A NAME IN CAPITALS that resolved to nothing is worth saying out
            # loud: it is a constant this script cannot see, i.e. a string
            # silently missing from every translation -- which is exactly what
            # happened to the month names. Lower-case identifiers are the
            # ordinary case (a parameter inside a sink's own definition, or a
            # runtime string) and would be nothing but noise. NULL is neither.
            if name != "NULL" and re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
                unresolved.add((os.path.basename(path), where, name))
            return []

        # 1. tr("..."), including tr(cond ? "a" : "b") and tr(SOME_MACRO)
        for m in re.finditer(r"\btr\s*\(", src):
            span = matching(src, m.end() - 1)
            if span:
                for s in texts(src[span[0] : span[1]], "tr"):
                    add(s, path)

        # 2. literals handed to something that translates them itself
        for name, positions in sinks.items():
            for m in re.finditer(r"\b%s\s*\(" % re.escape(name), src):
                # skip the definition itself
                if src[: m.start()].rstrip().endswith(("static", "*", "t", ")")) and False:
                    pass
                span = matching(src, m.end() - 1)
                if not span:
                    continue
                after = src[span[1] + 1 :].lstrip()
                if after.startswith("{"):
                    continue # this is the definition
                args = split_args(src[span[0] : span[1]])
                for p in positions:
                    if p < len(args):
                        for s in texts(args[p], name):
                            add(s, path)

        # 3. the label field of a struct that whoever draws it translates
        for struct in LABEL_STRUCTS:
            for m in re.finditer(r"\b%s\b" % re.escape(struct), src):
                span = matching(src, m.end(), "{", "}")
                if not span or span[0] - m.end() > 40:
                    continue
                inner = src[span[0]:span[1]]
                # An array of them is several braced rows, each beginning with
                # the label; a single compound literal is one row.
                rows, i = [], 0
                if re.match(r"\s*\{", inner):
                    while True:
                        sub = matching(inner, i, "{", "}")
                        if not sub:
                            break
                        rows.append(inner[sub[0]:sub[1]])
                        i = sub[1] + 1
                else:
                    rows = [inner]
                for row in rows:
                    for s2 in grouped_literals(split_args(row)[0]):
                        add(s2, path)

        # 3b. lo stesso campo, ma riempito una riga alla volta invece che con un
        # initializer: `item.label = "delete_playlist";` dentro uno switch. Va
        # nello stesso posto e viene tradotto dagli stessi tr(), ma la regola
        # sopra guarda la forma dell'initializer e queste righe non ce l'hanno.
        if any(re.search(r"\b%s\b" % re.escape(s), src) for s in LABEL_STRUCTS):
            for m in re.finditer(r"\.label\s*=\s*(\"(?:[^\"\\]|\\.)*\")", src):
                for s2 in grouped_literals(m.group(1)):
                    add(s2, path)

        # 4. static tables that reach a sink through an index
        for table in TABLES:
            # The size may be a number or a constant: SLEEP_LABELS[SLEEP_CHOICES] = {...}
            for m in re.finditer(r"\b%s\s*\[[^\]]*\]\s*=" % re.escape(table), src):
                span = matching(src, m.end(), "{", "}")
                if span:
                    for _, _, body in literals(src[span[0] : span[1]]):
                        add(body, path)

    # The page last, so the C strings keep the order they are declared in and
    # the page's own additions are grouped at the end of the file.
    for body in web_strings(collect_root[0]):
        add(body, WEB_PAGE)

    return found, sinks, unresolved


if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    paths = sorted(
        p
        for p in glob.glob(os.path.join(root, "src/gui/**/*.c"), recursive=True)
        + glob.glob(os.path.join(root, "src/system/**/*.c"), recursive=True)
        if not p.endswith("icons.c")
    )
    collect_root[0] = root
    found, sinks, unresolved = collect(paths)
    for where in sorted(unresolved):
        print("non risolto: %s -> %s(%s)" % where, file=sys.stderr)
    if "--sinks" in sys.argv:
        for name in sorted(sinks):
            print("sink: %s%s" % (name, sorted(sinks[name])), file=sys.stderr)
    for s, path in found:
        print("%s\t%s" % (os.path.basename(path), s))
