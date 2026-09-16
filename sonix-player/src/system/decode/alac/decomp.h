#ifndef __ALAC__DECOMP_H
#define __ALAC__DECOMP_H

typedef struct alac_file alac_file;

alac_file *create_alac(int samplesize, int numchannels);
/* MODIFICA LOCALE (sonix_player): non c'era. Senza, i sei buffer interni
 * che alac_set_info() alloca non venivano mai liberati -- vedere la nota in
 * alac.c. */
void free_alac(alac_file *alac);
void decode_frame(alac_file *alac,
                  unsigned char *inbuffer,
                  void *outbuffer, int *outputsize);
void alac_set_info(alac_file *alac, char *inputbuffer);

#endif /* __ALAC__DECOMP_H */

