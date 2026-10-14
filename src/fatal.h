#ifndef GERANIUM_FATAL_H
#define GERANIUM_FATAL_H

/* Private, process-lifetime core boundary; the public ABI is unchanged. */
void geranium_fatal_cleanup(void (*cleanup)(void));
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void geranium_fatal_exit(int status);

#endif
