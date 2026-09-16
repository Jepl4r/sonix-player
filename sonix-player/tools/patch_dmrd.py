#!/usr/bin/env python3
"""Allarga la lista dei formati che il renderer DLNA di HiBy dichiara.

    tools/patch_dmrd.py /percorso/dmrd [-o dmrd.patched]

PERCHE' SERVE UNO STRUMENTO INVECE DI UNA RIGA DI CODICE

Il renderer DLNA non e' nostro. Quando si accende il DLNA, sys_server avvia
/usr/bin/dmrd -- un binario chiuso di HiBy (un gmrender-resurrect rimarchiato)
-- e da li' in poi tutto quello che il telefono legge lo scrive lui: la
descrizione del dispositivo, le tre descrizioni di servizio, e soprattutto la
risposta a GetProtocolInfo. Il nostro dlna.c e' solo il ponte che riceve
"set_uri:<indirizzo>" e mette il brano in riproduzione: non ha modo di cambiare
una sola parola di quello che il telefono vede.

E quella risposta e' il problema. La lista dei formati accettati sta scritta
dentro dmrd come una stringa sola:

    http-get:*:audio/mpeg:*,http-get:*:audio/wav:*,http-get:*:audio/flac:*,
    http-get:*:audio/ape:*,http-get:*:audio/aac:*

Manca audio/mp4 e audio/x-m4a (cioe' l'AAC come lo manda qualunque telefono),
manca audio/x-flac, manca audio/ogg, manca video/mp4, e manca il jolly. Un
punto di controllo che filtra su questa lista conclude che il lettore non sa
suonare quasi niente -- ed e' esattamente l'avviso che compare sul telefono.

Dentro il binario c'e' anche il meccanismo per costruire quella lista a runtime
(register_mime_type), ma non lo chiama nessuno: e' codice morto. Non c'e' un
appiglio, quindi si cambia la stringa.

COSA FA QUESTO STRUMENTO

Riscrive quella stringa, e nient'altro. Un solo byte range, nessuna
istruzione toccata, nessuna rilocazione da sistemare: e' una stringa in
.rodata raggiunta da un puntatore assoluto (0x00408268, che sta a 0x840c), il
che vuol dire che NON si puo' spostare e che la nuova deve stare nello spazio
della vecchia. Fra la fine della stringa e quella successiva ci sono 120 byte,
quindi il tetto e' 119 caratteri piu' il terminatore.

La lista nuova tiene le voci esplicite piu' usate e aggiunge in coda il jolly
http-get:*:*:*, che e' quello che davvero toglie il filtro: da li' in poi il
telefono manda quello che vuole e a decidere se sa suonarlo e' il lettore, che
e' il posto giusto per decidere.

video/mp4 c'e' apposta: un mp4 spinto dal telefono ha una traccia audio come
tutte le altre, e mp4.c pesca quella ignorando le immagini. E' il caso "il
film sul telefono, il suono sul lettore".

CONTROLLI

Rifiuta di scrivere se non trova la stringa originale esatta (un firmware
diverso, o un binario gia' corretto), e se la nuova non ci sta. Lo spazio che
avanza si riempie di zeri, cosi' il binario resta lungo uguale.
"""
import argparse
import os
import sys

# La stringa com'e' nel binario originale. Se non c'e', non e' questo binario.
ORIGINAL = (
	b"http-get:*:audio/mpeg:*,"
	b"http-get:*:audio/wav:*,"
	b"http-get:*:audio/flac:*,"
	b"http-get:*:audio/ape:*,"
	b"http-get:*:audio/aac:*"
)

# La nuova. Il jolly in coda e' la parte che conta; le voci esplicite prima
# servono ai punti di controllo che mostrano all'utente cosa il renderer
# accetta, e che con il solo jolly non scriverebbero niente.
REPLACEMENT = (
	b"http-get:*:audio/mpeg:*,"
	b"http-get:*:audio/mp4:*,"
	b"http-get:*:audio/flac:*,"
	b"http-get:*:video/mp4:*,"
	b"http-get:*:*:*"
)


def main():
	ap = argparse.ArgumentParser(description="Allarga i formati dichiarati da dmrd.")
	ap.add_argument("binary", help="il dmrd da correggere")
	ap.add_argument("-o", "--output", help="dove scriverlo (predefinito: accanto, con .patched)")
	args = ap.parse_args()

	data = bytearray(open(args.binary, "rb").read())

	found = data.find(ORIGINAL)
	if found < 0:
		if data.find(REPLACEMENT) >= 0:
			print("Questo dmrd e' gia' corretto: non c'e' niente da fare.")
			return 0
		print("Non trovo la lista dei formati dentro %s." % args.binary, file=sys.stderr)
		print("Non e' il dmrd che questo strumento sa correggere.", file=sys.stderr)
		return 1
	if data.count(ORIGINAL) != 1:
		print("La lista compare piu' di una volta: mi fermo.", file=sys.stderr)
		return 1

	# Quanto spazio c'e' davvero: fino al primo byte non nullo dopo il
	# terminatore. Oltre quello c'e' un'altra stringa e non si tocca.
	end = data.index(b"\x00", found)
	slack_end = end
	while data[slack_end] == 0:
		slack_end += 1
	room = slack_end - found

	print("Trovata a 0x%x, %d caratteri, %d byte di spazio in tutto." % (found, end - found, room))
	if len(REPLACEMENT) + 1 > room:
		print("La lista nuova e' di %d caratteri e non ci sta." % len(REPLACEMENT), file=sys.stderr)
		return 1

	data[found:slack_end] = REPLACEMENT + b"\x00" * (room - len(REPLACEMENT))

	out = args.output or (args.binary + ".patched")
	with open(out, "wb") as f:
		f.write(data)
	os.chmod(out, 0o755)

	print("Scritto %s (%d caratteri, %d byte liberi dopo)." % (out, len(REPLACEMENT), room - len(REPLACEMENT) - 1))
	print("Nuova lista: %s" % REPLACEMENT.decode())
	return 0


if __name__ == "__main__":
	sys.exit(main())
