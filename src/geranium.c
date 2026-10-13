#include "geranium.h"
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include "backend.h"
#include "image.h"
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <skalibs/strerr.h>

#define MAX_ARGS GERANIUM_COMMAND_ARGS_MAX
#define MAX_BYTES GERANIUM_COMMAND_BYTES_MAX
static int manager_diagnostic;
static void die(const char *s)
{
	if (manager_diagnostic) {
		/* A separate 512-byte fatal diagnostic budget beside workload logs. */
		char buffer[512];
		int n = snprintf(buffer, sizeof buffer, "%s: %s", s,
				 strerror(errno));
		size_t size = n < 0 ? 0 : (size_t)n;
		if (size >= sizeof buffer)
			size = sizeof buffer - 1;
		buffer[size++] = '\n';
		ssize_t written = write(2, buffer, size);
		(void)written;
		_exit(125);
	}
	perror(s);
	exit(1);
}
static void bad(const char *s)
{
	fprintf(stderr, "geranium: %s\n", s);
	exit(1);
}
static void must(int v, const char *s)
{
	if (v < 0)
		die(s);
}
static int directory(int fd, const char *name)
{
	return openat(fd, name,
		      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
static int name_ok(const char *s)
{
	size_t n = strlen(s);
	if (!n || n > 63 || s[0] == '.' || s[0] == '-')
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '-' || *s == '_'))
			return 0;
	return 1;
}
/* Traverse every component with O_NOFOLLOW; never follow an ancestor symlink. */
static int path_dir(const char *path, int create)
{
	if (path[0] != '/')
		bad("paths must be absolute");
	int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC), next;
	if (fd < 0)
		die("open /");
	char *copy = strdup(path), *p = copy;
	if (!copy)
		die("strdup");
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		char *end = p;
		while (*end && *end != '/')
			end++;
		char save = *end;
		*end = 0;
		if (!strcmp(p, ".") || !strcmp(p, ".."))
			bad("dot path components forbidden");
		if (create && mkdirat(fd, p, 0700) < 0 && errno != EEXIST)
			die("mkdir state");
		next = directory(fd, p);
		if (next < 0)
			die("open path component (no symlinks)");
		close(fd);
		fd = next;
		*end = save;
		p = end;
	}
	free(copy);
	return fd;
}
/* Compare inode identities while walking state upwards; lexical prefixes are insufficient. */
static int ancestor_of(int source, int state)
{
	struct stat src, at, parent;
	if (fstat(source, &src) < 0)
		die("stat source");
	int cur = dup(state);
	if (cur < 0)
		die("dup state");
	for (;;) {
		if (fstat(cur, &at) < 0)
			die("stat ancestor");
		if (src.st_dev == at.st_dev && src.st_ino == at.st_ino) {
			close(cur);
			return 1;
		}
		int up = directory(cur, "..");
		if (up < 0)
			die("open ancestor");
		if (fstat(up, &parent) < 0)
			die("stat parent");
		if (parent.st_dev == at.st_dev && parent.st_ino == at.st_ino) {
			close(up);
			break;
		}
		close(cur);
		cur = up;
	}
	close(cur);
	return 0;
}
static void copy_identity(const struct stat *st, int image)
{
	if (st->st_uid != geteuid() || st->st_gid != getegid() ||
	    (!image && st->st_nlink > 1) || (st->st_mode & 07000))
		bad("unsupported rootfs ownership, hardlink, or special mode");
}
struct copy_link {
	dev_t dev;
	ino_t ino;
	char *path;
	struct copy_link *next;
};
static struct copy_link *copy_links[4096];
static size_t copy_link_bytes;
static unsigned link_bucket(dev_t dev, ino_t ino)
{
	return (unsigned)(((unsigned long long)dev ^ (unsigned long long)ino) %
			  4096);
}
static void copytree(int src, int dst, unsigned depth, unsigned *count,
		     int image, int destination_root, const char *prefix)
{
	if (depth > 32)
		bad("rootfs too deep");
	DIR *dir = fdopendir(dup(src));
	if (!dir)
		die("fdopendir");
	struct dirent *ent;
	for (;;) {
		errno = 0;
		ent = readdir(dir);
		if (!ent) {
			if (errno)
				die("read source directory");
			break;
		}
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		if (++*count > 100000)
			bad("rootfs too many entries");
		struct stat st;
		if (fstatat(src, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			die("stat rootfs");
		if (st.st_uid != geteuid() || st.st_gid != getegid() ||
		    (st.st_mode &
		     (image && S_ISDIR(st.st_mode) ? 06000 : 07000)))
			bad("unsupported rootfs identity or special mode");
		if (S_ISDIR(st.st_mode)) {
			must(mkdirat(dst, ent->d_name, 0700),
			     "mkdir copied directory");
			int a = directory(src, ent->d_name),
			    b = directory(dst, ent->d_name);
			if (a < 0 || b < 0)
				die("open copied directory");
			struct stat actual;
			if (fstat(a, &actual) < 0 || !S_ISDIR(actual.st_mode) ||
			    actual.st_dev != st.st_dev ||
			    actual.st_ino != st.st_ino)
				bad("source directory changed");
			char subpath[PATH_MAX];
			if (snprintf(subpath, sizeof subpath, "%s%s/", prefix,
				     ent->d_name) >= (int)sizeof subpath)
				bad("copy path too long");
			copytree(a, b, depth + 1, count, image,
				 destination_root, subpath);
			must(close(a), "close source directory");
			must(close(b), "close copied directory");
			must(fchmodat(dst, ent->d_name,
				      st.st_mode & (image ? 01777 : 0777), 0),
			     "chmod directory");
		} else if (S_ISREG(st.st_mode)) {
			if (st.st_size > 1024LL * 1024 * 1024)
				bad("file larger than 1 GiB");
			int a = openat(src, ent->d_name,
				       O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
					       O_NONBLOCK);
			if (a < 0)
				die("open copied source");
			struct stat actual;
			if (fstat(a, &actual) < 0 || !S_ISREG(actual.st_mode) ||
			    actual.st_dev != st.st_dev ||
			    actual.st_ino != st.st_ino)
				bad("source file changed");
			copy_identity(&actual, image);
			if (actual.st_size > 1024LL * 1024 * 1024)
				bad("file larger than 1 GiB");
			unsigned bucket =
				link_bucket(actual.st_dev, actual.st_ino);
			if (image && actual.st_nlink > 1) {
				struct copy_link *known = copy_links[bucket];
				while (known && (known->dev != actual.st_dev ||
						 known->ino != actual.st_ino))
					known = known->next;
				if (known) {
					must(linkat(destination_root,
						    known->path, dst,
						    ent->d_name, 0),
					     "copy image hardlink");
					must(close(a), "close linked source");
					continue;
				}
				struct copy_link *record =
					malloc(sizeof *record);
				if (!record)
					die("copy link allocation");
				char relative[PATH_MAX];
				if (snprintf(relative, sizeof relative, "%s%s",
					     prefix, ent->d_name) >=
				    (int)sizeof relative)
					bad("copy path too long");
				copy_link_bytes +=
					strlen(relative) + 1 + sizeof *record;
				if (copy_link_bytes > 16 * 1024 * 1024)
					bad("image hardlink map too large");
				record->dev = actual.st_dev;
				record->ino = actual.st_ino;
				record->path = strdup(relative);
				if (!record->path)
					die("copy link path");
				record->next = copy_links[bucket];
				copy_links[bucket] = record;
			}
			int b = openat(dst, ent->d_name,
				       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
				       0600);
			if (b < 0)
				die("open copied file");
			char buf[32768];
			ssize_t n;
			unsigned long long total = 0;
			while ((n = read(a, buf, sizeof buf)) > 0) {
				total += (unsigned long long)n;
				if (total > 1024ULL * 1024 * 1024)
					bad("file grew beyond 1 GiB");
				ssize_t off = 0;
				while (off < n) {
					ssize_t k = write(b, buf + off,
							  (size_t)(n - off));
					if (k < 0)
						die("copy write");
					off += k;
				}
			}
			if (n < 0)
				die("copy read");
			must(fchmod(b, st.st_mode & 0777), "chmod file");
			must(close(a), "close copied source");
			must(close(b), "close copied file");
		} else if (S_ISLNK(st.st_mode)) {
			char target[PATH_MAX];
			ssize_t n = readlinkat(src, ent->d_name, target,
					       sizeof target - 1);
			if (n < 0 || n == (ssize_t)sizeof target - 1)
				bad("invalid rootfs symlink");
			target[n] = 0;
			must(symlinkat(target, dst, ent->d_name),
			     "copy symlink");
		} else
			bad("rootfs special files are not supported");
	}
	must(closedir(dir), "close directory enumeration");
}
static void remove_tree(int fd)
{
	must(fchmod(fd, 0700), "chmod removable directory");
	DIR *dir = fdopendir(dup(fd));
	if (!dir)
		die("fdopendir remove");
	struct dirent *ent;
	for (;;) {
		errno = 0;
		ent = readdir(dir);
		if (!ent) {
			if (errno)
				die("read removable directory");
			break;
		}
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		struct stat st;
		if (fstatat(fd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			die("stat remove");
		if (S_ISDIR(st.st_mode)) {
			must(fchmodat(fd, ent->d_name, 0700, 0),
			     "chmod removable child");
			int sub = directory(fd, ent->d_name);
			if (sub < 0)
				die("open remove");
			remove_tree(sub);
			must(close(sub), "close removable child");
			must(unlinkat(fd, ent->d_name, AT_REMOVEDIR), "rmdir");
		} else
			must(unlinkat(fd, ent->d_name, 0), "unlink");
	}
	must(closedir(dir), "close directory enumeration");
}
static void write_all(int fd, const void *p, size_t n)
{
	const char *s = p;
	while (n) {
		ssize_t k = write(fd, s, n);
		if (k < 0)
			die("write");
		s += k;
		n -= (size_t)k;
	}
}
static int file(int dir, const char *name, int flags, mode_t mode)
{
	return openat(dir, name, flags | O_CLOEXEC | O_NOFOLLOW, mode);
}
static int status_replace(int dir, const char *status)
{
	if (unlinkat(dir, "status.tmp", 0) < 0 && errno != ENOENT)
		return -1;
	int fd = file(dir, "status.tmp", O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
		return -1;
	write_all(fd, status, strlen(status));
	if (fsync(fd) < 0) {
		int error = errno;
		(void)close(fd);
		errno = error;
		return -1;
	}
	if (close(fd) < 0)
		return -1;
	if (renameat(dir, "status.tmp", dir, "status") < 0)
		return -1;
	return fsync(dir);
}
static void status_write(int dir, const char *status)
{
	if (status_replace(dir, status) < 0) {
		int error = errno;
		/* Init still owns runtime: no client may accept a result until this
         * error cleanup finishes and the process releases its lease. */
		if (manager_diagnostic && !strcmp(status, "exited\n")) {
			if (unlinkat(dir, "exitcode", 0) < 0 && errno != ENOENT)
				die("remove failed result");
		}
		errno = error;
		die("publish status");
	}
}
static void status_read(int dir, char *out, size_t cap)
{
	int fd = file(dir, "status", O_RDONLY, 0);
	if (fd < 0)
		die("read status");
	ssize_t n = read(fd, out, cap - 1);
	if (n < 0 || n == (ssize_t)cap - 1)
		bad("invalid status");
	out[n] = 0;
	must(close(fd), "close status read");
}
/* Stable name identity across staging, removal and recreation. Anchors are
 * retained: unlinking one could split waiters and new commands across inodes.
 * The caller validates the private state root and the name before opening it. */
static int command_lock(int root, const char *name)
{
	char anchor[96];
	if (snprintf(anchor, sizeof anchor, ".command-%s.lock", name) >=
	    (int)sizeof anchor)
		bad("name too long");
	int fd = file(root, anchor, O_RDWR | O_CREAT | O_NONBLOCK, 0600);
	if (fd < 0)
		die("command lock");
	struct stat st;
	if (fstat(fd, &st) < 0)
		die("stat command lock");
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0077) || st.st_nlink != 1)
		bad("command lock must be a private owned regular file with one link");
	struct flock lock = { 0 };
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	must(fcntl(fd, F_SETLKW, &lock), "command lock");
	return fd;
}
#define LOG_LIMIT (1024U * 1024U)
static long long monotonic_ms(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		die("clock");
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int outcome(int status)
{
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
static void code_write(int dir, int code)
{
	char text[16];
	int n = snprintf(text, sizeof text, "%d\n", code);
	int fd = file(dir, "exitcode.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		die("exitcode tmp");
	write_all(fd, text, (size_t)n);
	must(fsync(fd), "sync exitcode");
	must(close(fd), "close exitcode");
	must(renameat(dir, "exitcode.tmp", dir, "exitcode"),
	     "publish exitcode");
}
static int code_read(int dir)
{
	char text[16] = { 0 };
	int fd = file(dir, "exitcode", O_RDONLY, 0);
	if (fd < 0)
		die("exitcode");
	ssize_t n = read(fd, text, sizeof text - 1);
	if (n < 0)
		die("read exitcode");
	must(close(fd), "close exitcode read");
	if (n < 2 || n > 4 || text[n - 1] != '\n')
		bad("invalid exitcode");
	char *end;
	long code = strtol(text, &end, 10);
	if (*end != '\n' || code < 0 || code > 255)
		bad("invalid exitcode");
	return (int)code;
}
/* The runtime lock is owned only by namespace init. */
static int runtime_open(int container)
{
	int fd = file(container, "runtime", O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		die("runtime lock");
	return fd;
}
static int runtime_pid(int fd)
{
	struct flock l = { 0 };
	l.l_type = F_WRLCK;
	l.l_whence = SEEK_SET;
	must(fcntl(fd, F_GETLK, &l), "runtime identity");
	return l.l_type == F_UNLCK ? 0 : (int)l.l_pid;
}
static int attachment_open(int container)
{
	int fd = file(container, "attachment", O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		die("foreground attachment");
	return fd;
}
/* Private stages may be incomplete: never require status to reclaim them.
 * The name lock excludes all cooperating starters; retain the original runtime
 * and attachment inode checks before deleting any stage contents. */
static void reclaim_stage(int state, const char *name)
{
	struct stat st;
	if (fstatat(state, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		if (errno == ENOENT)
			return;
		die("check pending stage");
	}
	int stage = directory(state, name);
	if (stage < 0)
		die("open pending stage");
	struct stat actual;
	if (fstat(stage, &actual) < 0)
		die("stat pending stage");
	if (!S_ISDIR(st.st_mode) || actual.st_dev != st.st_dev ||
	    actual.st_ino != st.st_ino || actual.st_uid != geteuid() ||
	    (actual.st_mode & 0077))
		bad("unsafe pending stage");
	int runtime = runtime_open(stage), attachment = attachment_open(stage);
	if (runtime_pid(runtime))
		bad("pending stage runtime is still owned");
	if (runtime_pid(attachment))
		bad("pending stage foreground client is still attached");
	must(close(runtime), "close stage runtime");
	must(close(attachment), "close stage attachment");
	remove_tree(stage);
	must(close(stage), "close pending stage");
	must(unlinkat(state, name, AT_REMOVEDIR), "remove pending stage");
	must(fsync(state), "sync removed stage");
}
static void refresh(int container, char *status, size_t size)
{
	status_read(container, status, size);
	int fd = runtime_open(container), pid = runtime_pid(fd);
	close(fd);
	if (!pid &&
	    (!strcmp(status, "running\n") || !strcmp(status, "starting\n"))) {
		/* Init publishes its result before releasing runtime ownership. */
		status_read(container, status, size);
		if (!strcmp(status, "running\n") ||
		    !strcmp(status, "starting\n")) {
			/* An incomplete publication cannot prove the workload's outcome. */
			if (unlinkat(container, "exitcode", 0) < 0 &&
			    errno != ENOENT)
				die("remove exitcode");
			status_write(container, "exited\n");
			strcpy(status, "exited\n");
		}
	}
}
static volatile sig_atomic_t pending;
static void signal_pending(int sig)
{
	pending = sig;
}
static void namespace_init(int container, char **args, int log, int ack,
			   int owner, struct image_config *image)
{
	int runtime = runtime_open(container);
	struct flock l = { 0 };
	l.l_type = F_WRLCK;
	l.l_whence = SEEK_SET;
	must(fcntl(runtime, F_SETLK, &l), "own runtime");
	struct sigaction sa = { 0 };
	sa.sa_handler = signal_pending;
	sigemptyset(&sa.sa_mask);
	must(sigaction(SIGTERM, &sa, NULL), "TERM handler");
	must(sigaction(SIGINT, &sa, NULL), "INT handler");
	must(sigaction(SIGHUP, &sa, NULL), "HUP handler");
	signal(SIGPIPE, SIG_IGN);
	if (signal(SIGCHLD, SIG_DFL) == SIG_ERR)
		die("CHLD disposition");
	sigset_t unblocked;
	sigemptyset(&unblocked);
	must(sigprocmask(SIG_SETMASK, &unblocked, NULL), "init signal mask");
	int output[2], execack[2];
	must(pipe(output), "output");
	must(pipe(execack), "exec ack");
	must(fcntl(execack[1], F_SETFD, FD_CLOEXEC), "exec cloexec");
	must(fcntl(execack[0], F_SETFD, FD_CLOEXEC), "exec read cloexec");
	must(fcntl(output[0], F_SETFD, FD_CLOEXEC), "output read cloexec");
	must(fcntl(output[1], F_SETFD, FD_CLOEXEC), "output write cloexec");
	pid_t child = fork();
	if (child < 0)
		die("fork payload");
	if (!child) {
		must(backend_exec_prepare(), "exec readiness ptrace prepare");
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		must(setpgid(0, 0), "payload group");
		must(dup2(output[1], 1), "stdout");
		must(dup2(output[1], 2), "stderr");
		/* Bootstrap already removed arbitrary inherited descriptors. Close the
         * known manager descriptors before exec (including failed exec paths). */
		close(output[0]);
		close(output[1]);
		close(execack[0]);
		close(container);
		close(log);
		close(runtime);
		close(ack);
		if (owner >= 0)
			close(owner);
		char *legacy_env[] = { "PATH=/bin:/usr/bin", NULL };
		char **env = image ? image->env : legacy_env;
		if (image && chdir(image->workdir) < 0) {
			perror("image WorkingDir");
			int code = 125;
			write_all(execack[1], &code, sizeof code);
			_exit(code);
		}
		if (args[0][0] == '/')
			execve(args[0], args, env);
		else {
			const char *path = NULL;
			for (unsigned i = 0; env[i]; i++)
				if (!strncmp(env[i], "PATH=", 5))
					path = env[i] + 5;
			if (!path)
				errno = ENOENT;
			else {
				char paths[IMAGE_BYTES];
				if (strlen(path) >= sizeof paths)
					bad("image PATH too large");
				strcpy(paths, path);
				int denied = 0;
				char *p = paths;
				for (;;) {
					char *end = strchr(p, ':');
					if (end)
						*end = 0;
					const char *prefix = *p ? p : ".";
					char executable[PATH_MAX];
					int n = snprintf(executable,
							 sizeof executable,
							 "%s/%s", prefix,
							 args[0]);
					if (n > 0 &&
					    (size_t)n < sizeof executable) {
						execve(executable, args, env);
						if (errno == EACCES)
							denied = 1;
						else if (errno != ENOENT &&
							 errno != ENOTDIR)
							break;
					}
					if (!end) {
						errno = denied ? EACCES :
								 ENOENT;
						break;
					}
					p = end + 1;
				}
			}
		}
		int code = errno == ENOENT ? 127 : errno == EACCES ? 126 : 125;
		perror(args[0]);
		write_all(execack[1], &code, sizeof code);
		_exit(code);
	}
	close(output[1]);
	close(execack[1]);
	int status = 0, finished = 0;
	int ready = backend_exec_wait((int)child, &status, owner, ack,
				      monotonic_ms() + 8000, &pending);
	if (ready < 0) {
		perror("exec readiness ptrace wait");
		/* Still our unreaped child, so no recycled process can be signalled. */
		(void)kill(child, SIGKILL);
		pid_t got;
		do {
			got = waitpid(child, &status, 0);
			if (got == child && WIFSTOPPED(status))
				(void)kill(child, SIGKILL);
		} while ((got < 0 && errno == EINTR) ||
			 (got == child && WIFSTOPPED(status)));
		if (got < 0)
			die("reap failed readiness");
	}
	if (ready != 1)
		finished = 1;
	int code = 0;
	ssize_t n;
	do {
		n = read(execack[0], &code, sizeof code);
	} while (n < 0 && errno == EINTR);
	close(execack[0]);
	if (n != 0 && n != (ssize_t)sizeof code)
		code = 125;
	if (ready != 1 && !code)
		code = ready < 0 ? 125 : outcome(status);
	if (ready != 1 && !code)
		code = 125; /* Even a clean pre-exec exit is failure. */
	if (!code)
		status_write(container, "running\n");
	unsigned char reply = (unsigned char)code;
	if (!code) {
		write_all(ack, &reply, 1);
		close(ack);
	}
	must(fcntl(output[0], F_SETFL, O_NONBLOCK), "output nonblocking");
	size_t logged = 0;
	int eof = 0;
	long long deadline = 0;
	for (;;) {
		struct pollfd fds[2] = { { .fd = eof ? -1 : output[0],
					   .events = POLLIN | POLLHUP },
					 { .fd = owner, .events = POLLHUP } };
		if (poll(fds, 2, 20) < 0 && errno != EINTR)
			die("init poll");
		if (fds[1].revents & POLLHUP) {
			close(owner);
			owner = -1;
			pending = SIGTERM;
			deadline = monotonic_ms() + 2000;
		}
		if (pending) {
			(void)kill(-child, pending);
			pending = 0;
		}
		if (deadline && monotonic_ms() >= deadline) {
			(void)kill(-1, SIGKILL);
			deadline = 0;
		}
		if (!finished) {
			pid_t got;
			while ((got = waitpid(-1, &status, WNOHANG)) > 0)
				if (got == child) {
					finished = 1;
					break;
				}
		}
		if (finished)
			(void)kill(-1, SIGKILL);
		if (!eof)
			for (int tick = 0; tick < 8; tick++) {
				char buf[8192];
				n = read(output[0], buf, sizeof buf);
				if (n > 0) {
					size_t room = LOG_LIMIT - logged,
					       keep = (size_t)n < room ?
							      (size_t)n :
							      room;
					if (keep) {
						write_all(log, buf, keep);
						logged += keep;
					}
				} else {
					if (n == 0)
						eof = 1;
					else if (errno != EAGAIN &&
						 errno != EINTR)
						die("log read");
					break;
				}
			}
		if (finished && eof)
			break;
	}
	must(close(log), "close completed log");
	code_write(container, outcome(status));
	status_write(container, "exited\n");
	/* Keep runtime owned until all publication and reply operations finish. */
	if (code) {
		/* A cancelled caller has no reader. Failure is already published and
         * runtime ownership retained until exit; EPIPE is expected abandonment. */
		ssize_t sent;
		do {
			sent = write(ack, &reply, 1);
		} while (sent < 0 && errno == EINTR);
		if (sent < 0 && errno != EPIPE)
			die("startup failure reply");
		close(ack);
	}
	_exit(0);
}
static int follow(int container, int owner, int state, const char *name)
{
	int log = file(container, "log", O_RDONLY, 0);
	if (log < 0)
		die("open log");
	int runtime = runtime_open(container);
	for (;;) {
		char buffer[4096];
		ssize_t n = read(log, buffer, sizeof buffer);
		if (n < 0)
			die("read log");
		if (n > 0) {
			write_all(STDOUT_FILENO, buffer, (size_t)n);
			continue;
		}
		char status[64];
		status_read(container, status, sizeof status);
		if (!runtime_pid(runtime)) {
			n = read(log, buffer, sizeof buffer);
			if (n < 0)
				die("read final log");
			if (n > 0) {
				write_all(1, buffer, (size_t)n);
				continue;
			}
			break;
		}
		struct timespec pause = { .tv_sec = 0, .tv_nsec = 50000000 };
		nanosleep(&pause, NULL);
	}
	must(close(runtime), "close follow runtime");
	must(close(log), "close follow log");
	/* Attachment fences this generation. Take the same name lock as inspect
     * and stop only after draining output, so backpressure holds no name lock. */
	int lock = command_lock(state, name);
	char status[64];
	refresh(container, status, sizeof status);
	int result = 125;
	int fd = file(container, "exitcode", O_RDONLY, 0);
	if (fd < 0) {
		if (errno != ENOENT)
			die("open exitcode");
	} else {
		must(close(fd), "close result probe");
		if (!strcmp(status, "exited\n"))
			result = code_read(container);
	}
	must(close(lock), "close completion lock");
	must(close(owner), "close owner");
	return result;
}
static int start(int container, const char *rootfs, int detached, int *lockfd,
		 int statefd, const char *name)
{
	if (!backend_supported())
		bad("isolation unsupported on this platform");
	int occupied = runtime_open(container);
	if (runtime_pid(occupied))
		bad("container runtime is still owned");
	close(occupied);
	int attachment = attachment_open(container);
	if (runtime_pid(attachment))
		bad("previous foreground client is still attached");
	if (!detached) {
		struct flock l = { 0 };
		l.l_type = F_WRLCK;
		l.l_whence = SEEK_SET;
		must(fcntl(attachment, F_SETLK, &l),
		     "own foreground attachment");
	}
	char state[64];
	refresh(container, state, sizeof state);
	if (strcmp(state, "created\n") && strcmp(state, "exited\n"))
		bad("container is not stopped");
	int command = file(container, "command", O_RDONLY, 0);
	if (command < 0)
		die("open command");
	char data[MAX_BYTES + 1];
	ssize_t n = read(command, data, MAX_BYTES + 1);
	close(command);
	if (n <= 0 || n > MAX_BYTES || data[n - 1] != 0)
		bad("invalid command record");
	char *args[MAX_ARGS + 1];
	int count = 0;
	for (ssize_t j = 0; j < n;) {
		if (count == MAX_ARGS)
			bad("too many saved arguments");
		args[count++] = data + j;
		size_t len = strnlen(data + j, (size_t)(n - j));
		if (j + (ssize_t)len >= n)
			bad("invalid argument");
		j += (ssize_t)len + 1;
	}
	args[count] = NULL;
	struct image_config image;
	int has_image = image_config_load(container, &image);
	if (args[0][0] != '/' &&
	    (!has_image || strchr(args[0], '/') || !*args[0]))
		bad("command must be absolute inside rootfs");
	int log = file(container, "log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (log < 0)
		die("open log");
	int ack[2], owner[2];
	must(pipe(ack), "startup pipe");
	if (!detached)
		must(pipe(owner), "owner pipe");
	if (unlinkat(container, "exitcode", 0) < 0 && errno != ENOENT)
		die("remove exitcode");
	int caller_pid = (int)getpid();
	pid_t launcher = fork();
	if (launcher < 0)
		die("fork launcher");
	if (launcher == 0) {
		must(backend_watch_parent(caller_pid), "watch launcher parent");
		must(setsid(), "detach launcher session");
		close(ack[0]);
		if (!detached)
			close(owner[1]);
		close(*lockfd);
		close(statefd);
		int null = open("/dev/null", O_RDWR);
		if (null < 0)
			die("/dev/null");
		must(dup2(null, 0), "stdin");
		must(dup2(null, 1), "init stdout");
		must(dup2(log, 2), "init stderr");
		manager_diagnostic = 1;
		must(fcntl(ack[1], F_SETFD, FD_CLOEXEC), "startup cloexec");
		if (!detached)
			must(fcntl(owner[0], F_SETFD, FD_CLOEXEC),
			     "owner cloexec");
		if (null > 2)
			close(null);
		int keep[] = { container, log, ack[1],
			       detached ? -1 : owner[0] };
		must(backend_close_inherited(keep, 4),
		     "close inherited descriptors");
		int result = backend_enter(rootfs);
		if (result < 0) {
			unsigned char code = 125;
			write_all(ack[1], &code, 1);
			_exit(125);
		}
		if (result > 0)
			_exit(0);
		namespace_init(container, args, log, ack[1],
			       detached ? -1 : owner[0],
			       has_image ? &image : NULL);
		_exit(125);
	}
	close(log);
	close(ack[1]);
	if (!detached)
		close(owner[0]);
	int ignored;
	while (waitpid(launcher, &ignored, 0) < 0 && errno == EINTR) {
	}
	struct pollfd startup = { .fd = ack[0], .events = POLLIN | POLLHUP };
	if (poll(&startup, 1, 12000) <= 0)
		bad("namespace init startup timed out");
	unsigned char code = 125;
	if (read(ack[0], &code, 1) != 1)
		code = 125;
	close(ack[0]);
	close(*lockfd);
	*lockfd = -1;
	if (code) {
		fprintf(stderr, "geranium: startup failed with status %u\n",
			(unsigned)code);
		int diagnostic = file(container, "log", O_RDONLY, 0);
		if (diagnostic >= 0) {
			char buf[4096];
			ssize_t bytes;
			while ((bytes = read(diagnostic, buf, sizeof buf)) > 0)
				write_all(2, buf, (size_t)bytes);
			close(diagnostic);
		}
		close(attachment);
		if (!detached)
			close(owner[1]);
		return code;
	}
	if (detached) {
		close(attachment);
		strerr_warnn(2, "geranium: ", "started (detached)");
		return 0;
	}
	strerr_warnn(2, "geranium: ", "started");
	int result = follow(container, owner[1], statefd, name);
	must(close(attachment), "close foreground attachment");
	return result;
}
/* Direct calls must restore closed stdio before opening manager descriptors. */
static void restore_stdio(void)
{
	for (int fd = 0; fd < 3; fd++) {
		if (fcntl(fd, F_GETFD) >= 0)
			continue;
		if (errno != EBADF)
			die("check standard descriptor");
		int null = open("/dev/null", O_RDWR);
		if (null < 0)
			die("open standard descriptor");
		if (null != fd) {
			must(dup2(null, fd), "restore standard descriptor");
			must(close(null), "close standard descriptor");
		}
	}
}
static int state_open(const char *root)
{
	restore_stdio();
	if (!root)
		bad("root path required");
	int state = path_dir(root, 1);
	struct stat st;
	if (fstat(state, &st) < 0 || st.st_uid != geteuid() ||
	    (st.st_mode & 0077))
		bad("state directory must be owned by you and mode 0700");
	return state;
}
int geranium_open_state_root(const char *root)
{
	return state_open(root);
}

static void require_name(const char *name)
{
	if (!name || !name_ok(name))
		bad("invalid container name");
}
static int create_or_run(const char *root, enum geranium_source source_type,
			 const char *source, const char *name,
			 int command_count, const char *const command[],
			 bool run, bool detached)
{
	if (source_type != GERANIUM_SOURCE_ROOTFS &&
	    source_type != GERANIUM_SOURCE_IMAGE)
		bad("invalid source type");
	if (!source)
		bad("source required");
	require_name(name);
	if (command_count > MAX_ARGS)
		bad("too many arguments");
	if (command_count < 0 || (command_count && !command))
		bad("invalid arguments");
	for (int j = 0; j < command_count; j++)
		if (!command[j])
			bad("null command argument");
	int state = state_open(root), lock;
	int from_image = source_type == GERANIUM_SOURCE_IMAGE;
	int image_lock = -1;
	struct image_config config;
	int src;
	if (from_image)
		src = image_open(state, source, &image_lock, &config);
	else {
		src = path_dir(source, 0);
		if (ancestor_of(src, state))
			bad("rootfs encloses state directory");
	}
	const char *const *args = command;
	int nargs = command_count;
	if (!nargs && from_image) {
		args = (const char *const *)config.args;
		while (args[nargs]) {
			if (nargs >= MAX_ARGS)
				bad("too many arguments");
			nargs++;
		}
	}
	if (!nargs)
		bad("command required");
	if (args[0][0] != '/' &&
	    (!from_image || command_count || strchr(args[0], '/') || !*args[0]))
		bad("command must be absolute inside rootfs");
	size_t bytes = 0;
	for (int j = 0; j < nargs; j++)
		if ((bytes += strlen(args[j]) + 1) > MAX_BYTES)
			bad("command too long");
	struct stat srcst;
	if (fstat(src, &srcst) < 0)
		die("stat rootfs");
	if (srcst.st_uid != geteuid() || srcst.st_gid != getegid() ||
	    (srcst.st_mode & (from_image ? 06000 : 07000)))
		bad("unsupported rootfs directory identity or mode");
	lock = command_lock(state, name);
	char pending[80];
	if (snprintf(pending, sizeof pending, ".pending-%s", name) >=
	    (int)sizeof pending)
		bad("name too long");
	struct stat exists;
	if (fstatat(state, name, &exists, AT_SYMLINK_NOFOLLOW) == 0 ||
	    errno != ENOENT)
		bad("container already exists or cannot check name");
	reclaim_stage(state, pending);
	if (mkdirat(state, pending, 0700) < 0)
		die("stage container");
	int container = directory(state, pending);
	if (container < 0)
		die("open stage");
	must(mkdirat(container, "rootfs", 0700), "mkdir rootfs");
	int dst = directory(container, "rootfs");
	if (dst < 0)
		die("open destination");
	unsigned count = 0;
	copytree(src, dst, 0, &count, from_image, dst, "");
	must(close(src), "close source rootfs");
	must(close(dst), "close copied rootfs");
	must(fchmodat(container, "rootfs",
		      srcst.st_mode & (from_image ? 01777 : 0777), 0),
	     "chmod copied rootfs");
	int cmd = file(container, "command", O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (cmd < 0)
		die("create command");
	for (int j = 0; j < nargs; j++)
		write_all(cmd, args[j], strlen(args[j]) + 1);
	must(fsync(cmd), "sync command");
	must(close(cmd), "close command");
	if (from_image) {
		image_config_save(container, &config);
		free(config.source);
		must(close(image_lock), "close image copy lock");
	}
	status_write(container, "created\n");
	must(fsync(container), "sync staged container");
	must(renameat(state, pending, state, name), "publish container");
	must(fsync(state), "sync published container");
	if (run) {
		char path[PATH_MAX];
		if (snprintf(path, sizeof path, "%s/%s/rootfs", root, name) >=
		    (int)sizeof path)
			bad("state path too long");
		int result =
			start(container, path, detached, &lock, state, name);
		must(close(container), "close run container");
		if (lock >= 0)
			must(close(lock), "close run lock");
		must(close(state), "close run state");
		return result;
	}
	close(container);
	close(lock);
	close(state);
	strerr_warnn(2, "geranium: ", "created");
	return 0;
}
int geranium_create(const char *root, enum geranium_source source_type,
		    const char *source, const char *name, int command_count,
		    const char *const command[])
{
	return create_or_run(root, source_type, source, name, command_count,
			     command, false, false);
}
int geranium_run(const char *root, enum geranium_source source_type,
		 const char *source, const char *name, int command_count,
		 const char *const command[], bool detached)
{
	return create_or_run(root, source_type, source, name, command_count,
			     command, true, detached);
}
int geranium_pull(const char *root, const char *reference, bool http_loopback)
{
	if (!reference)
		bad("reference required");
	return image_pull(state_open(root), reference, http_loopback);
}
int geranium_rmi(const char *root, const char *reference)
{
	if (!reference)
		bad("reference required");
	return image_remove(state_open(root), reference);
}
int geranium_logs(const char *root, const char *name)
{
	require_name(name);
	int state = state_open(root);
	int lock = command_lock(state, name);
	int container = directory(state, name);
	if (container < 0)
		die("open container");
	int log = file(container, "log", O_RDONLY, 0);
	if (log < 0)
		die("open log");
	close(lock);
	char buf[4096];
	ssize_t n;
	while ((n = read(log, buf, sizeof buf)) > 0)
		write_all(STDOUT_FILENO, buf, (size_t)n);
	if (n < 0)
		die("read log");
	close(log);
	close(container);
	close(state);
	return 0;
}
int geranium_start(const char *root, const char *name, bool detached)
{
	require_name(name);
	int state = state_open(root);
	int lock = command_lock(state, name);
	int container = directory(state, name);
	if (container < 0)
		die("open container");
	char path[PATH_MAX];
	if (snprintf(path, sizeof path, "%s/%s/rootfs", root, name) >=
	    (int)sizeof path)
		bad("state path too long");
	int result = start(container, path, detached, &lock, state, name);
	must(close(container), "close start container");
	if (lock >= 0)
		must(close(lock), "close start lock");
	must(close(state), "close start state");
	return result;
}
int geranium_stop(const char *root, const char *name)
{
	require_name(name);
	int state = state_open(root);
	int lock = command_lock(state, name);
	int container = directory(state, name);
	if (container < 0)
		die("open container");
	int runtime = runtime_open(container), pid = runtime_pid(runtime);
	if (!pid)
		bad("container is not running");
	int handle = backend_pidfd_open(pid);
	if (handle < 0 || runtime_pid(runtime) != pid)
		bad("runtime identity changed");
	if (backend_signal(handle, SIGTERM) < 0 && errno != ESRCH)
		die("signal init TERM");
	long long deadline = monotonic_ms() + 2000;
	while (runtime_pid(runtime) == pid && monotonic_ms() < deadline) {
		struct timespec pause = { 0, 20000000 };
		nanosleep(&pause, NULL);
	}
	if (runtime_pid(runtime) == pid &&
	    backend_signal(handle, SIGKILL) < 0 && errno != ESRCH)
		die("signal init KILL");
	deadline = monotonic_ms() + 2000;
	while (runtime_pid(runtime) == pid && monotonic_ms() < deadline) {
		struct timespec pause = { 0, 20000000 };
		nanosleep(&pause, NULL);
	}
	if (runtime_pid(runtime))
		bad("stop timed out");
	close(handle);
	close(runtime);
	char status[64];
	refresh(container, status, sizeof status);
	close(container);
	close(lock);
	close(state);
	strerr_warnn(2, "geranium: ", "stopped");
	return 0;
}
static int inspect_or_rm(const char *root, const char *name, bool remove)
{
	require_name(name);
	int state = state_open(root);
	int lock = command_lock(state, name);
	char stage[80];
	if (snprintf(stage, sizeof stage, ".pending-%s", name) >=
	    (int)sizeof stage)
		bad("name too long");
	int container = directory(state, name);
	if (container < 0) {
		if (errno != ENOENT || !remove)
			die("open container");
		struct stat st;
		if (fstatat(state, stage, &st, AT_SYMLINK_NOFOLLOW) < 0)
			die("open pending removal");
		reclaim_stage(state, stage);
		must(close(lock), "close command lock");
		must(close(state), "close state");
		strerr_warnn(2, "geranium: ", "removed pending container");
		return 0;
	}
	char status[64];
	refresh(container, status, sizeof status);
	if (!remove) {
		if (printf("name=%s\nstatus=%s", name, status) < 0 ||
		    fflush(stdout) == EOF)
			die("write inspect");
		close(container);
		close(lock);
		close(state);
		return 0;
	}
	int occupied = runtime_open(container);
	if (runtime_pid(occupied))
		bad("container runtime is still owned");
	must(close(occupied), "close removal runtime");
	if (!strcmp(status, "running\n") || !strcmp(status, "starting\n"))
		bad("container is running or starting");
	int attachment = attachment_open(container);
	if (runtime_pid(attachment))
		bad("foreground client is still attached");
	must(close(attachment), "close removal attachment");
	reclaim_stage(state, stage);
	/* Atomic unpublication precedes destructive cleanup. */
	must(renameat(state, name, state, stage), "unpublish container");
	must(fsync(state), "sync unpublished container");
	remove_tree(container);
	must(close(container), "close removed container");
	must(unlinkat(state, stage, AT_REMOVEDIR), "remove container stage");
	must(fsync(state), "sync removed container");
	must(close(lock), "close command lock");
	must(close(state), "close state");
	strerr_warnn(2, "geranium: ", "removed container");
	return 0;
}
int geranium_inspect(const char *root, const char *name)
{
	return inspect_or_rm(root, name, false);
}
int geranium_rm(const char *root, const char *name)
{
	return inspect_or_rm(root, name, true);
}
