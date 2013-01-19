#define FREEARRAY_MIN_LEN 128

#define FREEARRAY_TYPE(name, type) \
	typedef struct { \
		type *arr; \
		size_t arrlen; \
		void **freestack; \
		size_t freestacklen; \
		size_t allocatedlen; \
	} name

#define FREEARRAY_CREATE(fa) \
	do { \
		(fa)->arr = malloc(FREEARRAY_MIN_LEN * sizeof(*((fa)->arr))); \
		(fa)->arrlen = 0; \
		(fa)->freestack = malloc(FREEARRAY_MIN_LEN * sizeof(void *)); \
		(fa)->freestacklen = 0; \
		(fa)->allocatedlen = FREEARRAY_MIN_LEN; \
	} while (0)

#define FREEARRAY_ARR(fa) ((fa)->arr)

#define FREEARRAY_ID(fa, ptr) (((ptr) - (fa)->arr) / sizeof(typeof(*((fa)->arr))))

#define FREEARRAY_LEN(fa) ((fa)->arrlen)

#define FREEARRAY_ALLOC(fa, ptr) \
	do { \
		if ((fa)->freestacklen) { \
			(fa)->freestacklen--; \
			(ptr) = (fa)->freestack[(fa)->freestacklen]; \
		} else { \
			if ((fa)->arrlen == (fa)->allocatedlen) { \
				(fa)->allocatedlen *= 2; \
				(fa)->arr = realloc((fa)->arr, sizeof(typeof(*((fa)->arr))) * (fa)->allocatedlen); \
				(fa)->arr = realloc((fa)->arr, sizeof(void *) * (fa)->allocatedlen); \
			} \
			(ptr) = &((fa)->arr[(fa)->arrlen++]); \
		} \
	} while (0)

#define FREEARRAY_FREE(fa, ptr) \
	do { \
		(fa)->freestack[(fa)->freestacklen++] = (ptr); \
	} while (0)

#define FREEARRAY_DESTROY(fa) \
	do { \
		free((fa)->arr); \
		free((fa)->freestack); \
	} while (0)
