# src/gb — il core Game Boy

L'emulatore, sotto Altro → Gearboy. Il core sta qui; quello che lo fa girare sta
altrove:

| file | cosa |
|---|---|
| `src/system/gearboy.c` | il thread che emula: fotogrammi, audio, salvataggi |
| `src/system/gbinput.c` | il vetro letto in multitouch, mentre si gioca |
| `src/system/gbdb.c` | quali ROM ci sono e come si chiamano davvero |
| `src/gui/gearboypage.c` | la lista dei giochi |
| `src/gui/gearboyplay.c` | lo schermo di gioco e i comandi |

**Ci sta dentro.** Misurato sul dispositivo con una ROM di carico: 7,05 ms di
emulazione piu' 1,04 di ingrandimento contro un budget di 16,74, cioe' il
**206% del tempo reale** -- meta' CPU libera. Con una traccia 24/88,2 che suona
sopra scende al 170%, e continua a starci.

## Cosa c'è dentro

| cartella | cos'è | licenza |
|---|---|---|
| `core/` | [Gearboy](https://github.com/drhelius/Gearboy) di Ignacio Sanchez, `src/` così com'è | GPL-3.0-or-later |
| `core/audio/` | il `Gb_Snd_Emu` di Blargg che Gearboy si porta dietro | LGPL-2.1-or-later |
| `miniz/` | **non** miniz: un guscio nostro, vedere sotto | pubblico dominio (l'originale) |
| `gbcore.h` / `gbcore.cpp` | la porta fra il player (C) e il core (C++) | come il resto del progetto |

Preso da `drhelius/Gearboy`, commit `542f60b7`.

## Perché Gearboy e non Gambatte

Gambatte è tecnicamente il migliore per questo hardware — mappa di memoria a
puntatori diretti invece di una chiamata virtuale per accesso, ed è il core GB
predefinito su tutti i portatili di questa classe. Ma è **GPL versione 2
soltanto**: i suoi file dicono "under the terms of the GNU General Public
License version 2 as published", senza "or later". Il player è GPLv3, e le due
non si combinano: non si può distribuire il risultato.

Gearboy è GPL-3.0-**or-later**, quindi si combina senza fare niente. Sul core
originale non è stata cambiata una riga.

Le altre due strade, per memoria:

* **SameBoy** (MIT) è stato provato per primo ed è risultato troppo pesante —
  65% del tempo reale sul dispositivo. Emula a granularità di T-cycle, 4,19
  milioni di passi per secondo emulato: è il più accurato che esista, ed è
  accurato proprio per quello.
* **Peanut-GB** (MIT) è un header solo e sarebbe entrato nel Makefile senza
  toccare niente, ma l'accuratezza è quella pensata per i microcontrollori.
  Come misura di "regge il core?" avrebbe dato un numero ottimista che non
  rappresenta quello che poi si spedisce.

## Il guscio di miniz

Gearboy usa miniz per due cose molto diverse: il CRC32 con cui riconosce un
paio di cartucce multi-gioco (M161, MultiMBC1) e la lettura di ROM dentro un
`.zip`. Il primo sono venti righe; il secondo ottomila, e qui non serve a
niente — le ROM arrivano da un buffer in memoria.

E miniz intero non si potrebbe nemmeno mettere accanto a quello che il player
ha già: `src/system/image/miniz` è il sottoinsieme di inflate che decodifica i
PNG delle copertine, e i due esportano gli stessi simboli. Il linker se ne
accorge subito (`multiple definition of tinfl_decompressor_free`).

Quindi `miniz/` qui è un file di sessanta righe: CRC32 vero, lettore di zip che
risponde sempre "non è uno zip". Così i sorgenti di Gearboy restano identici a
quelli a monte.

## Aggiornare Gearboy

È una copia, non una fusione — è tutto il punto di non aver toccato niente:

```sh
git clone --depth 1 https://github.com/drhelius/Gearboy /tmp/gearboy
cp /tmp/gearboy/src/*.cpp /tmp/gearboy/src/*.h        src/gb/core/
cp /tmp/gearboy/src/audio/*.cpp /tmp/gearboy/src/audio/*.h  src/gb/core/audio/
cp /tmp/gearboy/LICENSE                                src/gb/core/LICENSE
```

Se a monte compare un nuovo simbolo di miniz, il linker lo dirà e va aggiunto al
guscio.

## Come viene compilato

Con il resto, sempre: `make target` (o `make host`). L'emulatore e' una
funzione del lettore e non c'e' piu' un interruttore per toglierlo -- c'e'
stato, ed e' servito finche' non e' stato chiaro che il prezzo qui sotto si
poteva pagare.

Quel prezzo vale comunque la pena di conoscerlo. `<iostream>` sta dentro `definitions.h` di Gearboy,
che e' incluso da tutto: la sola presenza di quell'include -- non l'uso --
costruisce `cout`/`cerr`/`cin` all'avvio in ogni unita' di compilazione e si
tira dietro l'intera macchina di locale. Misurato con `mipsel-g++ -O2
-fno-exceptions -fno-rtti -static-libstdc++`:

| binario | testo |
|---|---|
| `main(){}` in C++, senza iostream | 1.242 byte |
| `main(){}` piu' un solo `std::cout` | 1.147.072 byte |
| Gearboy intero piu' `gbcore.cpp` | 1.516.288 byte |

Dei ~1,5 MB che il binario prende in piu', 1,15 sono iostream e nessuno serve.
E su questo dispositivo quel milione e mezzo non e' spazio su scheda: `main()`
fa `mlockall(MCL_CURRENT)`, che inchioda in RAM ogni pagina gia' mappata, su 32
MB totali. Il player si mette anche `oom_score_adj` a -500, quindi quando la
memoria finisce il kernel ammazza qualcun altro -- e se quel qualcun altro e' un
servizio di sistema il dispositivo riparte, senza una riga nel log del player.
Adesso l'emulatore e' una funzione del lettore e quel prezzo si paga. Il
giorno che la memoria stringe, la prima leva e' togliere `<iostream>` da
`definitions.h` e `<fstream>` da `common.h`: vale un megabyte e tiene
l'emulatore.

### La trappola che ci ha fatto bootloopare

`common.h` di Gearboy corregge la gamma dei colori con `powf()`. Sembra niente,
ed e' stata la riga che ha fatto entrare il dispositivo in bootloop.

glibc 2.27 ha rifatto `powf` e le ha dato un nome di versione nuovo. Il
toolchain compila contro 2.27, quindi chiede `powf@GLIBC_2.27`; il dispositivo
ha glibc 2.22 e quel nome non ce l'ha. Il link riesce senza una parola, e poi:

```
/lib/libm.so.6: version `GLIBC_2.27' not found (required by ...)
```

Il programma non parte, `hiby_player.sh` fa `sleep 1; reboot`, e il sintomo e'
un bootloop senza una riga di log da nessuna parte -- il player non arriva
nemmeno a `main()`, quindi il suo gestore dei crash non e' ancora installato.

Adesso ci sono due reti: `src/system/glibc_compat.h`, incluso a forza in ogni
unita' del target, che richiede per nome le versioni vecchie delle cinque
funzioni che glibc 2.27 ha toccato; e `make check-abi`, che gira da solo dopo
ogni link del target e cancella il binario se chiede qualcosa che il
dispositivo non ha. Cioe' d'ora in poi lo stesso sbaglio e' un errore di
compilazione e non un dispositivo da rianimare.

### La seconda leva

Prima di spegnere l'emulatore per fare posto, c'e' un megabyte da prendere
tenendolo: togliere `<iostream>` da `definitions.h`. E' la voce grossa, non
Gearboy -- e nessuno dei due file lo usa davvero, ci sono solo per una
Log() che qui e' gia' zitta.

Il resto delle flag, dal `Makefile`, che per il C++ ha una lista e delle regole
sue (`%.opp`):

```make
GB_CFLAGS = -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti \
            -Isrc/gb/core -Isrc/gb/miniz
```

* `-DGEARBOY_DISABLE_DISASSEMBLER` toglie il disassemblatore e i punti di
  interruzione, che sono per il debugger di Gearboy.
* Le `-I` servono perché i sorgenti di Gearboy si includono fra loro per nome
  corto (`"Video.h"`), come se fossero nella loro cartella — che è come stanno a
  monte.
* Esiste anche `-DPERFORMANCE`, che è di Gearboy: fa girare la CPU 75 cicli
  macchina prima di aggiornare PPU, APU e timer invece di uno alla volta. Vale
  circa un 1,5x, al prezzo degli effetti a metà scanline. Non è acceso: prima si
  guarda se serve.

Sul target il collegamento passa da `-static-libstdc++ -static-libgcc`, e non è
opzionale: il dispositivo ha `libstdc++.so.6.0.21` (`GLIBCXX_3.4.21`, l'ABI di
GCC 5) mentre il toolchain è GCC 9, che produce riferimenti fino a `3.4.26`.
Collegato dinamicamente il binario non parte proprio.

## Le tre cose che mancavano, e come sono finite

1. **La latenza dell'audio.** `audio_external_begin()` apriva ALSA con 8
   periodi da 4096 frame, ~750 ms: invisibile per la radio, morte per un gioco.
   Adesso c'e' `audio_external_begin_latency()`, e l'emulatore chiede 46 ms.
   Che e' anche il **metronomo**: scrivendo i 738 campioni di un fotogramma su
   un PCM bloccante e' ALSA a tenere il passo, quindi il ritmo lo da' il quarzo
   del DAC e video e suono non possono scollarsi -- erano due problemi e si
   risolvono con la stessa riga.
2. **I comandi.** L'indev di LVGL e' un puntatore: un dito. Un Game Boy si
   tiene con due pollici. Quindi mentre si gioca il vetro cambia padrone --
   `panel_touch_enable(false)` e `src/system/gbinput.c` legge il nodo evdev per
   conto suo, in protocollo multitouch (quello che il driver corretto sa fare
   da quando gt9xx_touch.ko e' stato sistemato).
3. **Il ritmo.** Risolto dal punto 1. Il pannello va a 60 e il gioco a 59,73:
   si segue l'audio, e i fotogrammi che cadono fra due vsync non si vedono.

Quello che ancora non c'e': gli stati salvati (la RAM tamponata si', il `.sav`
sta accanto alla ROM), il riavvolgimento, i tasti fisici e la scelta della
tavolozza per i giochi in bianco e nero.
