#!/usr/bin/env python3
"""Costruisce le copertine su cui gira run_covers_bench.sh.

Lo scopo non e' la bellezza: sono le forme. Nel log dell'utente, su 92
decodifiche 12 non erano quadrate -- 360x357, 300x293, 297x300 -- e le
dimensioni dispari sono esattamente dove un errore di un pixel nell'aritmetica
dei buffer si vede e altrove no. Qui ce ne sono di tutte le proporzioni, piu' i
casi che nessuno prova mai: un pixel, una riga sola, una colonna sola, il
progressivo, l'interlacciato, i 16 bit per canale, il grigio, il file troncato.

Uso: make_cover_zoo.py <cartella>
"""
import os
import random
import shutil
import sys

from PIL import Image


def picture(w, h, seed):
    """Una figura con del contrasto vero: un'immagine piatta non fa lavorare
    ne' la media del rimpicciolimento ne' il dither del pack a 565."""
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    base = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
    for y in range(h):
        for x in range(w):
            band = ((x * 8) // max(w, 1) + (y * 8) // max(h, 1)) & 1
            px[x, y] = base if band else (255 - base[0], 255 - base[1], 255 - base[2])
    return im


SHAPES = [
    # quadrate, le normali
    (400, 400), (300, 300), (1000, 1000),
    # le dispari viste nel log dell'utente
    (400, 397), (400, 398), (300, 293), (297, 300), (300, 299),
    # rapporti estremi in tutte e due le direzioni
    (1200, 300), (300, 1200), (2000, 17), (17, 2000),
    # i bordi veri
    (1, 1), (1, 400), (400, 1), (2, 3), (3, 2), (7, 7),
    # piu' grandi del riquadro chiesto, e piu' piccole
    (3000, 3000), (2400, 1350), (31, 29),
    # numeri primi, cosi' nessuna divisione torna tonda
    (401, 399), (523, 521), (127, 129),
]


def main():
    if len(sys.argv) != 2:
        sys.exit("uso: make_cover_zoo.py <cartella>")
    d = sys.argv[1]
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)

    n = 0
    for i, (w, h) in enumerate(SHAPES):
        im = picture(w, h, i)
        im.save(os.path.join(d, "b_%04dx%04d.jpg" % (w, h)), quality=88)
        im.save(os.path.join(d, "p_%04dx%04d.png" % (w, h)))
        n += 2

    # Il progressivo: non lo legge TJpgDec, quindi prende la strada dei piani
    # dentro stb, che e' un'altra aritmetica.
    for w, h in [(1200, 1200), (401, 399), (2400, 1350), (3, 2)]:
        picture(w, h, 100 + w).save(
            os.path.join(d, "prog_%04dx%04d.jpg" % (w, h)), quality=88, progressive=True)
        n += 1

    # PNG interlacciato e a 16 bit: tutti e due cadono fuori dal decodificatore
    # a scansione e finiscono in stb.
    picture(300, 293, 7).save(os.path.join(d, "inter_0300x0293.png"), interlace=True)
    picture(401, 399, 8).convert("L").convert("I;16").save(os.path.join(d, "i16_0401x0399.png"))
    picture(297, 300, 9).convert("L").save(os.path.join(d, "gray_0297x0300.png"))
    picture(400, 397, 10).convert("LA").save(os.path.join(d, "graya_0400x0397.png"))
    picture(400, 398, 11).convert("RGBA").save(os.path.join(d, "rgba_0400x0398.png"))
    picture(300, 300, 12).convert("CMYK").save(os.path.join(d, "cmyk_0300x0300.jpg"))
    n += 6

    # E i file rotti, che sul campo esistono: un tag tagliato a meta' e' un JPEG
    # troncato, e il decodificatore deve fermarsi senza uscire dal buffer.
    whole = open(os.path.join(d, "b_0400x0400.jpg"), "rb").read()
    for cut, name in ((len(whole) // 2, "troncato_meta.jpg"),
                      (len(whole) - 3, "troncato_coda.jpg"),
                      (200, "troncato_testa.jpg")):
        open(os.path.join(d, name), "wb").write(whole[:cut])
        n += 1
    # Bytes che cominciano come un JPEG e poi non lo sono.
    open(os.path.join(d, "finto.jpg"), "wb").write(b"\xff\xd8" + os.urandom(4096))
    open(os.path.join(d, "vuoto.jpg"), "wb").write(b"")
    n += 2

    print("  %d immagini in %s" % (n, d))


if __name__ == "__main__":
    main()
