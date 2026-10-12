#define _GNU_SOURCE
#include "backend.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sched.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

/* One fatal manager diagnostic, capped at 512 bytes including newline. */
static void fail(const char *message)
{
	char buffer[512];
	int n = snprintf(buffer, sizeof buffer, "%s: %s", message,
			 strerror(errno));
	size_t size = n < 0 ? 0 : (size_t)n;
	if (size >= sizeof buffer)
		size = sizeof buffer - 1;
	buffer[size++] = '\n';
	ssize_t written = write(2, buffer, size);
	(void)written;
	_exit(125);
}
static void must(int value, const char *message)
{
	if (value < 0)
		fail(message);
}
static void write_all(int fd, const char *data, size_t size)
{
	while (size) {
		ssize_t n = write(fd, data, size);
		if (n < 0)
			fail("write mapping");
		data += n;
		size -= (size_t)n;
	}
}
static void map_id(const char *path, unsigned outside)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		fail(path);
	char line[80];
	int n = snprintf(line, sizeof line, "0 %u 1\n", outside);
	write_all(fd, line, (size_t)n);
	close(fd);
}
/* Namespace PID 1 is both real parent and temporary tracer. Pipe EOF is
 * ambiguous; only PTRACE_EVENT_EXEC proves replacement of the image. */
int backend_exec_prepare(void)
{
	if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
		return -1;
	return raise(SIGSTOP);
}
static int exec_event(int child, int *status, int owner, int ack,
		      long long deadline, const volatile sig_atomic_t *cancel)
{
	for (;;) {
		struct pollfd fds[2] = { { .fd = owner, .events = POLLHUP },
					 { .fd = ack, .events = 0 } };
		if (poll(fds, 2, 0) < 0 && errno != EINTR)
			return -1;
		if (*cancel ||
		    (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) ||
		    (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
			errno = ECANCELED;
			return -1;
		}
		struct timespec t;
		if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
			return -1;
		long long now =
			(long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		pid_t got = waitpid(child, status, WNOHANG);
		if (got > 0)
			return 0;
		if (got < 0 && errno != EINTR)
			return -1;
		int delay = deadline - now < 20 ? (int)(deadline - now) : 20;
		if (poll(fds, 2, delay) < 0 && errno != EINTR)
			return -1;
	}
}
int backend_exec_wait(int child, int *status, int owner, int ack,
		      long long deadline, const volatile sig_atomic_t *cancel)
{
	if (exec_event(child, status, owner, ack, deadline, cancel) < 0)
		return -1;
	if (WIFEXITED(*status) || WIFSIGNALED(*status))
		return 0;
	if (!WIFSTOPPED(*status) || WSTOPSIG(*status) != SIGSTOP) {
		errno = EPROTO;
		return -1;
	}
	if (ptrace(PTRACE_SETOPTIONS, child, NULL,
		   (void *)(long)PTRACE_O_TRACEEXEC) < 0)
		return -1;
	if (ptrace(PTRACE_CONT, child, NULL, NULL) < 0)
		return -1;
	for (;;) {
		if (exec_event(child, status, owner, ack, deadline, cancel) < 0)
			return -1;
		if (WIFEXITED(*status) || WIFSIGNALED(*status))
			return 0;
		if (!WIFSTOPPED(*status)) {
			errno = EPROTO;
			return -1;
		}
		if ((unsigned)*status >> 8 ==
		    (SIGTRAP | (PTRACE_EVENT_EXEC << 8))) {
			if (ptrace(PTRACE_DETACH, child, NULL, NULL) < 0)
				return -1;
			return 1;
		}
		/* Preserve signals delivered before exec, including fatal signals. */
		if (ptrace(PTRACE_CONT, child, NULL,
			   (void *)(long)WSTOPSIG(*status)) < 0)
			return -1;
	}
}
int backend_pidfd_open(int pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}
int backend_signal(int pidfd, int signum)
{
	return (int)syscall(SYS_pidfd_send_signal, pidfd, signum, NULL, 0);
}
/* /proc enumeration handles inherited descriptors above a lowered RLIMIT. */
int backend_close_inherited(const int *keep, unsigned count)
{
	DIR *dir = opendir("/proc/self/fd");
	if (!dir)
		return -1;
	struct dirent *ent;
	for (;;) {
		errno = 0;
		ent = readdir(dir);
		if (!ent) {
			if (errno) {
				int error = errno;
				closedir(dir);
				errno = error;
				return -1;
			}
			break;
		}
		char *end;
		long fd = strtol(ent->d_name, &end, 10);
		if (*end || fd < 3 || fd == dirfd(dir))
			continue;
		unsigned i;
		for (i = 0; i < count; i++)
			if (fd == keep[i])
				break;
		if (i == count && close((int)fd) < 0) {
			int error = errno;
			closedir(dir);
			errno = error;
			return -1;
		}
	}
	return closedir(dir);
}
int backend_supported(void)
{
	int handle = backend_pidfd_open((int)getpid());
	if (handle < 0)
		return 0;
	int supported = backend_signal(handle, 0) == 0;
	close(handle);
	return supported;
}
int backend_watch_parent(int parent_pid)
{
	if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
		return -1;
	if (getppid() != parent_pid) {
		errno = ECANCELED;
		return -1;
	}
	return 0;
}
int backend_enter(const char *rootfs)
{
	uid_t uid = getuid();
	gid_t gid = getgid();
	must(unshare(CLONE_NEWUSER), "unshare user");
	int fd = open("/proc/self/setgroups", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		write_all(fd, "deny\n", 5);
		close(fd);
	}
	map_id("/proc/self/uid_map", (unsigned)uid);
	map_id("/proc/self/gid_map", (unsigned)gid);
	must(unshare(CLONE_NEWNS | CLONE_NEWPID), "unshare mount and pid");
	pid_t init = fork();
	if (init < 0)
		fail("fork namespace init");
	if (init)
		return 1;
	must(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL),
	     "private mounts");
	must(mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL),
	     "bind rootfs");
	must(chdir(rootfs), "chdir rootfs");
	/* Stack the old root on the new root, then detach it without scratch data. */
	must((int)syscall(SYS_pivot_root, ".", "."), "pivot_root");
	must(umount2(".", MNT_DETACH), "detach old root");
	must(chdir("/"), "chdir /");
	return 0;
}

const char *backend_image_arch(void)
{
#if defined(__x86_64__)
	return "amd64";
#elif defined(__aarch64__)
	return "arm64";
#else
	return NULL;
#endif
}
