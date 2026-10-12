#define _POSIX_C_SOURCE 200809L
#include "image.h"
#include "registry.h"
#include "json.h"
#include "layer.h"
#include "backend.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
struct reference {
	char host[256], repo[256], tag[129], key[65];
};
static void fail(const char *s)
{
	fprintf(stderr, "geranium image: %s\n", s);
	exit(1);
}
static void check(int n, const char *s)
{
	if (n < 0) {
		perror(s);
		exit(1);
	}
}
static void hash(const void *p, size_t n, char out[65])
{
	unsigned char d[32];
	unsigned len = 0;
	if (!EVP_Digest(p, n, d, &len, EVP_sha256(), NULL) || len != 32)
		fail("SHA256 failed");
	for (unsigned i = 0; i < 32; i++)
		(void)sprintf(out + 2 * i, "%02x", d[i]);
}
static int digest_ok(const char *s)
{
	if (strlen(s) != 71 || strncmp(s, "sha256:", 7))
		return 0;
	for (s += 7; *s; s++)
		if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
			return 0;
	return 1;
}
static struct reference reference(const char *input)
{
	struct reference r;
	memset(&r, 0, sizeof r);
	if (!*input || strlen(input) > 500)
		fail("invalid image reference");
	char text[512];
	strcpy(text, input);
	for (const char *p = text; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || strchr("/._-:@", *p)))
			fail("invalid image reference");
	char *suffix = strchr(text, '@');
	if (suffix) {
		*suffix++ = 0;
		if (!digest_ok(suffix))
			fail("invalid image reference");
	} else {
		char *colon = strrchr(text, ':'), *slash = strrchr(text, '/');
		if (colon && (!slash || colon > slash)) {
			*colon++ = 0;
			suffix = colon;
		}
	}
	if (!suffix)
		suffix = "latest";
	if (!*suffix || strlen(suffix) >= sizeof r.tag)
		fail("invalid image reference");
	if (!digest_ok(suffix)) {
		if (!((*suffix >= 'a' && *suffix <= 'z') ||
		      (*suffix >= 'A' && *suffix <= 'Z') ||
		      (*suffix >= '0' && *suffix <= '9') || *suffix == '_'))
			fail("invalid image reference");
		for (const char *p = suffix; *p; p++)
			if (!((*p >= 'a' && *p <= 'z') ||
			      (*p >= 'A' && *p <= 'Z') ||
			      (*p >= '0' && *p <= '9') || strchr("_.-", *p)))
				fail("invalid image reference");
	}
	strcpy(r.tag, suffix);
	if (text[0] == '.' || strstr(text, "://"))
		fail("invalid image reference");
	char *slash = strchr(text, '/'), *repo = text;
	if (slash && (memchr(text, '.', (size_t)(slash - text)) ||
		      memchr(text, ':', (size_t)(slash - text)) ||
		      !strncmp(text, "localhost/", 10))) {
		*slash = 0;
		if (strlen(text) >= sizeof r.host)
			fail("invalid image reference");
		strcpy(r.host, text);
		repo = slash + 1;
	} else
		strcpy(r.host, "registry-1.docker.io");
	if (!strcmp(r.host, "docker.io"))
		strcpy(r.host, "registry-1.docker.io");
	if (!*repo || strlen(repo) > 230)
		fail("invalid image reference");
	for (char *p = repo; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      strchr("/._-", *p)))
			fail("invalid image reference");
		if ((*p == '/' && (p == repo || p[1] == '/' || !p[1])) ||
		    (*p == '.' && (p == repo || p[-1] == '/') &&
		     (p[1] == '.' || p[1] == '/' || !p[1])))
			fail("invalid image reference");
	}
	for (char *p = repo; *p;) {
		char *end = strchr(p, '/');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ||
		    !((p[n - 1] >= 'a' && p[n - 1] <= 'z') ||
		      (p[n - 1] >= '0' && p[n - 1] <= '9')))
			fail("invalid image reference");
		for (size_t k = 0; k < n; k++)
			if (strchr("._", p[k])) {
				char separator = p[k];
				if (separator == '_' && k + 1 < n &&
				    p[k + 1] == '_')
					k++;
				if (k + 1 >= n ||
				    !((p[k + 1] >= 'a' && p[k + 1] <= 'z') ||
				      (p[k + 1] >= '0' && p[k + 1] <= '9')))
					fail("invalid image reference");
			}
		p = end ? end + 1 : p + n;
	}
	if (!strchr(repo, '/') && !strcmp(r.host, "registry-1.docker.io")) {
		strcpy(r.repo, "library/");
		strcat(r.repo, repo);
	} else
		strcpy(r.repo, repo);
	for (char *p = r.host; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      strchr(".:-", *p)))
			fail("invalid image reference");
	char hostcopy[256];
	strcpy(hostcopy, r.host);
	char *port = strchr(hostcopy, ':');
	if (port) {
		*port++ = 0;
		if (!*port || strlen(port) > 5)
			fail("invalid image reference");
		unsigned value = 0;
		for (char *p = port; *p; p++) {
			if (*p < '0' || *p > '9')
				fail("invalid image reference");
			value = value * 10 + (unsigned)(*p - '0');
		}
		if (!value || value > 65535)
			fail("invalid image reference");
	}
	for (char *p = hostcopy; *p;) {
		char *end = strchr(p, '.');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (!n || n > 63 || p[0] == '-' || p[n - 1] == '-' ||
		    (end && !end[1]))
			fail("invalid image reference");
		p = end ? end + 1 : p + n;
	}
	char normalized[700];
	int n = snprintf(normalized, sizeof normalized, "%s/%s@%s", r.host,
			 r.repo, r.tag);
	hash(normalized, (size_t)n, r.key);
	return r;
}
static int dir(int fd, const char *s)
{
	int n = openat(fd, s, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	check(n, "image directory");
	return n;
}
static int store(int state)
{
	if (mkdirat(state, ".images", 0700) < 0 && errno != EEXIST)
		check(-1, "image store");
	int fd = dir(state, ".images");
	struct stat st;
	check(fstat(fd, &st), "image store stat");
	if (st.st_uid != geteuid() || (st.st_mode & 0077))
		fail("unsafe image store");
	return fd;
}
static int lock_image(int fd, const char *key)
{
	char name[80];
	(void)snprintf(name, sizeof name, ".lock-%s", key);
	int lock = openat(
		fd, name,
		O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
	check(lock, "image lock");
	struct stat st;
	check(fstat(lock, &st), "image lock stat");
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0077) || st.st_nlink != 1)
		fail("unsafe image lock");
	struct flock l = { 0 };
	l.l_type = F_WRLCK;
	l.l_whence = SEEK_SET;
	check(fcntl(lock, F_SETLKW, &l), "image lock");
	return lock;
}
static void write_all(int fd, const void *data, size_t n)
{
	const char *p = data;
	while (n) {
		ssize_t k = write(fd, p, n);
		if (k <= 0) {
			if (k < 0 && errno == EINTR)
				continue;
			fail("write image");
		}
		p += k;
		n -= (size_t)k;
	}
}
static void save(int fd, const char *name, const void *data, size_t n)
{
	int out = openat(fd, name,
			 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			 0600);
	check(out, "image metadata");
	write_all(out, data, n);
	check(fsync(out), "sync image metadata");
	check(close(out), "close image metadata");
}
static void reclaim(int fd, const char *name)
{
	struct stat st;
	if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		if (errno == ENOENT)
			return;
		check(-1, "image stage stat");
	}
	if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0077))
		fail("unsafe image stage");
	int stage = dir(fd, name);
	image_delete_tree(stage);
	close(stage);
	check(unlinkat(fd, name, AT_REMOVEDIR), "image stage removal");
	check(fsync(fd), "sync image stage removal");
}
static struct response fetch(const struct reference *r, const char *kind,
			     const char *id, int local, char **token,
			     size_t limit)
{
	char url[2048];
	int n = snprintf(url, sizeof url, "%s://%s/v2/%s/%s/%s",
			 local ? "http" : "https", r->host, r->repo, kind, id);
	if (n < 0 || (size_t)n >= sizeof url)
		fail("registry URL too long");
	struct response res = registry_get(url, *token, local, limit);
	if (res.status == 401) {
		free(*token);
		*token = registry_token(res.challenge, local);
		free(res.data);
		res = registry_get(url, *token, local, limit);
	}
	if (res.status != 200) {
		fprintf(stderr, "geranium image: registry HTTP status %d\n",
			res.status);
		exit(1);
	}
	return res;
}
static void verify(struct response *res, const char *digest,
		   unsigned long long size)
{
	char got[72];
	strcpy(got, "sha256:");
	hash(res->data, res->size, got + 7);
	if (!digest_ok(digest) || strcmp(got, digest) || res->size != size)
		fail("descriptor digest or size mismatch");
	if (*res->digest && strcmp(res->digest, got))
		fail("registry digest header mismatch");
}
static int media(const char *s, const char *oci, const char *docker)
{
	return !strcmp(s, oci) || !strcmp(s, docker);
}
static const char *response_type(struct response *r, struct json *j)
{
	size_t n = strcspn(r->type, ";");
	while (n && (r->type[n - 1] == ' ' || r->type[n - 1] == '\t'))
		n--;
	r->type[n] = 0;
	struct json *field = json_get(j, "mediaType");
	if (!field) {
		if (!n)
			fail("missing HTTP manifest Content-Type");
		return r->type;
	}
	const char *type = json_string(field);
	if (strcmp(r->type, type))
		fail("HTTP manifest Content-Type mismatch");
	return type;
}
static void manifest_type(struct json *j, const char *type)
{
	if (json_uint(json_get(j, "schemaVersion")) != 2 ||
	    !media(type, "application/vnd.oci.image.manifest.v1+json",
		   "application/vnd.docker.distribution.manifest.v2+json"))
		fail("unsupported image manifest mediaType/schema");
}
static void config_parse(struct json *j, struct image_config *c)
{
	memset(c, 0, sizeof *c);
	const char *arch = backend_image_arch();
	if (!arch || strcmp(json_string(json_get(j, "os")), "linux") ||
	    strcmp(json_string(json_get(j, "architecture")), arch))
		fail("unsupported image platform");
	struct json *config = json_get(j, "config");
	if (!config || config->type != '{')
		fail("missing image configuration");
	struct json *user = json_get(config, "User");
	if (user && user->type != 'n') {
		const char *s = json_string(user);
		if (*s && strcmp(s, "0") && strcmp(s, "root") &&
		    strcmp(s, "0:0") && strcmp(s, "root:root"))
			fail("non-root image User unsupported");
	}
	struct json *wd = json_get(config, "WorkingDir");
	const char *work = wd && wd->type != 'n' ? json_string(wd) : "/";
	if (!*work)
		work = "/";
	if (*work != '/' || strlen(work) >= sizeof c->workdir)
		fail("invalid image WorkingDir");
	strcpy(c->workdir, work);
	struct json *lists[2] = { json_get(config, "Entrypoint"),
				  json_get(config, "Cmd") };
	size_t bytes = 0;
	unsigned count = 0;
	for (unsigned i = 0; i < 2; i++)
		if (lists[i] && lists[i]->type != 'n') {
			if (lists[i]->type != '[')
				fail("image command must be an array");
			for (struct json *q = lists[i]->child; q; q = q->next) {
				const char *s = json_string(q);
				size_t n = strlen(s) + 1;
				if (count == IMAGE_ARGS ||
				    bytes + n > IMAGE_BYTES)
					fail("image command too large");
				c->args[count++] = c->data + bytes;
				memcpy(c->data + bytes, s, n);
				bytes += n;
			}
		}
	struct json *env = json_get(config, "Env");
	bytes = 0;
	count = 0;
	if (env && env->type != 'n') {
		if (env->type != '[')
			fail("image Env must be an array");
		for (struct json *q = env->child; q; q = q->next) {
			const char *s = json_string(q), *eq = strchr(s, '=');
			if (!eq || eq == s)
				fail("invalid image environment");
			for (const char *p = s; p < eq; p++)
				if (!((*p >= 'a' && *p <= 'z') ||
				      (*p >= 'A' && *p <= 'Z') || *p == '_' ||
				      (p > s && *p >= '0' && *p <= '9')))
					fail("invalid image environment name");
			for (unsigned k = 0; k < count; k++)
				if (!strncmp(c->env[k], s,
					     (size_t)(eq - s) + 1))
					fail("duplicate image environment");
			size_t n = strlen(s) + 1;
			if (count == IMAGE_ARGS || bytes + n > IMAGE_BYTES)
				fail("image environment too large");
			c->env[count++] = c->envdata + bytes;
			memcpy(c->envdata + bytes, s, n);
			bytes += n;
		}
	}
}
static int config_read(int, const char *, struct image_config *, int);
int image_pull(int state, const char *input, int local_http)
{
	struct reference r = reference(input);
	int fd = store(state), lock = lock_image(fd, r.key);
	char pending[80];
	(void)snprintf(pending, sizeof pending, ".pending-%s", r.key);
	reclaim(fd, pending);
	struct stat st;
	if (fstatat(fd, r.key, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
		    (st.st_mode & 0077))
			fail("unsafe published image");
		int existing = dir(fd, r.key);
		struct image_config config;
		(void)config_read(existing, "config.json", &config, 0);
		free(config.source);
		int cached_root = dir(existing, "rootfs");
		close(cached_root);
		int manifest =
			openat(existing, "manifest.json",
			       O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
		check(manifest, "published image manifest");
		struct stat metadata;
		check(fstat(manifest, &metadata), "published manifest stat");
		if (!S_ISREG(metadata.st_mode) ||
		    metadata.st_uid != geteuid() || metadata.st_nlink != 1 ||
		    metadata.st_size < 1 || metadata.st_size > 1024 * 1024)
			fail("invalid published image manifest");
		close(manifest);
		close(existing);
		close(lock);
		close(fd);
		return 0;
	}
	if (errno != ENOENT)
		check(-1, "image existence");
	check(mkdirat(fd, pending, 0700), "image stage");
	int stage = dir(fd, pending);
	check(mkdirat(stage, "rootfs", 0700), "image rootfs");
	int root = dir(stage, "rootfs");
	char *token = NULL;
	struct response res =
		fetch(&r, "manifests", r.tag, local_http, &token, 1024 * 1024);
	if (digest_ok(r.tag))
		verify(&res, r.tag, res.size);
	else if (*res.digest)
		verify(&res, res.digest, res.size);
	struct json *j = json_parse(res.data, res.size);
	const char *type = response_type(&res, j);
	if (media(type, "application/vnd.oci.image.index.v1+json",
		  "application/vnd.docker.distribution.manifest.list.v2+json")) {
		if (json_uint(json_get(j, "schemaVersion")) != 2)
			fail("unsupported index schema");
		struct json *list = json_get(j, "manifests"), *selected = NULL;
		const char *arch = backend_image_arch();
		if (!arch || !list || list->type != '[')
			fail("unsupported index platform");
		for (struct json *d = list->child; d; d = d->next) {
			struct json *p = json_get(d, "platform");
			if (!p)
				continue;
			if (!strcmp(json_string(json_get(p, "os")), "linux") &&
			    !strcmp(json_string(json_get(p, "architecture")),
				    arch)) {
				struct json *v = json_get(p, "variant");
				const char *variant = v ? json_string(v) : "";
				if (*variant && (strcmp(arch, "arm64") ||
						 strcmp(variant, "v8")))
					continue;
				if (selected)
					fail("ambiguous index platform");
				selected = d;
			}
		}
		if (!selected)
			fail("matching Linux image platform not found");
		const char *dg = json_string(json_get(selected, "digest"));
		unsigned long long sz = json_uint(json_get(selected, "size"));
		if (!digest_ok(dg) || sz > 1024 * 1024 ||
		    !media(json_string(json_get(selected, "mediaType")),
			   "application/vnd.oci.image.manifest.v1+json",
			   "application/vnd.docker.distribution.manifest.v2+json"))
			fail("unsupported selected manifest descriptor");
		char selected_type[256];
		const char *mt = json_string(json_get(selected, "mediaType"));
		if (strlen(mt) >= sizeof selected_type)
			fail("selected mediaType too long");
		strcpy(selected_type, mt);
		free(res.data);
		res = fetch(&r, "manifests", dg, local_http, &token,
			    1024 * 1024);
		verify(&res, dg, sz);
		json_free(j);
		j = json_parse(res.data, res.size);
		type = response_type(&res, j);
		if (strcmp(type, selected_type))
			fail("selected manifest mediaType mismatch");
	}
	manifest_type(j, type);
	struct json *cfg = json_get(j, "config");
	const char *dg = json_string(json_get(cfg, "digest"));
	unsigned long long sz = json_uint(json_get(cfg, "size"));
	if (!digest_ok(dg) || sz > 1024 * 1024 ||
	    !media(json_string(json_get(cfg, "mediaType")),
		   "application/vnd.oci.image.config.v1+json",
		   "application/vnd.docker.container.image.v1+json"))
		fail("unsupported config descriptor");
	struct response conf =
		fetch(&r, "blobs", dg, local_http, &token, 1024 * 1024);
	verify(&conf, dg, sz);
	struct json *cj = json_parse(conf.data, conf.size);
	struct image_config parsed;
	config_parse(cj, &parsed);
	struct json *rf = json_get(cj, "rootfs");
	if (strcmp(json_string(json_get(rf, "type")), "layers"))
		fail("unsupported rootfs type");
	struct json *diffs = json_get(rf, "diff_ids"),
		    *layers = json_get(j, "layers");
	if (!diffs || diffs->type != '[' || !layers || layers->type != '[')
		fail("invalid image layers");
	struct json *diff = diffs->child;
	unsigned count = 0;
	unsigned long long expanded = 0;
	for (struct json *layer = layers->child; layer; layer = layer->next) {
		if (++count > 128 || !diff)
			fail("excessive layers or DiffID count mismatch");
		dg = json_string(json_get(layer, "digest"));
		sz = json_uint(json_get(layer, "size"));
		type = json_string(json_get(layer, "mediaType"));
		int gzip = media(
			type, "application/vnd.oci.image.layer.v1.tar+gzip",
			"application/vnd.docker.image.rootfs.diff.tar.gzip");
		if (!gzip &&
		    !media(type, "application/vnd.oci.image.layer.v1.tar",
			   "application/vnd.docker.image.rootfs.diff.tar"))
			fail("unsupported layer mediaType");
		if (!digest_ok(dg) || sz > 64 * 1024 * 1024 ||
		    !digest_ok(json_string(diff)))
			fail("unsupported or excessive layer descriptor");
		struct response blob = fetch(&r, "blobs", dg, local_http,
					     &token, 64 * 1024 * 1024);
		verify(&blob, dg, sz);
		layer_apply(root, stage, blob.data, blob.size, gzip,
			    json_string(diff), &expanded);
		free(blob.data);
		diff = diff->next;
	}
	if (diff)
		fail("DiffID count mismatch");
	check(fchmod(root, (mode_t)layer_root_mode()),
	      "image root permissions");
	close(root);
	save(stage, "config.json", conf.data, conf.size);
	save(stage, "manifest.json", res.data, res.size);
	save(stage, "reference", input, strlen(input));
	check(fsync(stage), "sync staged image");
	check(renameat(fd, pending, fd, r.key), "publish image");
	check(fsync(fd), "sync image publication");
	json_free(cj);
	json_free(j);
	free(conf.data);
	free(res.data);
	free(token);
	close(stage);
	close(lock);
	close(fd);
	return 0;
}
int image_remove(int state, const char *input)
{
	struct reference r = reference(input);
	int fd = store(state), lock = lock_image(fd, r.key);
	char pending[80];
	(void)snprintf(pending, sizeof pending, ".pending-%s", r.key);
	struct stat published, staged;
	int exists = fstatat(fd, r.key, &published, AT_SYMLINK_NOFOLLOW) == 0;
	if (!exists && errno != ENOENT)
		check(-1, "image lookup");
	int has_stage = fstatat(fd, pending, &staged, AT_SYMLINK_NOFOLLOW) == 0;
	if (!has_stage && errno != ENOENT)
		check(-1, "image stage lookup");
	if (!exists && !has_stage)
		fail("image not found");
	if (exists &&
	    (!S_ISDIR(published.st_mode) || published.st_uid != geteuid() ||
	     (published.st_mode & 0077)))
		fail("unsafe published image");
	reclaim(fd, pending);
	if (exists) {
		check(renameat(fd, r.key, fd, pending), "unpublish image");
		check(fsync(fd), "sync image unpublication");
		reclaim(fd, pending);
	}
	close(lock);
	close(fd);
	return 0;
}

static int config_read(int fd, const char *name, struct image_config *c,
		       int optional)
{
	int in = openat(fd, name,
			O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (in < 0 && optional && errno == ENOENT)
		return 0;
	check(in, "image configuration");
	struct stat st;
	check(fstat(in, &st), "image config stat");
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
	    st.st_uid != geteuid() || st.st_size < 1 ||
	    st.st_size > 1024 * 1024)
		fail("invalid saved image configuration");
	size_t size = (size_t)st.st_size;
	unsigned char *data = malloc(size + 1);
	if (!data)
		fail("allocation");
	size_t offset = 0;
	while (offset < size) {
		ssize_t n = read(in, data + offset, size - offset);
		if (n <= 0)
			fail("read image config");
		offset += (size_t)n;
	}
	close(in);
	data[size] = 0;
	struct json *j = json_parse(data, size);
	config_parse(j, c);
	json_free(j);
	c->source = data;
	c->size = (unsigned long)size;
	return 1;
}
int image_open(int state, const char *input, int *lock, struct image_config *c)
{
	struct reference r = reference(input);
	int fd = store(state);
	*lock = lock_image(fd, r.key);
	int image = dir(fd, r.key);
	struct stat st;
	check(fstat(image, &st), "image stat");
	if (st.st_uid != geteuid() || (st.st_mode & 0077))
		fail("unsafe published image");
	(void)config_read(image, "config.json", c, 0);
	int root = dir(image, "rootfs");
	close(image);
	close(fd);
	return root;
}
void image_config_save(int container, const struct image_config *c)
{
	save(container, "image.json", c->source, (size_t)c->size);
}
int image_config_load(int container, struct image_config *c)
{
	return config_read(container, "image.json", c, 1);
}
