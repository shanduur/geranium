#ifndef GERANIUM_IMAGE_H
#define GERANIUM_IMAGE_H
#define IMAGE_ARGS 64
#define IMAGE_BYTES 8192
struct image_config {
	char data[IMAGE_BYTES];
	char *args[IMAGE_ARGS + 1];
	char envdata[IMAGE_BYTES];
	char *env[IMAGE_ARGS + 1];
	char workdir[4096];
	unsigned char *source;
	unsigned long size;
};
int image_pull(int state, const char *reference, int local_http);
int image_remove(int state, const char *reference);
int image_open(int state, const char *reference, int *lock,
	       struct image_config *config);
void image_config_save(int container, const struct image_config *config);
int image_config_load(int container, struct image_config *config);
#endif
