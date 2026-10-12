#define _POSIX_C_SOURCE 200809L
#include "layer.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <openssl/evp.h>
#include <zlib.h>
#define EXPAND_LIMIT (1024ULL * 1024 * 1024)
static void fail(const char *s)
{
	fprintf(stderr, "geranium layer: %s\n", s);
	exit(1);
}
static void check(int n, const char *s)
{
	if (n < 0) {
		perror(s);
		exit(1);
	}
}
static int directory(int fd, const char *s)
{
	int n = openat(fd, s, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	check(n, "layer directory (symlinks forbidden)");
	return n;
}
static void write_all(int fd, const void *data, size_t size)
{
	const char *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			fail("layer write");
		}
		p += n;
		size -= (size_t)n;
	}
}
void image_delete_tree(int fd)
{
	check(fchmod(fd, 0700), "cleanup permissions");
	DIR *d = fdopendir(directory(fd, "."));
	if (!d)
		fail("cleanup enumeration");
	struct dirent *e;
	for (;;) {
		errno = 0;
		e = readdir(d);
		if (!e) {
			if (errno)
				fail("cleanup read");
			break;
		}
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		struct stat st;
		check(fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW),
		      "cleanup stat");
		if (S_ISDIR(st.st_mode)) {
			check(fchmodat(fd, e->d_name, 0700, 0),
			      "cleanup chmod");
			int sub = directory(fd, e->d_name);
			image_delete_tree(sub);
			close(sub);
			check(unlinkat(fd, e->d_name, AT_REMOVEDIR),
			      "cleanup directory");
		} else
			check(unlinkat(fd, e->d_name, 0), "cleanup entry");
	}
	check(closedir(d), "cleanup close");
}
static void erase(int fd, const char *s)
{
	struct stat st;
	if (fstatat(fd, s, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		if (errno == ENOENT)
			return;
		fail("whiteout stat");
	}
	if (S_ISDIR(st.st_mode)) {
		check(fchmodat(fd, s, 0700, 0), "whiteout chmod");
		int sub = directory(fd, s);
		image_delete_tree(sub);
		close(sub);
		check(unlinkat(fd, s, AT_REMOVEDIR), "whiteout rmdir");
	} else
		check(unlinkat(fd, s, 0), "whiteout unlink");
}
static unsigned long long octal(const unsigned char *s, size_t n)
{
	unsigned long long v = 0;
	size_t i = 0;
	while (i < n && s[i] == ' ')
		i++;
	for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
		if (v > EXPAND_LIMIT)
			fail("excessive tar number");
		v = v * 8 + (s[i] - '0');
	}
	for (; i < n; i++)
		if (s[i] && s[i] != ' ')
			fail("unsupported tar numeric encoding");
	return v;
}
static void field(char *out, size_t cap, const unsigned char *s, size_t n)
{
	size_t len = 0;
	while (len < n && s[len])
		len++;
	if (len >= cap)
		fail("tar path too long");
	memcpy(out, s, len);
	out[len] = 0;
	for (size_t i = len; i < n; i++)
		if (s[i])
			fail("invalid tar string padding");
}
/* Normalize only conventional leading ./ and trailing /. Never normalize .. . */
static void path(char *s, int directory_entry)
{
	if (!*s || s[0] == '/')
		fail("unsafe tar path");
	while (!strncmp(s, "./", 2))
		memmove(s, s + 2, strlen(s + 2) + 1);
	size_t n = strlen(s);
	while (n && s[n - 1] == '/')
		s[--n] = 0;
	if (directory_entry && (!strcmp(s, ".") || !*s)) {
		*s = 0;
		return;
	}
	if (!*s || s[0] == '/' || n > 4095)
		fail("unsafe tar path");
	unsigned depth = 0;
	for (char *p = s; *p;) {
		char *end = strchr(p, '/');
		size_t k = end ? (size_t)(end - p) : strlen(p);
		if (!k || (k == 1 && p[0] == '.') ||
		    (k == 2 && !memcmp(p, "..", 2)) || k > 255 || ++depth > 32)
			fail("unsafe tar path component");
		for (size_t i = 0; i < k; i++)
			if ((unsigned char)p[i] < 32)
				fail("unsafe tar path control byte");
		p = end ? end + 1 : p + k;
	}
}
struct parents {
	int fd[33];
	mode_t mode[33];
	unsigned count;
};
static int parent(int root, char *s, char **leaf, struct parents *chain)
{
	memset(chain, 0, sizeof *chain);
	int fd = dup(root);
	check(fd, "dup layer root");
	char *p = s;
	struct stat st;
	check(fstat(fd, &st), "layer parent stat");
	chain->fd[0] = fd;
	chain->mode[0] = st.st_mode & 07777;
	chain->count = 1;
	check(fchmod(fd, (st.st_mode & 07777) | 0700),
	      "temporary layer parent mode");
	for (;;) {
		char *slash = strchr(p, '/');
		if (!slash) {
			*leaf = p;
			return fd;
		}
		*slash = 0;
		if (mkdirat(fd, p, 0700) < 0 && errno != EEXIST)
			check(-1, "layer mkdir parent");
		check(fstatat(fd, p, &st, AT_SYMLINK_NOFOLLOW),
		      "layer ancestor stat");
		if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
		    st.st_gid != getegid())
			fail("unsafe layer ancestor (symlinks forbidden)");
		check(fchmodat(fd, p, (st.st_mode & 07777) | 0700, 0),
		      "temporary layer ancestor mode");
		fd = directory(fd, p);
		if (chain->count >= 33)
			fail("layer ancestor depth");
		chain->fd[chain->count] = fd;
		chain->mode[chain->count++] = st.st_mode & 07777;
		p = slash + 1;
	}
}
static void parent_close(struct parents *chain)
{
	while (chain->count) {
		unsigned i = --chain->count;
		check(fchmod(chain->fd[i], chain->mode[i]),
		      "restore layer directory mode");
		check(close(chain->fd[i]), "close layer ancestor");
	}
}
static unsigned root_mode = 0755;
unsigned layer_root_mode(void)
{
	return root_mode;
}
struct dirmode {
	char *path;
	mode_t mode;
	struct dirmode *next;
};
struct seen_path {
	char *path;
	struct seen_path *next;
};
static void extract(int root, int tar, int whiteouts)
{
	check((int)lseek(tar, 0, SEEK_SET), "rewind layer");
	unsigned entries = 0, zeros = 0;
	struct dirmode *modes = NULL;
	struct seen_path *seen[4096] = { 0 };
	size_t path_bytes = 0;
	for (;;) {
		unsigned char h[512];
		ssize_t n = read(tar, h, sizeof h);
		if (n == 0) {
			if (zeros < 2)
				fail("missing tar terminator");
			break;
		}
		if (n != 512)
			fail("truncated tar header");
		unsigned sum = 0;
		for (unsigned i = 0; i < 512; i++)
			sum += h[i];
		if (!sum) {
			zeros++;
			continue;
		}
		if (zeros)
			fail("nonzero tar data after terminator");
		if (++entries > 100000)
			fail("too many layer entries");
		unsigned actual = 0;
		for (unsigned i = 0; i < 512; i++)
			actual += (i >= 148 && i < 156) ? ' ' : h[i];
		if (octal(h + 148, 8) != actual)
			fail("tar checksum mismatch");
		if (memcmp(h + 257, "ustar", 5))
			fail("only ustar supported");
		unsigned long long size = octal(h + 124, 12),
				   uid = octal(h + 108, 8),
				   gid = octal(h + 116, 8),
				   mode = octal(h + 100, 8);
		char type = (char)h[156];
		if (!type)
			type = '0';
		if (type != '0' && type != '1' && type != '2' && type != '5')
			fail("unsupported tar entry (extensions or devices)");
		if (uid || gid)
			fail("non-root image ownership unsupported");
		if (mode > 07777 || (mode & 06000) ||
		    ((mode & 01000) && type != '5'))
			fail("unsupported image special mode");
		if (size > 256ULL * 1024 * 1024 || (type != '0' && size))
			fail("invalid or excessive tar entry size");
		char name[4096], prefix[156], link[101];
		field(name, sizeof name, h, 100);
		field(prefix, sizeof prefix, h + 345, 155);
		field(link, sizeof link, h + 157, 100);
		if (*prefix) {
			char joined[4096];
			int k = snprintf(joined, sizeof joined, "%s/%s", prefix,
					 name);
			if (k < 0 || (size_t)k >= sizeof joined)
				fail("tar path too long");
			strcpy(name, joined);
		}
		path(name, type == '5');
		unsigned bucket = 2166136261U;
		for (const unsigned char *p = (const unsigned char *)name; *p;
		     p++)
			bucket = (bucket ^ *p) * 16777619U;
		bucket %= 4096;
		for (struct seen_path *p = seen[bucket]; p; p = p->next)
			if (!strcmp(p->path, name))
				fail("duplicate layer path");
		path_bytes += strlen(name) + 1 + sizeof(struct seen_path);
		if (path_bytes > 16 * 1024 * 1024)
			fail("layer path metadata exceeds 16 MiB");
		struct seen_path *record = malloc(sizeof *record);
		if (!record)
			fail("allocation");
		record->path = strdup(name);
		if (!record->path)
			fail("allocation");
		record->next = seen[bucket];
		seen[bucket] = record;
		off_t data = lseek(tar, 0, SEEK_CUR);
		if (data < 0)
			fail("tar seek");
		struct stat tst;
		check(fstat(tar, &tst), "tar stat");
		unsigned long long padded = (size + 511) / 512 * 512;
		if ((unsigned long long)data + padded >
		    (unsigned long long)tst.st_size)
			fail("truncated tar payload");
		if (!*name) {
			if (type != '5')
				fail("invalid root entry");
			if (!whiteouts)
				root_mode = (unsigned)mode;
		} else {
			char *last = strrchr(name, '/');
			last = last ? last + 1 : name;
			int wh = !strncmp(last, ".wh.", 4);
			if (wh && (type != '0' || size))
				fail("invalid whiteout");
			if (wh == whiteouts) {
				char full[4096];
				strcpy(full, name);
				char *leaf;
				struct parents chain;
				int fd = parent(root, name, &leaf, &chain);
				if (wh) {
					if (!strcmp(leaf, ".wh..wh..opq"))
						image_delete_tree(fd);
					else {
						char *target = leaf + 4;
						if (!*target ||
						    !strcmp(target, ".") ||
						    !strcmp(target, ".."))
							fail("unsafe whiteout");
						erase(fd, target);
					}
				} else if (type == '5') {
					struct stat st;
					if (fstatat(fd, leaf, &st,
						    AT_SYMLINK_NOFOLLOW) == 0 &&
					    !S_ISDIR(st.st_mode))
						erase(fd, leaf);
					if (mkdirat(fd, leaf, 0700) < 0 &&
					    errno != EEXIST)
						check(-1, "layer directory");
					check(fstatat(fd, leaf, &st,
						      AT_SYMLINK_NOFOLLOW),
					      "layer directory stat");
					if (!S_ISDIR(st.st_mode) ||
					    st.st_uid != geteuid() ||
					    st.st_gid != getegid())
						fail("unsafe layer directory (symlinks forbidden)");
					/* As for ancestors, grant owner access before O_RDONLY. */
					check(fchmodat(fd, leaf,
						       (st.st_mode & 07777) |
							       0700,
						       0),
					      "temporary layer directory mode");
					int sub = directory(fd, leaf);
					check(fchmod(sub, 0700),
					      "temporary directory permissions");
					close(sub);
					struct dirmode *m = malloc(sizeof *m);
					if (!m)
						fail("allocation");
					m->path = strdup(full);
					if (!m->path)
						fail("allocation");
					m->mode = (mode_t)mode;
					m->next = modes;
					modes = m;
				} else if (type == '1') {
					char target[4096];
					strcpy(target, link);
					path(target, 0);
					char *targetleaf;
					struct parents targetchain;
					int targetfd = parent(root, target,
							      &targetleaf,
							      &targetchain);
					struct stat st;
					check(fstatat(targetfd, targetleaf, &st,
						      AT_SYMLINK_NOFOLLOW),
					      "hardlink target");
					if (!S_ISREG(st.st_mode) ||
					    st.st_uid != geteuid() ||
					    st.st_gid != getegid() ||
					    (st.st_mode & 07777) !=
						    (mode_t)mode)
						fail("unsupported hardlink target identity or mode");
					struct stat current;
					if (fstatat(fd, leaf, &current,
						    AT_SYMLINK_NOFOLLOW) == 0 &&
					    current.st_dev == st.st_dev &&
					    current.st_ino ==
						    st.st_ino) { /* already the desired link */
					} else {
						erase(fd, leaf);
						check(linkat(targetfd,
							     targetleaf, fd,
							     leaf, 0),
						      "layer hardlink");
					}
					parent_close(&targetchain);
				} else if (type == '2') {
					if (!*link)
						fail("empty symlink");
					erase(fd, leaf);
					check(symlinkat(link, fd, leaf),
					      "layer symlink");
				} else {
					erase(fd, leaf);
					int out = openat(
						fd, leaf,
						O_WRONLY | O_CREAT | O_EXCL |
							O_NOFOLLOW | O_CLOEXEC,
						0600);
					check(out, "layer file");
					unsigned char buf[32768];
					unsigned long long left = size;
					while (left) {
						size_t want =
							left > sizeof buf ?
								sizeof buf :
								(size_t)left;
						ssize_t got =
							read(tar, buf, want);
						if (got <= 0)
							fail("truncated tar file");
						write_all(out, buf,
							  (size_t)got);
						left -= (unsigned long long)got;
					}
					check(fchmod(out, (mode_t)mode),
					      "layer file mode");
					check(close(out), "layer close");
				}
				parent_close(&chain);
			}
		}
		if (lseek(tar, data + (off_t)padded, SEEK_SET) < 0)
			fail("tar seek payload");
	}
	for (unsigned i = 0; i < 4096; i++)
		while (seen[i]) {
			struct seen_path *p = seen[i];
			seen[i] = p->next;
			free(p->path);
			free(p);
		}
	/* Permissions follow extraction, so restrictive directories cannot block it. */
	for (struct dirmode *m = modes; m;) {
		char *leaf;
		struct parents chain;
		int fd = parent(root, m->path, &leaf, &chain);
		int sub = directory(fd, leaf);
		check(fchmod(sub, m->mode), "layer final directory mode");
		close(sub);
		parent_close(&chain);
		struct dirmode *next = m->next;
		free(m->path);
		free(m);
		m = next;
	}
}
void layer_apply(int root, int stage, const void *bytes, size_t size, int gzip,
		 const char *diff, unsigned long long *expanded)
{
	int tar = openat(stage, "layer.tar",
			 O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			 0600);
	check(tar, "stage layer");
	EVP_MD_CTX *md = EVP_MD_CTX_new();
	if (!md || !EVP_DigestInit_ex(md, EVP_sha256(), NULL))
		fail("digest initialization");
	if (gzip) {
		z_stream z;
		memset(&z, 0, sizeof z);
		if (inflateInit2(&z, 15 + 16) != Z_OK)
			fail("gzip initialization");
		z.next_in = (Bytef *)bytes;
		z.avail_in = (uInt)size;
		int status;
		do {
			unsigned char out[32768];
			z.next_out = out;
			z.avail_out = sizeof out;
			status = inflate(&z, Z_NO_FLUSH);
			if (status != Z_OK && status != Z_STREAM_END)
				fail("corrupt or truncated gzip");
			size_t n = sizeof out - z.avail_out;
			*expanded += n;
			if (*expanded > EXPAND_LIMIT)
				fail("expanded image exceeds 1 GiB");
			write_all(tar, out, n);
			if (!EVP_DigestUpdate(md, out, n))
				fail("digest update");
			if (!n && !z.avail_in && status != Z_STREAM_END)
				fail("truncated gzip");
		} while (status != Z_STREAM_END);
		if (z.avail_in)
			fail("gzip trailing data or concatenation unsupported");
		inflateEnd(&z);
	} else {
		*expanded += size;
		if (*expanded > EXPAND_LIMIT)
			fail("expanded image exceeds 1 GiB");
		write_all(tar, bytes, size);
		if (!EVP_DigestUpdate(md, bytes, size))
			fail("digest update");
	}
	unsigned char d[32];
	unsigned n = 0;
	char text[72];
	strcpy(text, "sha256:");
	if (!EVP_DigestFinal_ex(md, d, &n) || n != 32)
		fail("digest final");
	EVP_MD_CTX_free(md);
	for (unsigned i = 0; i < 32; i++)
		(void)sprintf(text + 7 + 2 * i, "%02x", d[i]);
	if (strcmp(text, diff))
		fail("layer DiffID mismatch");
	extract(root, tar, 1);
	extract(root, tar, 0);
	close(tar);
	check(unlinkat(stage, "layer.tar", 0), "remove staged layer");
}
