#!/usr/bin/env python3
"""Costruisce i libri su cui gira run_ebook_bench.sh.

I file li scrive `zipfile`, non codice mio: cosi' il lettore di ZIP e di OPF si
misura contro archivi veri, prodotti dalla libreria standard, e non contro
qualcosa costruito con le stesse assunzioni che dovrebbe provare.

I libri sono scelti per i casi che rompono un lettore:

  epub3.epub   EPUB 3 con indice di navigazione, grassetto, corsivo, liste,
               citazioni, <script>, <style>, un elemento sconosciuto, entita'
               e un capitolo grande;
  epub2.epub   EPUB 2 con l'indice NCX, l'OPF in una sottocartella e href che
               risalgono con "../";
  stile.epub   immagini in una sottocartella, un foglio di stile collegato e
               uno interno, corsivo e grassetto scritti come classi (che e'
               come li scrive quasi ogni convertitore), testo centrato,
               display:none, e una copertina che e' un <svg><image>;
  grosso.epub  un libro di dimensioni vere, per misurare la memoria;
  rotto.epub   non e' uno zip;
  vuoto.epub   e' uno zip ma non e' un libro.

Uso: make_test_epub.py <cartella>
"""
import os
import sys
import zipfile

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="%s" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>"""


def new_epub(path):
    z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED)
    # Il mimetype va memorizzato senza compressione ed e' il primo membro:
    # cosi' vuole la specifica, e cosi' lo scrive chiunque produca EPUB.
    z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    return z


def build_epub3(path):
    z = new_epub(path)
    z.writestr("META-INF/container.xml", CONTAINER % "OEBPS/content.opf")

    z.writestr(
        "OEBPS/content.opf",
        """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Il libro di prova</dc:title>
    <dc:creator>Autore Tale</dc:creator>
    <dc:language>it</dc:language>
    <dc:identifier id="uid">urn:uuid:prova</dc:identifier>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="cov" href="images/cover.png" media-type="image/png" properties="cover-image"/>
    <item id="c1" href="text/ch01.xhtml" media-type="application/xhtml+xml"/>
    <item id="c2" href="text/ch02.xhtml" media-type="application/xhtml+xml"/>
    <item id="c3" href="text/ch03.xhtml" media-type="application/xhtml+xml"/>
    <item id="c4" href="text/ch04.xhtml" media-type="application/xhtml+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
  </manifest>
  <spine>
    <itemref idref="c1"/>
    <itemref idref="c2"/>
    <itemref idref="c3"/>
    <itemref idref="c4"/>
  </spine>
</package>""",
    )

    # L'indice punta ai capitoli con "text/..." dalla cartella dell'OPF.
    z.writestr(
        "OEBPS/nav.xhtml",
        """<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Indice</title></head>
<body>
  <nav epub:type="toc">
    <ol>
      <li><a href="text/ch01.xhtml">Primo capitolo</a></li>
      <li><a href="text/ch02.xhtml#inizio">Secondo capitolo</a></li>
      <li><a href="text/ch03.xhtml">Terzo capitolo</a>
        <ol><li><a href="text/ch04.xhtml">Un sottocapitolo</a></li></ol>
      </li>
    </ol>
  </nav>
  <nav epub:type="landmarks"><ol><li><a href="text/ch01.xhtml">Inizio</a></li></ol></nav>
</body></html>""",
    )

    # Il capitolo che porta tutti i casi difficili.
    z.writestr(
        "OEBPS/text/ch01.xhtml",
        """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>Questo titolo non va letto</title>
  <style type="text/css">p { color: COLOREROSSO; }</style>
  <script>var x = "NONDEVEUSCIRE";</script>
</head>
<body>
  <h1>Primo capitolo</h1>
  <p>Frase di prova con del <strong>grassetto</strong> e del <em>corsivo</em>.</p>
  <p>Una riga
     spezzata     su   piu'  righe con spazi   assurdi.</p>
  <section><p>Testo dentro un tag sconosciuto.</p></section>
  <ul><li>Prima voce</li><li>Seconda voce</li></ul>
  <blockquote><p>Una citazione.</p></blockquote>
  <p>Entita&#768;: &amp; &lt; &gt; &quot; &#233; &egrave; &mdash; e un &bogus; lasciato stare.</p>
  <p></p>
  <hr/>
  <p>Dopo la riga.<br/>Su una riga nuova.</p>
  <p>Un <i>corsivo <b>dentro cui</b> c'e' del grassetto</i>.</p>
</body></html>""",
    )

    z.writestr(
        "OEBPS/text/ch02.xhtml",
        """<html xmlns="http://www.w3.org/1999/xhtml"><body>
<h1 id="inizio">Secondo capitolo</h1>
<p>%s</p>
</body></html>"""
        % ("Testo del secondo capitolo. " * 40),
    )

    # Grande apposta: e' quello che fa crescere l'arena del capitolo.
    z.writestr(
        "OEBPS/text/ch03.xhtml",
        "<html><body><h1>Terzo capitolo</h1>"
        + "".join("<p>Paragrafo numero %d di un capitolo lungo. %s</p>" % (i, "Parole in piu'. " * 20)
                  for i in range(400))
        + "</body></html>",
    )

    # E questo torna piccolo: serve a vedere che il precedente e' stato buttato.
    z.writestr(
        "OEBPS/text/ch04.xhtml",
        "<html><body><h1>Un sottocapitolo</h1><p>Corto.</p></body></html>",
    )

    z.writestr("OEBPS/style.css", "body { margin: 0; }")
    z.writestr("OEBPS/images/cover.png", b"\x89PNG\r\n\x1a\n" + b"\x00" * 64)
    z.close()


def build_epub2(path):
    """L'OPF sta in una sottocartella e l'indice risale con '../'."""
    z = new_epub(path)
    z.writestr("META-INF/container.xml", CONTAINER % "book/opf/package.opf")

    z.writestr(
        "book/opf/package.opf",
        """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Libro vecchio</dc:title>
    <dc:creator opf:role="aut">Qualcuno</dc:creator>
    <dc:identifier id="uid">x</dc:identifier>
    <meta name="cover" content="cov"/>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="cov" href="../img/c.png" media-type="image/png"/>
    <item id="c1" href="../text/uno.xhtml" media-type="application/xhtml+xml"/>
    <item id="c2" href="../text/due.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="c1"/>
    <itemref idref="c2"/>
  </spine>
</package>""",
    )

    z.writestr(
        "book/opf/toc.ncx",
        """<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <navMap>
    <navPoint id="n1" playOrder="1">
      <navLabel><text>Capitolo uno</text></navLabel>
      <content src="../text/uno.xhtml"/>
    </navPoint>
    <navPoint id="n2" playOrder="2">
      <navLabel><text>Capitolo due</text></navLabel>
      <content src="../text/due.xhtml"/>
    </navPoint>
  </navMap>
</ncx>""",
    )

    z.writestr("book/text/uno.xhtml", "<html><body><h1>Capitolo uno</h1><p>Testo.</p></body></html>")
    z.writestr(
        "book/text/due.xhtml",
        "<html><body><h1>Capitolo due</h1><p>Questo libro vive in una sottocartella.</p></body></html>",
    )
    z.writestr("book/img/c.png", b"\x89PNG\r\n\x1a\n" + b"\x00" * 64)
    z.close()


def build_big(path, chapters=12, paragraphs=500):
    z = new_epub(path)
    z.writestr("META-INF/container.xml", CONTAINER % "content.opf")
    items = "".join(
        '<item id="c%d" href="ch%02d.xhtml" media-type="application/xhtml+xml"/>' % (i, i)
        for i in range(chapters)
    )
    refs = "".join('<itemref idref="c%d"/>' % i for i in range(chapters))
    z.writestr(
        "content.opf",
        """<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="u">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Libro grosso</dc:title>
 <dc:identifier id="u">g</dc:identifier></metadata>
 <manifest>%s</manifest><spine>%s</spine></package>"""
        % (items, refs),
    )
    body = "".join(
        "<p>Paragrafo %d. %s</p>" % (k, "Una frase di riempimento lunga abbastanza da contare. " * 8)
        for k in range(paragraphs)
    )
    for i in range(chapters):
        z.writestr("ch%02d.xhtml" % i, "<html><body><h1>Capitolo %d</h1>%s</body></html>" % (i, body))
    z.close()


# Un PNG vero, piccolo: 4x4 grigi. Scritto a mano perche' il bench non deve
# dipendere da PIL, e perche' quello che si misura e' che il lettore trovi
# l'immagine e ne sappia la misura, non che sia bella.
def tiny_png(width=4, height=4):
    import struct
    import zlib

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes([(x * 60) % 256] * 3 * width) for x in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


STYLE_SHEET = """
/* un commento con una } dentro, che rompe un parser ingenuo */
p { margin: 1em 0; line-height: 1.4; }
.corsivo { font-style: italic; }
.grassetto { font-weight: 700; }
p.centrato, div.centrato { text-align: center; }
.nascosto { display: none; }
.normale { font-style: normal; }
@media screen and (max-width: 400px) { .corsivo { font-weight: bold; } }
h1 > span { color: red; }
#unico { font-style: italic; }
"""


def build_styled(path):
    z = new_epub(path)
    z.writestr("META-INF/container.xml", CONTAINER % "OEBPS/content.opf")
    z.writestr("OEBPS/css/stile.css", STYLE_SHEET)
    z.writestr("OEBPS/images/fig1.png", tiny_png(4, 4))
    z.writestr("OEBPS/images/fig2.png", tiny_png(8, 6))
    z.writestr("OEBPS/images/copertina.png", tiny_png(12, 18))

    # La copertina come <svg><image>, che e' come la scrive meta' dei
    # convertitori, e con l'href che risale di una cartella.
    z.writestr("OEBPS/text/copertina.xhtml", """<?xml version='1.0' encoding='utf-8'?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Copertina</title>
<link rel="stylesheet" type="text/css" href="../css/stile.css"/></head>
<body><div class="centrato"><svg xmlns="http://www.w3.org/2000/svg"
 xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 12 18">
<image width="12" height="18" xlink:href="../images/copertina.png"/></svg></div></body></html>""")

    z.writestr("OEBPS/text/uno.xhtml", """<?xml version='1.0' encoding='utf-8'?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Uno</title>
<link rel="stylesheet" type="text/css" href="../css/stile.css"/>
<style type="text/css">.solo-qui { font-weight: bold; } p.destra { text-align: right; }</style>
</head><body>
<h1>Il capitolo con le figure</h1>
<p>Una riga normale.</p>
<p>Una con <span class="corsivo">il corsivo di classe</span> dentro.</p>
<p>Una con <span class="grassetto">il grassetto di classe</span> dentro.</p>
<p>Una con <span class="solo-qui">il grassetto del foglio interno</span> dentro.</p>
<p class="centrato">Questa e' centrata.</p>
<p class="destra">Questa e' a destra.</p>
<p class="nascosto">Questa non si deve vedere per niente.</p>
<p><span class="corsivo">fuori<span class="normale">dentro</span></span></p>
<p>Prima della figura.</p>
<img src="../images/fig1.png" alt="una figura"/>
<p>In mezzo.</p>
<p><img src="../images/fig2.png"/></p>
<img src="../images/manca.png"/>
<p>Dopo la figura.</p>
</body></html>""")

    # L'indice accanto ai capitoli, non accanto all'OPF: i suoi href sono
    # relativi a se stesso, ed e' il caso che faceva sparire l'indice intero.
    z.writestr("OEBPS/text/nav.xhtml", """<?xml version='1.0' encoding='utf-8'?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<body><nav epub:type="toc"><ol>
<li><a href="copertina.xhtml">La copertina</a></li>
<li><a href="uno.xhtml">Il capitolo con le figure</a></li>
</ol></nav></body></html>""")

    z.writestr("OEBPS/content.opf", """<?xml version='1.0' encoding='utf-8'?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">urn:uuid:stile</dc:identifier>
<dc:title>Immagini e stile</dc:title><dc:language>it</dc:language>
<dc:creator>La prova</dc:creator></metadata>
<manifest>
<item id="cov" href="images/copertina.png" media-type="image/png" properties="cover-image"/>
<item id="css" href="css/stile.css" media-type="text/css"/>
<item id="nav" href="text/nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="c0" href="text/copertina.xhtml" media-type="application/xhtml+xml"/>
<item id="c1" href="text/uno.xhtml" media-type="application/xhtml+xml"/>
</manifest>
<spine><itemref idref="c0"/><itemref idref="c1"/></spine></package>""")
    z.close()


def build_broken(dirname):
    with open(os.path.join(dirname, "rotto.epub"), "wb") as f:
        f.write(b"questo non e' uno zip, e nemmeno per sbaglio" * 10)
    z = zipfile.ZipFile(os.path.join(dirname, "vuoto.epub"), "w", zipfile.ZIP_DEFLATED)
    z.writestr("qualcosa.txt", "uno zip senza container.xml")
    z.close()


def main():
    if len(sys.argv) != 2:
        sys.exit("uso: make_test_epub.py <cartella>")
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    build_epub3(os.path.join(d, "epub3.epub"))
    build_epub2(os.path.join(d, "epub2.epub"))
    build_styled(os.path.join(d, "stile.epub"))
    build_big(os.path.join(d, "grosso.epub"))
    build_broken(d)
    for name in sorted(os.listdir(d)):
        if name.endswith(".epub"):
            print("  %-14s %8d byte" % (name, os.path.getsize(os.path.join(d, name))))


if __name__ == "__main__":
    main()
