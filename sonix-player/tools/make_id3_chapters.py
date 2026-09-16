#!/usr/bin/env python3
"""Costruisce gli MP3 su cui gira run_id3chap_bench.sh.

I tag li scrive questo file a mano, byte per byte, perche' il lettore deve
misurarsi contro la forma vera di ID3v2 -- compresa la differenza fra la
dimensione sincsafe della 2.4 e quella normale della 2.3, che e' l'errore che
fa leggere a meta' ogni frame.

I file non sono MP3 validi: dopo il tag c'e' un pugno di byte qualsiasi. Non
importa, perche' quello che si prova e' il tag.

Uso: make_id3_chapters.py <cartella>
"""
import os
import struct
import sys


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def frame(fid, payload, version):
    size = syncsafe(len(payload)) if version >= 4 else struct.pack(">I", len(payload))
    return fid + size + b"\x00\x00" + payload


def tit2(title, encoding=0):
    if encoding == 0:
        return frame(b"TIT2", b"\x00" + title.encode("latin-1", "replace"), 3)
    if encoding == 1:  # UTF-16 con BOM little endian
        return frame(b"TIT2", b"\x01\xff\xfe" + title.encode("utf-16-le"), 3)
    return frame(b"TIT2", b"\x03" + title.encode("utf-8"), 3)


def chap(element, start_ms, end_ms, title, version, encoding=0):
    payload = element.encode("ascii") + b"\x00"
    payload += struct.pack(">IIII", start_ms, end_ms, 0, 0)
    if title is not None:
        # I sotto-frame hanno la stessa forma dei frame, quindi anche la stessa
        # differenza di dimensione fra 2.3 e 2.4.
        sub = b"\x00" + title.encode("latin-1", "replace") if encoding == 0 else None
        if encoding == 0:
            payload += frame(b"TIT2", sub, version)
        elif encoding == 1:
            payload += frame(b"TIT2", b"\x01\xff\xfe" + title.encode("utf-16-le"), version)
        else:
            payload += frame(b"TIT2", b"\x03" + title.encode("utf-8"), version)
    return frame(b"CHAP", payload, version)


def write_mp3(path, frames, version=4, extended=False, padding=0):
    body = b"".join(frames) + b"\x00" * padding
    flags = 0x40 if extended else 0x00
    if extended:
        ext = syncsafe(6) + b"\x01\x00" if version >= 4 else struct.pack(">I", 2) + b"\x00\x00"
        body = ext + body
    header = b"ID3" + bytes([version, 0]) + bytes([flags]) + syncsafe(len(body))
    with open(path, "wb") as f:
        f.write(header + body + b"\xff\xfb\x90\x00" * 8)


def main():
    if len(sys.argv) != 2:
        sys.exit("uso: make_id3_chapters.py <cartella>")
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)

    # Il caso normale: ID3v2.4, tre capitoli con il nome.
    write_mp3(os.path.join(d, "tre_capitoli.mp3"), [
        chap("ch0", 0, 60000, "Il primo capitolo", 4),
        chap("ch1", 60000, 180500, "Il secondo capitolo", 4),
        chap("ch2", 180500, 300000, "Il terzo", 4),
    ], version=4)

    # Lo stesso in ID3v2.3, dove le dimensioni NON sono sincsafe. Letto con la
    # regola sbagliata, il primo frame finisce nel mezzo del secondo.
    write_mp3(os.path.join(d, "v23.mp3"), [
        chap("ch0", 0, 10000, "Uno", 3),
        chap("ch1", 10000, 20000, "Due", 3),
    ], version=3)

    # Titoli in UTF-16 e in UTF-8, che e' come li scrive meta' dei programmi.
    write_mp3(os.path.join(d, "utf16.mp3"), [
        chap("ch0", 1500, 9000, "Capitolo accentato", 4, encoding=1),
    ], version=4)
    write_mp3(os.path.join(d, "utf8.mp3"), [
        chap("ch0", 2500, 9000, "Capitolo in utf8", 4, encoding=2),
    ], version=4)

    # Un capitolo senza nome: deve contare lo stesso.
    write_mp3(os.path.join(d, "senza_nome.mp3"), [
        chap("ch0", 0, 5000, None, 4),
    ], version=4)

    # Con l'intestazione estesa davanti, e con il riempimento di zeri dietro.
    write_mp3(os.path.join(d, "esteso.mp3"), [
        chap("ch0", 0, 1000, "Con intestazione estesa", 4),
    ], version=4, extended=True, padding=64)

    # Un MP3 con un tag ma senza capitoli: e' quello che NON deve entrare nella
    # libreria degli audiolibri.
    write_mp3(os.path.join(d, "niente_capitoli.mp3"), [tit2("Solo un brano")], version=4)

    # E uno senza tag per niente.
    with open(os.path.join(d, "nudo.mp3"), "wb") as f:
        f.write(b"\xff\xfb\x90\x00" * 64)

    # Un frame che dichiara di essere piu' grande del tag: il lettore deve
    # fermarsi, non leggere oltre la fine.
    bad = b"CHAP" + syncsafe(0x0FFFFFFF) + b"\x00\x00" + b"ch0\x00" + struct.pack(">IIII", 0, 1, 0, 0)
    write_mp3(os.path.join(d, "bugiardo.mp3"), [bad], version=4)

    for name in sorted(os.listdir(d)):
        if name.endswith(".mp3"):
            print("  %-22s %6d byte" % (name, os.path.getsize(os.path.join(d, name))))


if __name__ == "__main__":
    main()
