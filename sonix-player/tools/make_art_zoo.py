#!/usr/bin/env python3
"""Costruisce i file su cui gira run_albumart_bench.sh.

I tag li scrive questo file a mano, byte per byte, perche' il lettore deve
misurarsi contro la forma vera di ID3v2 e non contro qualcosa costruito con le
sue stesse assunzioni. Le cose che contano sono quelle che nessun tagger
normale produce e che sulla scheda di qualcuno esistono lo stesso: dimensioni
sincsafe dove dovrebbero essere normali e viceversa, unsynchronisation,
intestazioni estese, frame che dichiarano di essere piu' grandi del tag,
descrizioni senza terminatore, APIC tagliati a meta'.

I file non sono MP3 riproducibili: dopo il tag c'e' un pugno di byte qualsiasi.
Non importa, perche' quello che si prova e' il tag.

Uso: make_art_zoo.py <cartella>
"""
import os
import shutil
import struct
import sys
import zlib

# Un JPEG e un PNG veri ma minuscoli: il lettore controlla i primi byte prima
# di tenere qualcosa, quindi un payload finto verrebbe scartato e meta' delle
# prove non proverebbero niente.
JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb004300"
    + "08060607060508070707090908" + "0a" * 51
    + "ffc0000b080001000101011100ffc40014000100000000000000000000000000000000"
    + "09ffc40014100100000000000000000000000000000000ffda0008010100003f00d2cfa0ffd9"
)
PNG = (b"\x89PNG\r\n\x1a\n"
       + b"\x00\x00\x00\x0dIHDR" + struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
PNG += struct.pack(">I", zlib.crc32(b"IHDR" + PNG[16:29]) & 0xFFFFFFFF)
_idat = zlib.compress(b"\x00\x00\x00\x00")
PNG += struct.pack(">I", len(_idat)) + b"IDAT" + _idat
PNG += struct.pack(">I", zlib.crc32(b"IDAT" + _idat) & 0xFFFFFFFF)
PNG += b"\x00\x00\x00\x00IEND" + struct.pack(">I", zlib.crc32(b"IEND") & 0xFFFFFFFF)

AUDIO_TAIL = b"\xff\xfb\x90\x00" * 16  # qualcosa dopo il tag, come in un file vero


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def frame(fid, payload, version, flags=b"\x00\x00"):
    """Un frame ID3v2.3/2.4. La differenza che rompe tutto e' qui: nella 2.4 la
    dimensione e' sincsafe, nella 2.3 e' un normale big endian."""
    size = syncsafe(len(payload)) if version >= 4 else struct.pack(">I", len(payload))
    return fid + size + flags + payload


def frame22(fid, payload):
    """Un frame ID3v2.2: id di tre byte, dimensione di tre byte, niente flag."""
    return fid + struct.pack(">I", len(payload))[1:] + payload


def apic(image=JPEG, mime=b"image/jpeg", pic_type=3, description=b"", encoding=0):
    """Il corpo di un APIC: codifica, tipo MIME, tipo di immagine, descrizione,
    e i byte dell'immagine."""
    body = bytes([encoding]) + mime + b"\x00" + bytes([pic_type])
    if encoding in (1, 2):
        body += description + b"\x00\x00"
    else:
        body += description + b"\x00"
    return body + image


def pic22(image=JPEG, fmt=b"JPG", pic_type=3, description=b""):
    """Il corpo di un PIC della 2.2: al posto del tipo MIME ci sono tre
    caratteri fissi."""
    return b"\x00" + fmt + bytes([pic_type]) + description + b"\x00" + image


def unsynchronise(data):
    """Quello che fa un tagger quando il flag di unsynchronisation e' alzato:
    dopo ogni FF infila uno zero, cosi' niente nel tag somiglia all'inizio di un
    frame MPEG."""
    out = bytearray()
    for i, b in enumerate(data):
        out.append(b)
        if b == 0xFF:
            out.append(0x00)
    return bytes(out)


def tag(frames, version=3, flags=0, extended=None, padding=0):
    body = b"".join(frames)
    if extended is not None:
        body = extended + body
    body += b"\x00" * padding
    if flags & 0x80:
        body = unsynchronise(body)
    return b"ID3" + bytes([version, 0]) + bytes([flags]) + syncsafe(len(body)) + body


def write(d, name, data):
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def main():
    if len(sys.argv) != 2:
        sys.exit("uso: make_art_zoo.py <cartella>")
    root = sys.argv[1]
    shutil.rmtree(root, ignore_errors=True)

    # --- i tag: una cartella per se', senza immagini dentro, cosi' il ripiego
    # sulla cartella non puo' salvare un tag che non si legge.
    d = os.path.join(root, "tags")
    os.makedirs(d)

    # Il caso normale, nelle tre versioni.
    write(d, "v23.mp3", tag([frame(b"APIC", apic(), 3)], version=3) + AUDIO_TAIL)
    write(d, "v24.mp3", tag([frame(b"APIC", apic(), 4)], version=4) + AUDIO_TAIL)
    write(d, "v22.mp3", tag([frame22(b"PIC", pic22())], version=2) + AUDIO_TAIL)
    write(d, "png.mp3", tag([frame(b"APIC", apic(PNG, b"image/png"), 3)], version=3) + AUDIO_TAIL)

    # Una 2.4 con le dimensioni scritte NON sincsafe, che e' quello che fanno
    # parecchi tagger. Letta secondo le regole, il frame finisce nel posto
    # sbagliato e la copertina si perde: il lettore ha una regola apposta per
    # accorgersene, e questa la prova.
    payload = apic()
    bad = b"APIC" + struct.pack(">I", len(payload)) + b"\x00\x00" + payload
    tail = frame(b"TIT2", b"\x00dopo", 4)
    write(d, "v24_dimensioni_normali.mp3",
          tag([bad, tail], version=4) + AUDIO_TAIL)

    # Unsynchronisation: sull'intero tag (2.3) e sul singolo frame (2.4).
    write(d, "unsync_tag.mp3", tag([frame(b"APIC", apic(), 3)], version=3, flags=0x80) + AUDIO_TAIL)
    body = apic()
    write(d, "unsync_frame.mp3",
          tag([b"APIC" + syncsafe(len(unsynchronise(body))) + b"\x00\x02" + unsynchronise(body)],
              version=4) + AUDIO_TAIL)

    # Intestazione estesa: una buona, e una che dichiara una lunghezza assurda.
    write(d, "esteso.mp3",
          tag([frame(b"APIC", apic(), 4)], version=4, flags=0x40,
              extended=syncsafe(6) + b"\x01\x00") + AUDIO_TAIL)
    write(d, "esteso_bugiardo.mp3",
          tag([frame(b"APIC", apic(), 4)], version=4, flags=0x40,
              extended=syncsafe(0x0FFFFFFF) + b"\x01\x00") + AUDIO_TAIL)

    # Un frame che dichiara di essere piu' grande di tutto il tag.
    liar = b"APIC" + syncsafe(0x0FFFFFFF) + b"\x00\x00" + apic()[:32]
    write(d, "frame_bugiardo.mp3", tag([liar], version=4) + AUDIO_TAIL)

    # Un APIC tagliato: i byte dell'immagine finiscono a meta'.
    short = apic()[: 20 + len(JPEG) // 2]
    write(d, "apic_troncato.mp3", tag([frame(b"APIC", short, 3)], version=3) + AUDIO_TAIL)

    # Il tag dichiara piu' byte di quanti ce ne siano nel file.
    whole = tag([frame(b"APIC", apic(), 3)], version=3)
    write(d, "tag_troncato.mp3", whole[: len(whole) - len(JPEG) // 2])

    # Descrizioni che non finiscono mai, nelle quattro codifiche. Il lettore
    # deve fermarsi in fondo al frame e non un byte oltre.
    for enc, name in ((0, "latin1"), (1, "utf16bom"), (2, "utf16be"), (3, "utf8")):
        body = bytes([enc]) + b"image/jpeg\x00" + bytes([3]) + b"descrizione senza fine"
        write(d, "desc_%s.mp3" % name, tag([frame(b"APIC", body, 3)], version=3) + AUDIO_TAIL)

    # Piu' immagini: deve vincere il front cover (tipo 3) anche se arriva dopo.
    write(d, "tre_immagini.mp3",
          tag([frame(b"APIC", apic(pic_type=1, description=b"artista"), 3),
               frame(b"APIC", apic(PNG, b"image/png", pic_type=4, description=b"retro"), 3),
               frame(b"APIC", apic(pic_type=3, description=b"copertina"), 3)],
              version=3) + AUDIO_TAIL)
    # E lo stesso al contrario: il front cover per primo, niente lo sostituisce.
    write(d, "front_per_primo.mp3",
          tag([frame(b"APIC", apic(pic_type=3), 3),
               frame(b"APIC", apic(PNG, b"image/png", pic_type=4), 3)],
              version=3) + AUDIO_TAIL)

    # Un'immagine che non e' ne' JPEG ne' PNG: va scartata, non tenuta.
    write(d, "non_immagine.mp3",
          tag([frame(b"APIC", apic(b"GIF89a" + b"\x00" * 64, b"image/gif"), 3)], version=3) + AUDIO_TAIL)

    # E i casi vuoti.
    write(d, "senza_immagini.mp3", tag([frame(b"TIT2", b"\x00Solo un titolo", 3)], version=3) + AUDIO_TAIL)
    write(d, "solo_riempimento.mp3", tag([], version=3, padding=256) + AUDIO_TAIL)
    write(d, "tag_minuscolo.mp3", b"ID3\x03\x00\x00" + syncsafe(4) + b"\x00\x00\x00\x00" + AUDIO_TAIL)
    write(d, "nudo.mp3", AUDIO_TAIL)
    write(d, "vuoto.mp3", b"")

    # Le stesse cose dove le mettono gli altri contenitori: un .dsf punta al
    # tag da un campo della sua intestazione, un AIFF lo mette in un chunk.
    id3 = tag([frame(b"APIC", apic(), 3)], version=3)
    dsd_header = b"DSD " + struct.pack("<QQQ", 28, 0, 0)
    dsd = dsd_header + AUDIO_TAIL + id3
    dsd = dsd[:20] + struct.pack("<Q", len(dsd_header) + len(AUDIO_TAIL)) + dsd[28:]
    write(d, "con_tag.dsf", dsd)

    aiff_body = b"AIFF" + b"ID3 " + struct.pack(">I", len(id3)) + id3
    write(d, "con_tag.aiff", b"FORM" + struct.pack(">I", len(aiff_body)) + aiff_body)

    # --- il FLAC, dove la copertina sta in un blocco PICTURE letto da dr_flac
    def flac_picture_block(image, mime=b"image/jpeg", pic_type=3, description=b""):
        b = struct.pack(">I", pic_type)
        b += struct.pack(">I", len(mime)) + mime
        b += struct.pack(">I", len(description)) + description
        b += struct.pack(">IIII", 1, 1, 24, 0)  # larghezza, altezza, bit, colori
        b += struct.pack(">I", len(image)) + image
        return b

    def flac(blocks_last_picture=True, picture=None):
        # STREAMINFO: dr_flac non apre niente senza.
        streaminfo = struct.pack(">HH", 4096, 4096)
        streaminfo += b"\x00\x00\x00" + b"\x00\x00\x00"  # min/max frame
        # 20 bit di frequenza, 3 di canali, 5 di bit, 36 di campioni totali
        rate, channels, bits, total = 44100, 2, 16, 0
        packed = (rate << 44) | ((channels - 1) << 41) | ((bits - 1) << 36) | total
        streaminfo += packed.to_bytes(8, "big") + b"\x00" * 16
        out = b"fLaC" + b"\x00" + struct.pack(">I", len(streaminfo))[1:] + streaminfo
        if picture is not None:
            out += b"\x86" + struct.pack(">I", len(picture))[1:] + picture
        else:
            pad = b"\x00" * 16
            out += b"\x81" + struct.pack(">I", len(pad))[1:] + pad
        return out

    write(d, "con_copertina.flac", flac(picture=flac_picture_block(JPEG)))
    write(d, "copertina_png.flac", flac(picture=flac_picture_block(PNG, b"image/png")))
    write(d, "senza_copertina.flac", flac())
    # Un blocco che dichiara un'immagine piu' lunga del blocco stesso.
    bad_pic = flac_picture_block(JPEG)
    bad_pic = bad_pic[:-len(JPEG) - 4] + struct.pack(">I", 0x7FFFFFFF) + JPEG
    write(d, "copertina_bugiarda.flac", flac(picture=bad_pic))

    # --- le cartelle, per la strada di ripiego
    covers = os.path.join(root, "con_cover")
    os.makedirs(covers)
    write(covers, "01 brano.mp3", tag([frame(b"TIT2", b"\x00Niente foto", 3)], version=3) + AUDIO_TAIL)
    write(covers, "cover.jpg", JPEG)
    write(covers, "front.png", PNG)   # cover vince su front
    write(covers, "casuale.jpg", JPEG)

    solo = os.path.join(root, "una_immagine_sola")
    os.makedirs(solo)
    write(solo, "01 brano.mp3", tag([frame(b"TIT2", b"\x00Niente foto", 3)], version=3) + AUDIO_TAIL)
    write(solo, "qualsiasi_nome.jpg", JPEG)  # una sola immagine: si prende quella

    dal_brano = os.path.join(root, "solo_nei_tag")
    os.makedirs(dal_brano)
    write(dal_brano, "02 secondo.mp3", tag([frame(b"APIC", apic(PNG, b"image/png"), 3)], version=3) + AUDIO_TAIL)
    write(dal_brano, "01 primo.mp3", tag([frame(b"APIC", apic(), 3)], version=3) + AUDIO_TAIL)

    niente = os.path.join(root, "niente")
    os.makedirs(niente)
    write(niente, "01 brano.mp3", AUDIO_TAIL)
    write(niente, "lettere.txt", b"nessuna copertina qui")

    total = sum(len(files) for _, _, files in os.walk(root))
    print("  %d file in %s" % (total, root))


if __name__ == "__main__":
    main()
