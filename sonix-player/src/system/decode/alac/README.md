# ALAC (Apple Lossless) — decoder di terze parti

`alac.c` e `decomp.h` sono il decoder ALAC di **David Hammerton**, versione
0.2.0 (2009), licenza MIT — il testo completo sta in testa a `alac.c` e va
lasciato lì.

    http://crazney.net/programs/itunes/alac.html

## Perché questo e non quello di Apple

Il decoder ufficiale di Apple (`macosforge/alac`, Apache 2.0) copre anche 20 e
32 bit e il multicanale, ma il suo decoder è C++ e il dispositivo ha una
`libstdc++.so.6.0.21` (gcc 5) mentre noi compiliamo con gcc 9.5: legarlo
vorrebbe dire `-static-libstdc++` e portarsi dietro il runtime C++ per un solo
file. Questo invece è C puro, sta in un file, e copre 16 e 24 bit mono e
stereo — cioè tutta la musica ALAC che esiste davvero.

Il limite è reale e va conosciuto: **20 e 32 bit non sono implementati**
(l'originale stampava `FIXME: unimplemented sample size` e restituiva
silenzio). `alacdec.c` rifiuta quei file in apertura, con una riga di log, così
non si arriva mai a quel ramo.

## Modifiche locali

Il file è quello originale tranne due punti, entrambi marcati nel sorgente con
`MODIFICA LOCALE (sonix_player)`:

1. **`count_leading_zeros`** — aggiunto un ramo `__builtin_clz` prima di quelli
   scritti a mano. Su MIPS32r2 diventa una singola istruzione `clz` invece
   della dozzina del fallback generico, e questa funzione viene chiamata una
   volta per campione nella decodifica Rice. Toglie anche il `#warning` che il
   file emetteva su qualunque architettura non-x86.

2. **Il conteggio di campioni per frame** (due occorrenze, il ramo mono e
   quello stereo) — nell'originale `outputsamples` viene letto dal file come
   intero a 32 bit e usato senza alcun controllo. Un `.m4a` corrotto o
   costruito apposta può dichiarare due miliardi di campioni in un frame, e il
   decoder scrive oltre la fine sia dei propri buffer interni sia di quello del
   chiamante. Ora viene tagliato a `setinfo_max_samples_per_frame`, che è il
   massimo che il formato ammette.

3. **Il blocco compresso di zeri in `entropy_rice_decode()`** — `blockSize`
   arriva dal bitstream e viene passato a `memset` senza confronto con
   `outputSize`: un blocco dichiarato più lungo di quel che resta nel frame
   scrive oltre la fine del buffer di uscita. Non è teorico: rovinando qualche
   byte a caso dentro `mdat`, **5 file su 400 abortivano qui**, e valgrind
   indicava questa `memset`. Ora viene tagliato allo spazio disponibile; il
   confronto con `0xFFFF` continua a usare il valore dichiarato per non
   cambiare la logica del segno.

4. **`free_alac()`, che non esisteva** — `alac_set_info()` chiama
   `allocate_buffers()`, che prende sei blocchi da `frameLength * 4` byte, e
   nell'originale nessuno li restituisce mai. Nel programma da riga di comando
   dell'autore non si notava (apriva un file e usciva), ma in un lettore sono
   ~96 KB persi per ogni traccia aperta: con la RAM di questo dispositivo si
   arriva a fine memoria in un pomeriggio di ascolto. Trovata con valgrind.
   Insieme, `create_alac()` usa `calloc` invece di `malloc` e controlla il
   ritorno, così i sei puntatori partono `NULL` e liberare un decoder mai
   configurato è innocuo.

Se un giorno si aggiorna il file, queste quattro vanno riapplicate.

Le modifiche 2 e 3 sono state trovate facendo decodificare 448 file
volutamente rovinati (troncati e con byte sporcati dentro `mdat`) sotto
valgrind. Dopo: 448 su 448 gestiti senza un crash e senza un solo accesso di
memoria non valido.

## Come si usa

Non direttamente: `alac.c` vuole il magic cookie nella forma che gli passava il
demuxer dell'autore, e dichiara `extern int host_bigendian` lasciando la
definizione al chiamante. Entrambe le cose le sistema `../alacdec.c`, che è
l'unico file che deve includere `decomp.h`.
