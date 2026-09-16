#ifndef GB_MINIZ_SHIM_H
#define GB_MINIZ_SHIM_H

// Il pezzetto di miniz che serve a Gearboy, e nient'altro.
//
// Perche' non miniz vero. Gearboy usa miniz per due cose molto diverse: il
// CRC32 con cui riconosce un paio di cartucce multi-gioco, e la lettura di ROM
// dentro un file .zip. Il primo servono venti righe; il secondo ottomila, e in
// questo player non serve a niente -- le ROM arrivano da un buffer in memoria.
//
// E miniz intero non si puo' nemmeno mettere accanto a quello che il player ha
// gia': src/system/image/miniz e' il sottoinsieme di inflate che decodifica i
// PNG delle copertine, e i due esportano gli stessi simboli. Il linker se ne
// accorge subito ("multiple definition of tinfl_decompressor_free").
//
// Quindi: CRC32 vero, lettore di zip che risponde sempre "no". Cosi' i
// sorgenti di Gearboy restano identici a quelli a monte -- e aggiornarli
// resta una copia e non una fusione a mano.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int mz_bool;
typedef unsigned int mz_uint;
typedef unsigned long mz_ulong;

#define MZ_CRC32_INIT (0)

// Il CRC32 di zlib, quello vero: Gearboy ci riconosce le cartucce M161 e
// MultiMBC1 confrontando somme note, quindi qui una scorciatoia darebbe il
// mapper sbagliato su quelle due.
mz_ulong mz_crc32(mz_ulong crc, const unsigned char *ptr, size_t buf_len);

// --- il lettore di zip, che qui non legge niente --------------------------

typedef struct {
	void *unused;
} mz_zip_archive;

typedef struct {
	mz_uint m_file_index;
	unsigned long long m_comp_size;
	unsigned long long m_uncomp_size;
	char m_filename[260];
	char m_comment[256];
} mz_zip_archive_file_stat;

mz_bool mz_zip_reader_init_mem(mz_zip_archive *zip, const void *mem, size_t size, mz_uint flags);
mz_uint mz_zip_reader_get_num_files(mz_zip_archive *zip);
mz_bool mz_zip_reader_file_stat(mz_zip_archive *zip, mz_uint index, mz_zip_archive_file_stat *stat);
void *mz_zip_reader_extract_file_to_heap(mz_zip_archive *zip, const char *name, size_t *size, mz_uint flags);
mz_bool mz_zip_reader_end(mz_zip_archive *zip);

#ifdef __cplusplus
}
#endif

#endif /* GB_MINIZ_SHIM_H */
