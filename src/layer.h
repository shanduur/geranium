#ifndef GERANIUM_LAYER_H
#define GERANIUM_LAYER_H

#include <stddef.h>

enum layer_compression {
	LAYER_TAR = 0,
	LAYER_GZIP = 1,
	LAYER_ZSTD = 2,
};

void layer_apply(int root, int stage, const void *, size_t,
		 enum layer_compression compression, const char *diff,
		 unsigned long long *expanded);
void image_delete_tree(int);
unsigned layer_root_mode(void);

#endif
