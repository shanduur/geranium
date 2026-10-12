#ifndef GERANIUM_LAYER_H
#define GERANIUM_LAYER_H
#include <stddef.h>
void layer_apply(int root, int stage, const void *, size_t, int gzip,
		 const char *diff, unsigned long long *expanded);
void image_delete_tree(int);
unsigned layer_root_mode(void);
#endif
