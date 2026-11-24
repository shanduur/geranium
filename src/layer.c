#define _POSIX_C_SOURCE 200809L
#include "layer.h"
#include "fatal.h"
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
#include <archive.h>
#include <archive_entry.h>
#include <zstd.h>
#include <limits.h>
#include <locale.h>
#include <langinfo.h>
#define EXPAND_LIMIT (4ULL * 1024 * 1024 * 1024)
#define ENTRY_LIMIT (1024ULL * 1024 * 1024)

static void fail(const char *s)
{
	fprintf(stderr, "geranium layer: %s\n", s);
	geranium_fatal_exit(1);
}

static void check(int n, const char *s)
{
	if (n < 0) {
		perror(s);
		geranium_fatal_exit(1);
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

/* libarchive accepts EOF without two zero blocks and can read ahead on input. */
static void check_tar_end(struct archive *archive, int tar)
{
	la_int64_t end = archive_filter_bytes(archive, 0);
	la_int64_t terminator = archive_read_header_position(archive);
	struct stat st;
	unsigned char buf[32768];

	check(fstat(tar, &st), "tar size");
	if (end < 1024 || end % 512 || end > st.st_size ||
	    terminator != end - 1024)
		fail("invalid tar terminator");
	for (off_t pos = (off_t)end - 1024; pos < st.st_size;) {
		size_t want = st.st_size - pos > (off_t)sizeof buf ?
				      sizeof buf :
				      (size_t)(st.st_size - pos);
		ssize_t n = pread(tar, buf, want, pos);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			fail("read tar terminator");
		for (ssize_t i = 0; i < n; i++)
			if (buf[i])
				fail("invalid tar terminator");
		pos += n;
	}
}

static void extract(int root, int tar, int whiteouts, unsigned *warnings,
		    unsigned long long *output_total)
{
	struct archive *archive;
	struct archive_entry *entry;
	unsigned entries = 0;
	struct dirmode *modes = NULL;
	struct seen_path *seen[4096] = { 0 };
	size_t path_bytes = 0;
	int input, status;
	locale_t utf8, previous;

	/* libarchive 3.7.7 replaces unrepresentable PAX names with '*'
	 * without warning. Keep only this extraction's CTYPE UTF-8. */
	utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
	if (!utf8 || strcmp(nl_langinfo_l(CODESET, utf8), "UTF-8"))
		fail("UTF-8 locale required for tar extraction");
	previous = uselocale(utf8);
	if (!previous)
		fail("select tar extraction locale");
	check((int)lseek(tar, 0, SEEK_SET), "rewind layer");
	input = dup(tar);
	check(input, "duplicate layer input");
	archive = archive_read_new();
	if (!archive ||
	    archive_read_support_filter_none(archive) != ARCHIVE_OK ||
	    archive_read_support_format_tar(archive) != ARCHIVE_OK ||
	    archive_read_open_fd(archive, input, 32768) != ARCHIVE_OK)
		fail("initialize tar reader");
	while ((status = archive_read_next_header(archive, &entry)) ==
		       ARCHIVE_OK ||
	       status == ARCHIVE_WARN) {
		if (status == ARCHIVE_WARN) {
			const char *error = archive_error_string(archive);

			if (!error ||
			    (strcmp(error,
				    "Pathname can't be converted from UTF-8 to current locale") &&
			     strcmp(error,
				    "Pathname can't be converted from UTF-8 to current locale.") &&
			     strcmp(error,
				    "Linkname can't be converted from UTF-8 to current locale") &&
			     strcmp(error,
				    "Linkname can't be converted from UTF-8 to current locale.") &&
			     strcmp(error,
				    "Linkpath can't be converted from UTF-8 to current locale") &&
			     strcmp(error,
				    "Linkpath can't be converted from UTF-8 to current locale.")))
				fail("corrupt tar header");
			if (!(*warnings & 16)) {
				fprintf(stderr,
					"geranium layer: warning: tar pathname encoding could not be translated; original bytes retained\n");
				*warnings |= 16;
			}
		}
		const char *raw = archive_entry_pathname(entry);
		const char *target = archive_entry_hardlink(entry);
		const char *symlink = archive_entry_symlink(entry);
		la_int64_t length = archive_entry_size(entry);
		unsigned long long size;
		mode_t mode = archive_entry_perm(entry);
		char type;
		char name[4096], link[4096];

		if (++entries > 100000)
			fail("too many layer entries");
		if (!raw || strlen(raw) >= sizeof name)
			fail("tar path too long");
		strcpy(name, raw);
		if (length < 0 || (unsigned long long)length > ENTRY_LIMIT)
			fail("invalid or excessive tar entry size (1 GiB limit)");
		size = (unsigned long long)length;
		if (target)
			type = '1';
		else if (symlink)
			type = '2';
		else if (archive_entry_filetype(entry) == AE_IFDIR)
			type = '5';
		else if (archive_entry_filetype(entry) == AE_IFREG)
			type = '0';
		else
			type = '?';
		if (type == '?' && !(*warnings & 4)) {
			fprintf(stderr,
				"geranium layer: warning: unsupported device/FIFO/socket entries omitted; no host devices created\n");
			*warnings |= 4;
		}
		if ((archive_entry_uid(entry) || archive_entry_gid(entry)) &&
		    !(*warnings & 1)) {
			fprintf(stderr,
				"geranium layer: warning: image ownership flattened to caller UID/GID (single-ID user namespace)\n");
			*warnings |= 1;
		}
		if ((mode & 06000) && !(*warnings & 2)) {
			fprintf(stderr,
				"geranium layer: warning: set-ID permissions stripped (unsafe with flattened ownership)\n");
			*warnings |= 2;
		}
		if (archive_entry_xattr_count(entry) && !(*warnings & 8)) {
			fprintf(stderr,
				"geranium layer: warning: image extended attributes omitted\n");
			*warnings |= 8;
		}
		mode &= ~06000;
		if (type != '5')
			mode &= ~01000;
		if (type != '0' && size)
			fail("unexpected payload in non-file tar entry");
		const char *rawlink = target ? target : symlink;
		if (rawlink && strlen(rawlink) >= sizeof link)
			fail("tar link too long");
		strcpy(link, rawlink ? rawlink : "");
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

		if (type == '0' && size > ENTRY_LIMIT)
			fail("tar file exceeds 1 GiB limit");

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
				} else if (type == '?') {
					erase(fd, leaf);
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
					    st.st_gid != getegid())
						fail("unsafe hardlink target identity or type");
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
					if (size > EXPAND_LIMIT - *output_total)
						fail("extracted image exceeds 4 GiB");
					*output_total += size;
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
						ssize_t got = archive_read_data(
							archive, buf, want);

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
		if (archive_read_data_skip(archive) != ARCHIVE_OK)
			fail("corrupt or truncated tar payload");
	}
	if (status != ARCHIVE_EOF)
		fail("corrupt tar header");
	check_tar_end(archive, tar);
	if (archive_read_free(archive) != ARCHIVE_OK)
		fail("close tar reader");
	if (!uselocale(previous))
		fail("restore tar extraction locale");
	freelocale(utf8);
	check(close(input), "close layer input");

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

static void staged_bytes(int tar, EVP_MD_CTX *md, const void *data, size_t n,
			 unsigned long long *expanded)
{
	if (n > EXPAND_LIMIT - *expanded)
		fail("expanded image exceeds 4 GiB; use smaller layers/images");
	*expanded += n;
	write_all(tar, data, n);
	if (!EVP_DigestUpdate(md, data, n))
		fail("digest update");
}

void layer_apply(int root, int stage, const void *bytes, size_t size,
		 enum layer_compression compression, const char *diff,
		 unsigned long long *expanded, unsigned long long *output_total)
{
	int tar = openat(stage, "layer.tar",
			 O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			 0600);

	check(tar, "stage layer");
	EVP_MD_CTX *md = EVP_MD_CTX_new();
	if (!md || !EVP_DigestInit_ex(md, EVP_sha256(), NULL))
		fail("digest initialization");
	if (compression == LAYER_GZIP) {
		z_stream z;

		if (size > UINT_MAX)
			fail("gzip layer exceeds compressed input limit");
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
			staged_bytes(tar, md, out, n, expanded);
			if (!n && !z.avail_in && status != Z_STREAM_END)
				fail("truncated gzip");
		} while (status != Z_STREAM_END);
		if (z.avail_in)
			fail("gzip trailing data or concatenation unsupported");
		inflateEnd(&z);
	} else if (compression == LAYER_ZSTD) {
		ZSTD_DStream *stream = ZSTD_createDStream();
		ZSTD_inBuffer input = { bytes, size, 0 };
		size_t need;

		if (!stream || ZSTD_isError(ZSTD_initDStream(stream)))
			fail("zstd initialization");
		do {
			unsigned char output[32768];
			ZSTD_outBuffer out = { output, sizeof output, 0 };
			size_t before = input.pos;

			need = ZSTD_decompressStream(stream, &out, &input);
			if (ZSTD_isError(need))
				fail("corrupt zstd layer");
			staged_bytes(tar, md, output, out.pos, expanded);
			if (need && input.pos == size && !out.pos)
				fail("truncated zstd layer");
			if (need && before == input.pos && !out.pos)
				fail("stalled zstd layer");
		} while (need || input.pos < size);
		ZSTD_freeDStream(stream);
	} else if (compression == LAYER_TAR) {
		staged_bytes(tar, md, bytes, size, expanded);
	} else {
		fail("unsupported layer compression");
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

	unsigned warnings = 0;

	extract(root, tar, 1, &warnings, output_total);
	extract(root, tar, 0, &warnings, output_total);
	close(tar);
	check(unlinkat(stage, "layer.tar", 0), "remove staged layer");
}
