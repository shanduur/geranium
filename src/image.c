#define _POSIX_C_SOURCE 200809L
#include "image.h"
#include "registry.h"
#include "json.h"
#include "layer.h"
#include "backend.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <skalibs/strerr.h>
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
/* Slots are reserved until ordered extraction, including completed workers.
 * Four workers each own at most LAYER_LIMIT+1 response bytes and, during auth
 * refresh, an additional 1 MiB+1 token response. Four LAYER_LIMIT-sized files
 * bound compressed staging; the parent maps one. Extraction's layer.tar,
 * rootfs, TLS, JSON, inherited metadata and page cache are separate limits,
 * so this is not a bound on total staging or RSS. */
#define PULL_SLOTS 4
#define LAYER_LIMIT (64 * 1024 * 1024)
struct pull_layer {
	const char *digest, *diff;
	size_t size;
	int gzip;
};
struct pull_slot {
	volatile sig_atomic_t pid;
	int event, file, done;
};
static struct pull_slot pull_slots[PULL_SLOTS];
static pid_t pull_owner;
static volatile sig_atomic_t pull_cancel;
static sigset_t pull_signals, pull_mask;
static const int pull_signums[] = { SIGINT, SIGTERM, SIGCHLD };
static struct sigaction pull_actions[3];

static void pull_interrupt(int sig)
{
	int saved_errno = errno;
	pull_cancel = sig;
	/* PIDs cannot be recycled until reaped; reaping blocks these signals. */
	for (unsigned i = 0; i < PULL_SLOTS; i++)
		if (pull_slots[i].pid > 0)
			(void)kill((pid_t)pull_slots[i].pid, SIGKILL);
	errno = saved_errno;
}
static void pull_cleanup(void)
{
	if (!pull_owner || pull_owner != getpid())
		return;
	(void)sigprocmask(SIG_BLOCK, &pull_signals, NULL);
	for (unsigned i = 0; i < PULL_SLOTS; i++)
		if (pull_slots[i].pid > 0)
			(void)kill((pid_t)pull_slots[i].pid, SIGKILL);
	for (unsigned i = 0; i < PULL_SLOTS; i++) {
		if (pull_slots[i].pid > 0) {
			while (waitpid((pid_t)pull_slots[i].pid, NULL, 0) < 0 &&
			       errno == EINTR)
				;
			pull_slots[i].pid = 0;
		}
		if (pull_slots[i].event >= 0)
			close(pull_slots[i].event);
		if (pull_slots[i].file >= 0)
			close(pull_slots[i].file);
		pull_slots[i].event = pull_slots[i].file = -1;
	}
	pull_owner = 0;
	/* SIGCHLD was never changed: even restoring an ignored action can
	 * discard a caller notification pending while workers were active. */
	for (unsigned i = 0; i < 2; i++)
		(void)sigaction(pull_signums[i], &pull_actions[i], NULL);
	(void)sigprocmask(SIG_SETMASK, &pull_mask, NULL);
}
static void pull_begin(void)
{
	static int registered;
	if (!registered) {
		if (atexit(pull_cleanup))
			fail("register pull cleanup");
		registered = 1;
	}
	pull_cancel = 0;
	for (unsigned i = 0; i < PULL_SLOTS; i++) {
		pull_slots[i].pid = 0;
		pull_slots[i].event = pull_slots[i].file = -1;
		pull_slots[i].done = 0;
	}
	sigemptyset(&pull_signals);
	for (unsigned i = 0; i < 3; i++) {
		sigaddset(&pull_signals, pull_signums[i]);
		check(sigaction(pull_signums[i], NULL, &pull_actions[i]),
		      "read pull signal action");
	}
	check(sigprocmask(SIG_BLOCK, &pull_signals, &pull_mask),
	      "block pull signals");
	pull_owner = getpid();
	struct sigaction action;
	memset(&action, 0, sizeof action);
	sigemptyset(&action.sa_mask);
	action.sa_handler = pull_interrupt;
	if (pull_actions[0].sa_handler != SIG_IGN)
		check(sigaction(SIGINT, &action, NULL),
		      "pull interrupt action");
	if (pull_actions[1].sa_handler != SIG_IGN)
		check(sigaction(SIGTERM, &action, NULL),
		      "pull termination action");
	/* Retain pending caller notifications and prevent caller handlers from
	 * reaping owned workers. EOF pipes, not SIGCHLD, drive completion. */
	sigset_t running = pull_mask;
	sigaddset(&running, SIGCHLD);
	check(sigprocmask(SIG_SETMASK, &running, NULL), "unblock pull signals");
}
static void pull_cancelled(void)
{
	if (pull_cancel) {
		int sig = pull_cancel;
		pull_cleanup();
		/* Preserve a caller's disposition and mask when terminating. */
		(void)raise(sig);
		exit(128 + sig);
	}
}
static void layer_progress(unsigned index, unsigned count, const char *verb)
{
	char position[16];
	(void)snprintf(position, sizeof position, "%u/%u", index + 1, count);
	strerr_warnn(4, "geranium: layer ", position, " ", verb);
}
/* Last registered, first invoked: fatal helpers must not run inherited
 * caller atexit/on_exit callbacks or keep the completion pipe open. */
static void pull_worker_exit(void)
{
	_exit(1);
}
static void pull_spawn(struct pull_slot *slot, unsigned index, unsigned count,
		       int stage, const struct reference *r,
		       const struct pull_layer *layer, int local, char *token)
{
	pull_cancelled();
	char name[32];
	(void)snprintf(name, sizeof name, ".layer-%u", index);
	slot->file = openat(stage, name,
			    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			    0600);
	check(slot->file, "stage layer file");
	int events[2];
	check(pipe(events), "layer event pipe");
	slot->event = events[0];
	layer_progress(index, count, "downloading");
	sigset_t previous;
	check(sigprocmask(SIG_BLOCK, &pull_signals, &previous),
	      "block worker signals");
	pid_t pid = fork();
	check((int)pid, "fork layer worker");
	if (!pid) {
		if (atexit(pull_worker_exit))
			_exit(1);
		/* Close the parent-death registration race before doing any I/O. */
		check(backend_watch_parent((int)pull_owner),
		      "watch pull parent");
		pull_owner = 0;
		struct sigaction action;
		memset(&action, 0, sizeof action);
		sigemptyset(&action.sa_mask);
		for (unsigned i = 0; i < 3; i++) {
			action.sa_handler = SIG_DFL;
			if (pull_actions[i].sa_handler == SIG_IGN)
				action.sa_handler = SIG_IGN;
			check(sigaction(pull_signums[i], &action, NULL),
			      "worker signal action");
		}
		int keep[] = { slot->file, events[1] };
		check(backend_close_inherited(keep, 2),
		      "close worker descriptors");
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		check(sigprocmask(SIG_SETMASK, &previous, NULL),
		      "worker signal mask");
		struct response blob = fetch(r, "blobs", layer->digest, local,
					     &token, LAYER_LIMIT);
		layer_progress(index, count, "verifying");
		verify(&blob, layer->digest, layer->size);
		write_all(slot->file, blob.data, blob.size);
		check(fsync(slot->file), "sync downloaded layer");
		free(blob.data);
		/* Pipe EOF is the completion event, including fatal helper exits. */
		_exit(0);
	}
	slot->pid = pid;
	slot->done = 0;
	close(events[1]);
	check(sigprocmask(SIG_SETMASK, &previous, NULL),
	      "restore worker signals");
}
static void pull_events(unsigned slots, int wait)
{
	struct pollfd events[PULL_SLOTS];
	for (unsigned i = 0; i < slots; i++) {
		events[i].fd = pull_slots[i].event;
		events[i].events = POLLIN;
		events[i].revents = 0;
	}
	pull_cancelled();
	int n = poll(events, slots, wait ? -1 : 0);
	if (n < 0 && errno == EINTR) {
		pull_cancelled();
		return;
	}
	check(n, "poll layer workers");
	for (unsigned i = 0; i < slots; i++) {
		if (!events[i].revents)
			continue;
		struct pull_slot *slot = &pull_slots[i];
		sigset_t previous;
		check(sigprocmask(SIG_BLOCK, &pull_signals, &previous),
		      "block reap signals");
		int status;
		pid_t pid;
		do {
			pid = waitpid((pid_t)slot->pid, &status, 0);
		} while (pid < 0 && errno == EINTR);
		check((int)pid, "reap layer worker");
		slot->pid = 0;
		close(slot->event);
		slot->event = -1;
		slot->done = 1;
		check(sigprocmask(SIG_SETMASK, &previous, NULL),
		      "restore reap signals");
		pull_cancelled();
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			fail("layer worker failed");
	}
}
static void pull_layers(int root, int stage, const struct reference *r,
			struct pull_layer *layers, unsigned count, int local,
			char **token)
{
	unsigned long long expanded = 0;
	long cpus = backend_pull_cpus();
	struct sigaction children;
	check(sigaction(SIGCHLD, NULL, &children), "read pull child action");
	/* Auto-reaping cannot safely coexist with owned waitpid/PID lifetimes.
	 * Serial acquisition preserves the caller's action and notifications. */
	if (children.sa_handler == SIG_IGN ||
	    (children.sa_flags & SA_NOCLDWAIT))
		cpus = 0;
	if (!cpus) {
		/* Portable storage still works without unsafe orphanable workers. */
		for (unsigned i = 0; i < count; i++) {
			layer_progress(i, count, "downloading");
			struct response blob = fetch(r, "blobs",
						     layers[i].digest, local,
						     token, LAYER_LIMIT);
			layer_progress(i, count, "verifying");
			verify(&blob, layers[i].digest, layers[i].size);
			layer_progress(i, count, "extracting");
			layer_apply(root, stage, blob.data, blob.size,
				    layers[i].gzip, layers[i].diff, &expanded);
			free(blob.data);
		}
		return;
	}
	unsigned slots = cpus > PULL_SLOTS ? PULL_SLOTS : (unsigned)cpus;
	if (slots > count)
		slots = count;
	if (!slots)
		return;
	pull_begin();
	for (unsigned i = 0; i < slots; i++) {
		pull_events(slots, 0);
		pull_spawn(&pull_slots[i], i, count, stage, r, &layers[i],
			   local, *token);
	}
	for (unsigned i = 0; i < count; i++) {
		struct pull_slot *slot = &pull_slots[i % slots];
		/* Observe every completion, never just the next manifest-order PID. */
		pull_events(slots, 0);
		while (!slot->done)
			pull_events(slots, 1);
		struct stat st;
		check(fstat(slot->file, &st), "downloaded layer stat");
		if (!S_ISREG(st.st_mode) || st.st_size < 1 ||
		    (unsigned long long)st.st_size != layers[i].size ||
		    st.st_uid != geteuid() || st.st_nlink != 1)
			fail("invalid downloaded layer file");
		void *data = mmap(NULL, layers[i].size, PROT_READ, MAP_PRIVATE,
				  slot->file, 0);
		if (data == MAP_FAILED)
			check(-1, "map downloaded layer");
		layer_progress(i, count, "extracting");
		layer_apply(root, stage, data, layers[i].size, layers[i].gzip,
			    layers[i].diff, &expanded);
		check(munmap(data, layers[i].size), "unmap downloaded layer");
		close(slot->file);
		slot->file = -1;
		char name[32];
		(void)snprintf(name, sizeof name, ".layer-%u", i);
		check(unlinkat(stage, name, 0), "remove downloaded layer");
		pull_events(slots, 0);
		if (i + slots < count)
			pull_spawn(slot, i + slots, count, stage, r,
				   &layers[i + slots], local, *token);
	}
	pull_cancelled();
	pull_cleanup();
	pull_cancelled();
}
static int media(const char *s, const char *oci, const char *docker)
{
	return !strcmp(s, oci) || !strcmp(s, docker);
}
static const char *response_type(struct response *r, yyjson_val *j)
{
	size_t n = strcspn(r->type, ";");
	while (n && (r->type[n - 1] == ' ' || r->type[n - 1] == '\t'))
		n--;
	r->type[n] = 0;
	yyjson_val *field = json_get(j, "mediaType");
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
static void manifest_type(yyjson_val *j, const char *type)
{
	if (json_uint(json_get(j, "schemaVersion")) != 2 ||
	    !media(type, "application/vnd.oci.image.manifest.v1+json",
		   "application/vnd.docker.distribution.manifest.v2+json"))
		fail("unsupported image manifest mediaType/schema");
}
static void config_parse(yyjson_val *j, struct image_config *c)
{
	memset(c, 0, sizeof *c);
	const char *arch = backend_image_arch();
	if (!arch || strcmp(json_string(json_get(j, "os")), "linux") ||
	    strcmp(json_string(json_get(j, "architecture")), arch))
		fail("unsupported image platform");
	yyjson_val *config = json_get(j, "config");
	if (!config || !yyjson_is_obj(config))
		fail("missing image configuration");
	yyjson_val *user = json_get(config, "User");
	if (user && !yyjson_is_null(user)) {
		const char *s = json_string(user);
		if (*s && strcmp(s, "0") && strcmp(s, "root") &&
		    strcmp(s, "0:0") && strcmp(s, "root:root"))
			fail("non-root image User unsupported");
	}
	yyjson_val *wd = json_get(config, "WorkingDir");
	const char *work = wd && !yyjson_is_null(wd) ? json_string(wd) : "/";
	if (!*work)
		work = "/";
	if (*work != '/' || strlen(work) >= sizeof c->workdir)
		fail("invalid image WorkingDir");
	strcpy(c->workdir, work);
	yyjson_val *lists[2] = { json_get(config, "Entrypoint"),
				 json_get(config, "Cmd") };
	size_t bytes = 0;
	unsigned count = 0;
	for (unsigned i = 0; i < 2; i++)
		if (lists[i] && !yyjson_is_null(lists[i])) {
			if (!yyjson_is_arr(lists[i]))
				fail("image command must be an array");
			size_t index, length;
			yyjson_val *q;
			yyjson_arr_foreach(lists[i], index, length, q)
			{
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
	yyjson_val *env = json_get(config, "Env");
	bytes = 0;
	count = 0;
	if (env && !yyjson_is_null(env)) {
		if (!yyjson_is_arr(env))
			fail("image Env must be an array");
		size_t index, length;
		yyjson_val *q;
		yyjson_arr_foreach(env, index, length, q)
		{
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
		strerr_warnn(2, "geranium: ", "already present");
		return 0;
	}
	if (errno != ENOENT)
		check(-1, "image existence");
	check(mkdirat(fd, pending, 0700), "image stage");
	int stage = dir(fd, pending);
	check(mkdirat(stage, "rootfs", 0700), "image rootfs");
	int root = dir(stage, "rootfs");
	char *token = NULL;
	strerr_warnn(2, "geranium: ", "fetching manifest");
	struct response res =
		fetch(&r, "manifests", r.tag, local_http, &token, 1024 * 1024);
	if (digest_ok(r.tag))
		verify(&res, r.tag, res.size);
	else if (*res.digest)
		verify(&res, res.digest, res.size);
	yyjson_doc *doc = json_parse(res.data, res.size);
	yyjson_val *j = yyjson_doc_get_root(doc);
	const char *type = response_type(&res, j);
	if (media(type, "application/vnd.oci.image.index.v1+json",
		  "application/vnd.docker.distribution.manifest.list.v2+json")) {
		if (json_uint(json_get(j, "schemaVersion")) != 2)
			fail("unsupported index schema");
		yyjson_val *list = json_get(j, "manifests"), *selected = NULL;
		const char *arch = backend_image_arch();
		if (!arch || !list || !yyjson_is_arr(list))
			fail("unsupported index platform");
		size_t index, length;
		yyjson_val *d;
		yyjson_arr_foreach(list, index, length, d)
		{
			yyjson_val *p = json_get(d, "platform");
			if (!p)
				continue;
			if (!strcmp(json_string(json_get(p, "os")), "linux") &&
			    !strcmp(json_string(json_get(p, "architecture")),
				    arch)) {
				yyjson_val *v = json_get(p, "variant");
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
		yyjson_doc_free(doc);
		doc = json_parse(res.data, res.size);
		j = yyjson_doc_get_root(doc);
		type = response_type(&res, j);
		if (strcmp(type, selected_type))
			fail("selected manifest mediaType mismatch");
	}
	manifest_type(j, type);
	yyjson_val *cfg = json_get(j, "config");
	const char *dg = json_string(json_get(cfg, "digest"));
	unsigned long long sz = json_uint(json_get(cfg, "size"));
	if (!digest_ok(dg) || sz > 1024 * 1024 ||
	    !media(json_string(json_get(cfg, "mediaType")),
		   "application/vnd.oci.image.config.v1+json",
		   "application/vnd.docker.container.image.v1+json"))
		fail("unsupported config descriptor");
	strerr_warnn(2, "geranium: ", "fetching config");
	struct response conf =
		fetch(&r, "blobs", dg, local_http, &token, 1024 * 1024);
	verify(&conf, dg, sz);
	yyjson_doc *config_doc = json_parse(conf.data, conf.size);
	yyjson_val *cj = yyjson_doc_get_root(config_doc);
	struct image_config parsed;
	config_parse(cj, &parsed);
	yyjson_val *rf = json_get(cj, "rootfs");
	if (strcmp(json_string(json_get(rf, "type")), "layers"))
		fail("unsupported rootfs type");
	yyjson_val *diffs = json_get(rf, "diff_ids"),
		   *layers = json_get(j, "layers");
	if (!diffs || !yyjson_is_arr(diffs) || !layers ||
	    !yyjson_is_arr(layers))
		fail("invalid image layers");
	yyjson_arr_iter diff_iter = yyjson_arr_iter_with(diffs);
	unsigned count = 0;
	struct pull_layer validated[128];
	size_t index, length;
	yyjson_val *layer;
	yyjson_arr_foreach(layers, index, length, layer)
	{
		yyjson_val *diff = yyjson_arr_iter_next(&diff_iter);
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
		validated[count - 1] = (struct pull_layer){
			.digest = dg,
			.diff = json_string(diff),
			.size = (size_t)sz,
			.gzip = gzip,
		};
	}
	if (yyjson_arr_iter_has_next(&diff_iter))
		fail("DiffID count mismatch");
	pull_layers(root, stage, &r, validated, count, local_http, &token);
	check(fchmod(root, (mode_t)layer_root_mode()),
	      "image root permissions");
	close(root);
	save(stage, "config.json", conf.data, conf.size);
	save(stage, "manifest.json", res.data, res.size);
	save(stage, "reference", input, strlen(input));
	check(fsync(stage), "sync staged image");
	check(renameat(fd, pending, fd, r.key), "publish image");
	check(fsync(fd), "sync image publication");
	yyjson_doc_free(config_doc);
	yyjson_doc_free(doc);
	free(conf.data);
	free(res.data);
	free(token);
	close(stage);
	close(lock);
	close(fd);
	strerr_warnn(2, "geranium: ", "pulled");
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
	strerr_warnn(2, "geranium: ", "removed image");
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
	yyjson_doc *doc = json_parse(data, size);
	config_parse(yyjson_doc_get_root(doc), c);
	yyjson_doc_free(doc);
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
