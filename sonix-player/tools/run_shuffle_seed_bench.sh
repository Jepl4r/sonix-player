#!/bin/bash
# Fa girare tools/test_shuffle_seed.c contro la LVGL vera dell'albero.
#
# Serve `make` almeno una volta prima: linka gli oggetti gia' compilati in
# build_host/lvgl, cosi' il banco prova lo stesso lv_rand che gira nel player e
# non una copia.
#
# Uso: run_shuffle_seed_bench.sh
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1

# Gli oggetti derivati dai SORGENTI, come fa il Makefile, e non raccolti con un
# find su build_host/lvgl.
#
# La differenza si vede solo dopo un cambio di versione di LVGL: `make` non
# cancella gli oggetti dei sorgenti spariti, e un find li raccoglie insieme agli
# altri. Aggiornando dalla 9.1 alla 9.5 sono rimasti trenta .o della vecchia --
# fra cui _lv_cache_lru_rb.o, che chiama una _lv_ll_ins_head che nella 9.5 non
# esiste piu' -- e il banco non linkava piu'. Il Makefile non se ne accorge
# perche' la sua lista la ricava da `find lvgl/src`, che e' quello che si fa qui.
OBJS=""
for c in $(find lvgl/src -type f -name '*.c'); do
	o="build_host/lvgl/${c#lvgl/}"
	o="${o%.c}.o"
	[ -f "$o" ] && OBJS="$OBJS $o"
done
if [ -z "$OBJS" ]; then
	echo "build_host/lvgl e' vuota o non aggiornata: lancia prima make"
	exit 1
fi

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# Gli stessi flag del build host per LVGL: la configurazione la decide lv_conf.h
# dell'albero, e con una diversa lv_rand non sarebbe la stessa funzione.
# Le stesse librerie del build host: la LVGL dell'albero e' compilata con il
# driver SDL dentro, quindi gli oggetti se le portano dietro anche se il banco
# non apre nessuna finestra.
gcc -O1 -g -Wall -Wextra -I"$HERE" -I"$HERE/lvgl" -DLV_CONF_INCLUDE_SIMPLE=1 \
	$(pkg-config --cflags freetype2) \
	-o "$W/test_shuffle_seed" "$HERE/tools/test_shuffle_seed.c" $OBJS \
	$(sdl2-config --libs) $(pkg-config --libs freetype2) -lm -lpthread || exit 1

"$W/test_shuffle_seed"
