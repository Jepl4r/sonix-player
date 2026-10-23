#!/usr/bin/env python3
"""Generate synthetic EPUBs and exercise the real reader/controller. No LVGL/audio device required.
Run from sonix-player/: python3 tools/test_epub_audio.py [--fixtures DIR] [--sanitize]
CC may contain a compiler command; --fixtures retains books for the target bench.
"""
import argparse
import io
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile
import wave
import zipfile


def fixtures(root):
    root.mkdir(parents=True, exist_ok=True)
    data = io.BytesIO()
    with wave.open(data, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(random.Random(42).randbytes(2 * 1024 * 1024))
    audio = data.getvalue()
    (root / 'long.wav').write_bytes(audio)
    def book(name, body='', smil=None, method=zipfile.ZIP_DEFLATED, extra=None):
        overlay = ' media-overlay="mo"' if smil is not None else ''
        resources = '<item id="a" href="audio/a%20%26%20b.wav" media-type="audio/wav"/><item id="b" href="audio/extensionless" media-type="audio/wav"/><item id="long" href="audio/long.wav" media-type="audio/wav"/>'
        if smil is not None: resources += '<item id="mo" href="overlays/chapter.smil" media-type="application/smil+xml"/>'
        with zipfile.ZipFile(root / name, 'w', compression=method) as z:
            z.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
            z.writestr('META-INF/container.xml', '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="EPUB/package.opf"/></rootfiles></container>')
            z.writestr('EPUB/package.opf', f'<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Audio test</dc:title></metadata><manifest><item id="chapter" href="text/chapter.xhtml" media-type="application/xhtml+xml"{overlay}/>{resources}</manifest><spine><itemref idref="chapter"/></spine></package>')
            z.writestr('EPUB/text/chapter.xhtml', f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Test</title></head><body><h1>Title</h1><p>Narrated text.</p>{body}</body></html>')
            z.writestr('EPUB/audio/a & b.wav', audio[:16044])
            z.writestr('EPUB/audio/extensionless', audio[:16044])
            z.writestr('EPUB/audio/long.wav', audio)
            if smil is not None: z.writestr('EPUB/overlays/chapter.smil', f'<smil xmlns="http://www.w3.org/ns/SMIL" version="3.0"><body><seq>{smil}</seq></body></smil>')
            for path, content in (extra or {}).items(): z.writestr(path, content)
    book('plain.epub')
    book('inline.epub', '<audio controls="controls" src="../audio/a%20%26%20b.wav">Do not render fallback.</audio><audio><source src="../audio/extensionless"/>More fallback.</audio><video><audio src="../audio/long.wav"/></video>')
    def clip(begin, end=None):
        return f'<par><text src="../text/chapter.xhtml#p1"/><audio src="../audio/long.wav" clipBegin="{begin}"' + (f' clipEnd="{end}"' if end is not None else '') + '/></par>'
    book('overlay.epub', '<audio src="../audio/long.wav"/>', clip('1000ms', '3s') + clip('00:03', '00:00:05.000') + clip('npt=00:01:05.5', '2min') + clip('1h'))
    body = '<p><span id="s1">Hello <em>bright</em> world.</span> <span id="s&amp;2">Second sentence.</span></p><p id="s3">A longer sentence crossing a device page with enough words to seek inside it.</p>'
    sync = '<par><audio src="../audio/long.wav" clipBegin="1s" clipEnd="3s"/><text src="../text/chapter.xhtml#s1"/></par>'
    sync += '<par><text src="../text/chapter.xhtml#s%262"/><audio src="../audio/long.wav" clipBegin="3s" clipEnd="7s"/></par>'
    sync += '<par><text src="../text/chapter.xhtml#s3"/><audio src="../audio/long.wav" clipBegin="7s" clipEnd="17s"/></par>'
    sync += '<par><text src="../text/other.xhtml#s1"/><audio src="../audio/long.wav" clipBegin="17s" clipEnd="19s"/></par>'
    book('sync.epub', body, sync)
    dense_body = '<p>' + ''.join(f'<span id="s{i}">Sentence {i}. </span>' for i in range(4000)) + '</p>'
    dense_smil = ''.join(f'<par><text src="../text/chapter.xhtml#s{i}"/><audio src="../audio/long.wav" clipBegin="{i}s" clipEnd="{i+1}s"/></par>' for i in range(4000))
    book('dense.epub', dense_body, dense_smil)
    book('invalid.epub', smil=clip('-1s', '2s') + clip('NaN', '4s') + clip('5s', '3s') + '<audio src="https://example.com/a.mp3"/><audio src="../audio/missing.mp3"/><audio src="../audio/a%00.wav"/>')
    book('many.epub', smil=clip('0s', '1s') * 8193)
    book('stored.epub', '<audio src="../audio/long.wav"/>', method=zipfile.ZIP_STORED)
    book('deflated.epub', '<audio src="../audio/long.wav"/>')
    corrupt = bytearray((root / 'stored.epub').read_bytes())
    with zipfile.ZipFile(root / 'stored.epub') as z:
        info = z.getinfo('EPUB/audio/long.wav')
        offset = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
    corrupt[offset + 1000] ^= 0xff
    (root / 'corrupt.epub').write_bytes(corrupt)
    (root / 'truncated.epub').write_bytes(corrupt[:len(corrupt) // 2])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='sonix-epub-audio-') as temp:
        root = (args.fixtures or Path(temp)).resolve()
        fixtures(root)
        sources = ['tools/test_epub_audio.c', 'src/system/ebook/ebook.c', 'src/system/ebook/ebookaudio.c',
                   'src/system/ebook/bookmarks.c', 'src/system/core/config.c']
        sources += [str(p) for p in Path('src/system/ebook').glob('epub_*.c')]
        command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-I.']
        if args.sanitize: command += ['-fsanitize=address,undefined']
        binary = root / 'test_epub_audio'
        subprocess.run(command + sources + ['-pthread', '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(root)], check=True)

if __name__ == '__main__': main()
