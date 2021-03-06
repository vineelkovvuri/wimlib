/*
 * chunk_compressor.h
 *
 * Interface for serial/parallel chunk compression.
 */

#ifndef _WIMLIB_CHUNK_COMPRESSOR_H
#define _WIMLIB_CHUNK_COMPRESSOR_H

#include <wimlib/types.h>

/* Interface for chunk compression.  Users can submit chunks of data to be
 * compressed, then retrieve them later in order.  This interface can be
 * implemented either in serial (having the calling thread compress the chunks
 * itself) or in parallel (having other threads asynchronously compress the
 * chunks).  */
struct chunk_compressor {
	/* Variables set by the chunk compressor when it is created.  */
	int out_ctype;
	u32 out_chunk_size;
	unsigned num_threads;

	/* Free the chunk compressor.  */
	void (*destroy)(struct chunk_compressor *);

	/* Submit a chunk of uncompressed data for compression.
	 *
	 * The chunk must have size greater than 0 and less than or equal to
	 * @out_chunk_size.
	 *
	 * The return value is %true if the chunk was successfully submitted, or
	 * %false if the chunk compressor does not have space for the chunk at
	 * the present time.  In the latter case, get_chunk() must be called to
	 * retrieve a compressed chunk before trying again.  */
	bool (*submit_chunk)(struct chunk_compressor *, const void *, u32);

	/* Get the next chunk of compressed data.
	 *
	 * The compressed data, along with its size and the size of the original
	 * uncompressed chunk, are returned in the locations pointed to by
	 * arguments 2-4.  The compressed data is in storage internal to the
	 * chunk compressor, and it cannot be accessed beyond any subsequent
	 * calls to the chunk compressor.
	 *
	 * Chunks will be returned in the same order in which they were
	 * submitted for compression.
	 *
	 * The resulting compressed length may be up to the uncompressed length.
	 * In the case where they are equal, the returned data is actually the
	 * uncompressed data, not the compressed data.
	 *
	 * The return value is %true if a chunk of compressed data was
	 * successfully retrieved, or %false if there are no chunks currently
	 * being compressed.  */
	bool (*get_chunk)(struct chunk_compressor *,
			  const void **, u32 *, u32 *);
};


/* Functions that return implementations of the chunk_compressor interface.  */

#ifdef ENABLE_MULTITHREADED_COMPRESSION
int
new_parallel_chunk_compressor(int out_ctype, u32 out_chunk_size,
			      unsigned num_threads, u64 max_memory,
			      struct chunk_compressor **compressor_ret);
#endif

int
new_serial_chunk_compressor(int out_ctype, u32 out_chunk_size,
			    struct chunk_compressor **compressor_ret);

#endif /* _WIMLIB_CHUNK_COMPRESSOR_H  */
