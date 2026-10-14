#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include "fatal.h"
#include "json.h"
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#define HEAD_LIMIT 32768
#define IDLE_TIMEOUT 30000
#define BODY_TIMEOUT 300000
struct url {
	char host[256], port[8], path[4096], origin[300], authority[256];
	int tls;
};
static void fail(const char *s)
{
	fprintf(stderr, "geranium registry: %s\n", s);
	geranium_fatal_exit(1);
}
static long long now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
		fail("clock");
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static struct url parse(const char *s, int local)
{
	struct url u;
	memset(&u, 0, sizeof u);
	if (!strncmp(s, "https://", 8)) {
		u.tls = 1;
		s += 8;
		strcpy(u.port, "443");
	} else if (local && !strncmp(s, "http://", 7)) {
		s += 7;
		strcpy(u.port, "80");
	} else
		fail("HTTPS required (HTTP only for explicit loopback fixtures)");
	const char *slash = strchr(s, '/');
	size_t n = slash ? (size_t)(slash - s) : strlen(s);
	if (!n || n >= sizeof u.host)
		fail("invalid URL host");
	memcpy(u.host, s, n);
	u.host[n] = 0;
	/* Host uses the URL's spelling; connection/origin identity uses defaults. */
	strcpy(u.authority, u.host);
	char *p = strchr(u.host, ':');
	if (p) {
		*p++ = 0;
		if (!*p || strlen(p) >= sizeof u.port)
			fail("invalid port");
		strcpy(u.port, p);
	}
	unsigned port = 0;
	for (p = u.port; *p; p++) {
		if (*p < '0' || *p > '9')
			fail("invalid port");
		port = port * 10 + (unsigned)(*p - '0');
	}
	if (!port || port > 65535)
		fail("invalid port");
	if (!*u.host)
		fail("invalid host");
	for (p = u.host; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
			fail("invalid host");
	if (!u.tls && strcmp(u.host, "127.0.0.1"))
		fail("HTTP requires literal 127.0.0.1");
	if (!slash)
		slash = "/";
	if (strlen(slash) >= sizeof u.path)
		fail("URL too long");
	for (const char *q = slash; *q; q++)
		if ((unsigned char)*q <= 32 || (unsigned char)*q >= 127 ||
		    *q == '#')
			fail("invalid URL path");
	strcpy(u.path, slash);
	(void)snprintf(u.origin, sizeof u.origin, "%s:%s:%d", u.host, u.port,
		       u.tls);
	return u;
}
static void waitfd(int fd, int events, long long end)
{
	struct pollfd p = { fd, (short)events, 0 };
	int n;
	for (;;) {
		long long left = end - now();
		if (left <= 0)
			fail("request deadline");
		n = poll(&p, 1, (int)left);
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	if (n <= 0)
		fail("request timeout or poll failure");
}
struct connection {
	int fd;
	SSL_CTX *ctx;
	SSL *ssl;
	long long end, idle;
};
static long long deadline(const struct connection *c)
{
	return c->idle < c->end ? c->idle : c->end;
}
static struct connection connect_url(struct url *u)
{
	struct connection c;
	memset(&c, 0, sizeof c);
	c.fd = -1;
	c.end = c.idle = now() + IDLE_TIMEOUT;
	struct addrinfo hints, *list = NULL;
	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	/* POSIX getaddrinfo has no portable cancellation deadline; documented. */
	if (getaddrinfo(u->host, u->port, &hints, &list))
		fail("DNS lookup failed");
	for (struct addrinfo *a = list; a; a = a->ai_next) {
		c.fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (c.fd < 0)
			continue;
		if (fcntl(c.fd, F_SETFL, O_NONBLOCK) < 0 ||
		    fcntl(c.fd, F_SETFD, FD_CLOEXEC) < 0)
			fail("socket flags");
		int n = connect(c.fd, a->ai_addr, a->ai_addrlen);
		if (n < 0 && errno == EINPROGRESS) {
			waitfd(c.fd, POLLOUT, c.end);
			int e;
			socklen_t l = sizeof e;
			if (getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &e, &l) < 0)
				fail("connect status");
			if (!e)
				n = 0;
		}
		if (!n)
			break;
		close(c.fd);
		c.fd = -1;
	}
	freeaddrinfo(list);
	if (c.fd < 0)
		fail("connection failed");
	if (u->tls) {
		c.ctx = SSL_CTX_new(TLS_client_method());
		if (!c.ctx)
			fail("TLS context");
		SSL_CTX_set_verify(c.ctx, SSL_VERIFY_PEER, NULL);
		if (!SSL_CTX_set_min_proto_version(c.ctx, TLS1_2_VERSION) ||
		    !SSL_CTX_set_default_verify_paths(c.ctx))
			fail("TLS trust store");
		c.ssl = SSL_new(c.ctx);
		if (!c.ssl || !SSL_set_fd(c.ssl, c.fd) ||
		    !SSL_set_tlsext_host_name(c.ssl, u->host))
			fail("TLS setup");
		X509_VERIFY_PARAM *v = SSL_get0_param(c.ssl);
		if (!strcmp(u->host, "127.0.0.1")) {
			if (!X509_VERIFY_PARAM_set1_ip_asc(v, u->host))
				fail("TLS IP verification");
		} else if (!SSL_set1_host(c.ssl, u->host))
			fail("TLS hostname verification");
		for (;;) {
			int n = SSL_connect(c.ssl);
			if (n == 1)
				break;
			int e = SSL_get_error(c.ssl, n);
			if (e == SSL_ERROR_WANT_READ ||
			    e == SSL_ERROR_WANT_WRITE)
				waitfd(c.fd,
				       e == SSL_ERROR_WANT_READ ? POLLIN :
								  POLLOUT,
				       c.end);
			else
				fail("TLS handshake or certificate verification failed");
		}
	}
	return c;
}
static int transfer(struct connection *c, void *data, size_t size, int writing)
{
	for (;;) {
		if (now() >= deadline(c))
			fail("request deadline");
		int n;
		if (c->ssl) {
			n = writing ? SSL_write(c->ssl, data, (int)size) :
				      SSL_read(c->ssl, data, (int)size);
			if (n > 0) {
				c->idle = now() + IDLE_TIMEOUT;
				return n;
			}
			int e = SSL_get_error(c->ssl, n);
			if (e == SSL_ERROR_ZERO_RETURN && !writing)
				return 0;
			if (e == SSL_ERROR_WANT_READ ||
			    e == SSL_ERROR_WANT_WRITE) {
				waitfd(c->fd,
				       e == SSL_ERROR_WANT_READ ? POLLIN :
								  POLLOUT,
				       deadline(c));
				continue;
			}
			fail("TLS stream failed or truncated");
		}
		ssize_t k = writing ? write(c->fd, data, size) :
				      read(c->fd, data, size);
		if (k >= 0) {
			if (k > 0)
				c->idle = now() + IDLE_TIMEOUT;
			return (int)k;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			waitfd(c->fd, writing ? POLLOUT : POLLIN, deadline(c));
			continue;
		}
		fail("socket I/O failed");
	}
}
/* Nonempty accepted values make stored values a reliable presence marker. */
static void header(char *out, size_t cap, const char *value)
{
	if (*out || !*value || strlen(value) >= cap)
		fail("duplicate, empty or oversized header");
	strcpy(out, value);
}
static void validate_headers(const unsigned char *wire, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		unsigned char c = wire[i];
		if (c == '\r') {
			if (i + 1 >= size || wire[i + 1] != '\n')
				fail("invalid HTTP line ending");
			i++;
		} else if (c == '\n' || (c < 32 && c != '\t') || c == 127)
			fail("invalid HTTP header byte");
	}
}
static struct response request(struct url *u, const char *token, size_t limit)
{
	struct connection c = connect_url(u);
	char req[20000];
	if (token)
		for (const char *p = token; *p; p++)
			if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127)
				fail("invalid bearer token");
	int len = snprintf(
		req, sizeof req,
		"GET %s HTTP/1.1\r\nHost: %s\r\nAccept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json\r\nConnection: close\r\n%s%s%s\r\n",
		u->path, u->authority, token ? "Authorization: Bearer " : "",
		token ? token : "", token ? "\r\n" : "");
	if (len < 0 || (size_t)len >= sizeof req)
		fail("request too long");
	for (int off = 0; off < len;) {
		int n = transfer(&c, req + off, (size_t)(len - off), 1);
		if (!n)
			fail("short request");
		off += n;
	}
	unsigned char *wire = malloc(HEAD_LIMIT + 1);
	if (!wire)
		fail("allocation");
	size_t total = 0, body = 0;
	/* Read headers without depending on server EOF; body framing is mandatory. */
	while (!body) {
		if (total >= HEAD_LIMIT)
			fail("headers too large");
		int n = transfer(&c, wire + total, 1, 0);
		if (!n)
			fail("truncated headers");
		total += (size_t)n;
		if (total >= 4 && !memcmp(wire + total - 4, "\r\n\r\n", 4))
			body = total;
	}
	validate_headers(wire, total);
	if (total < 7 || memcmp(wire, "HTTP/1.", 7))
		fail("invalid HTTP status");
	wire[total] = 0;
	struct response r;
	memset(&r, 0, sizeof r);
	char *save, *line = strtok_r((char *)wire, "\r\n", &save);
	if (!line || strncmp(line, "HTTP/1.", 7) || strlen(line) < 13 ||
	    (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
	    line[9] < '1' || line[9] > '5' || line[10] < '0' ||
	    line[10] > '9' || line[11] < '0' || line[11] > '9' ||
	    line[12] != ' ')
		fail("invalid HTTP status");
	r.status =
		(line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
	char length[32] = "", encoding[64] = "";
	while ((line = strtok_r(NULL, "\r\n", &save))) {
		char *colon = strchr(line, ':');
		if (!colon || colon == line || line[0] == ' ' ||
		    line[0] == '\t')
			fail("invalid HTTP header");
		*colon++ = 0;
		for (const char *p = line; *p; p++)
			if (!((*p >= 'a' && *p <= 'z') ||
			      (*p >= 'A' && *p <= 'Z') ||
			      (*p >= '0' && *p <= '9') ||
			      strchr("!#$%&'*+-.^_`|~", *p)))
				fail("invalid HTTP header name");
		while (*colon == ' ' || *colon == '\t')
			colon++;
		char *end = colon + strlen(colon);
		while (end > colon && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = 0;
		if (!strcasecmp(line, "Content-Length"))
			header(length, sizeof length, colon);
		else if (!strcasecmp(line, "Transfer-Encoding"))
			header(encoding, sizeof encoding, colon);
		else if (!strcasecmp(line, "Location"))
			header(r.location, sizeof r.location, colon);
		else if (!strcasecmp(line, "WWW-Authenticate"))
			header(r.challenge, sizeof r.challenge, colon);
		else if (!strcasecmp(line, "Content-Type"))
			header(r.type, sizeof r.type, colon);
		else if (!strcasecmp(line, "Docker-Content-Digest"))
			header(r.digest, sizeof r.digest, colon);
		else if (!strcasecmp(line, "Content-Encoding") &&
			 strcasecmp(colon, "identity"))
			fail("unsupported HTTP content encoding");
	}
	if ((*length && *encoding) || (!*length && !*encoding))
		fail("ambiguous or missing HTTP framing");
	c.end = now() + BODY_TIMEOUT;
	c.idle = now() + IDLE_TIMEOUT;
	r.data = malloc(limit + 1);
	if (!r.data)
		fail("allocation");
	if (*length) {
		size_t size = 0;
		for (char *p = length; *p; p++) {
			if (*p < '0' || *p > '9' || size > limit / 10)
				fail("invalid Content-Length");
			size = size * 10 + (unsigned)(*p - '0');
			if (size > limit)
				fail("response too large");
		}
		while (r.size < size) {
			size_t left = size - r.size;
			int n = transfer(&c, r.data + r.size,
					 left > 32768 ? 32768 : left, 0);
			if (!n)
				fail("truncated HTTP body");
			r.size += (size_t)n;
		}
	} else {
		if (strcasecmp(encoding, "chunked"))
			fail("unsupported transfer encoding");
		for (;;) {
			char chunk[32];
			size_t n = 0;
			for (;;) {
				if (n >= sizeof chunk - 1 ||
				    transfer(&c, chunk + n, 1, 0) != 1)
					fail("invalid chunk");
				n++;
				if (n >= 2 && !memcmp(chunk + n - 2, "\r\n", 2))
					break;
			}
			chunk[n - 2] = 0;
			size_t size = 0;
			if (!chunk[0])
				fail("invalid chunk size");
			for (size_t i = 0; i < n - 2; i++) {
				unsigned char byte = (unsigned char)chunk[i];
				unsigned v;
				if (byte >= '0' && byte <= '9')
					v = (unsigned)(byte - '0');
				else if (byte >= 'a' && byte <= 'f')
					v = (unsigned)(byte - 'a' + 10);
				else if (byte >= 'A' && byte <= 'F')
					v = (unsigned)(byte - 'A' + 10);
				else
					fail("chunk extensions unsupported");
				if (size > limit / 16)
					fail("chunk too large");
				size = size * 16 + v;
			}
			if (size > limit - r.size)
				fail("response too large");
			size_t end = r.size + size;
			while (r.size < end) {
				size_t left = end - r.size;
				int k = transfer(&c, r.data + r.size,
						 left > 32768 ? 32768 : left,
						 0);
				if (!k)
					fail("truncated chunk");
				r.size += (size_t)k;
			}
			char crlf[2];
			for (int i = 0; i < 2; i++)
				if (transfer(&c, crlf + i, 1, 0) != 1)
					fail("truncated chunk terminator");
			if (memcmp(crlf, "\r\n", 2))
				fail("chunk trailers unsupported or invalid terminator");
			if (!size)
				break;
		}
	}
	r.data[r.size] = 0;
	free(wire);
	SSL_free(c.ssl);
	SSL_CTX_free(c.ctx);
	close(c.fd);
	return r;
}
struct response registry_get(const char *url, const char *token, int local,
			     size_t limit)
{
	struct url u = parse(url, local);
	const char *auth = token;
	for (int count = 0; count < 5; count++) {
		struct response r = request(&u, auth, limit);
		if (r.status != 301 && r.status != 302 && r.status != 303 &&
		    r.status != 307 && r.status != 308)
			return r;
		struct url next;
		if (r.location[0] == '/' && r.location[1] != '/') {
			char s[4600];
			(void)snprintf(s, sizeof s, "%s://%s%s",
				       u.tls ? "https" : "http", u.authority,
				       r.location);
			next = parse(s, local);
		} else
			next = parse(r.location, local);
		if (u.tls && !next.tls)
			fail("TLS downgrade redirect");
		if (strcmp(u.origin, next.origin))
			auth = NULL;
		free(r.data);
		u = next;
	}
	fail("too many redirects");
	struct response r = { 0 };
	return r;
}
static void query(char *out, size_t cap, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (n + 3 >= cap)
			fail("token query too long");
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || strchr("-._~", c))
			out[n++] = (char)c;
		else {
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = 0;
}
char *registry_token(const char *challenge, int local)
{
	if (strncmp(challenge, "Bearer ", 7))
		fail("unsupported registry authentication (anonymous Bearer only)");
	char realm[4096] = "", service[1024] = "", scope[1024] = "";
	const char *p = challenge + 7;
	while (*p) {
		while (*p == ' ' || *p == ',')
			p++;
		const char *key = p;
		while (*p && *p != '=')
			p++;
		size_t k = (size_t)(p - key);
		if (*p++ != '=' || *p++ != '"')
			fail("invalid Bearer challenge");
		const char *v = p;
		while (*p && *p != '"') {
			if (*p == '\\')
				fail("escaped challenge unsupported");
			p++;
		}
		size_t n = (size_t)(p - v);
		if (*p++ != '"')
			fail("invalid Bearer challenge");
		char *dest = NULL;
		size_t cap = 0;
		if (k == 5 && !memcmp(key, "realm", 5)) {
			dest = realm;
			cap = sizeof realm;
		} else if (k == 7 && !memcmp(key, "service", 7)) {
			dest = service;
			cap = sizeof service;
		} else if (k == 5 && !memcmp(key, "scope", 5)) {
			dest = scope;
			cap = sizeof scope;
		} else
			fail("unsupported Bearer challenge parameter");
		if (*dest || n >= cap)
			fail("invalid Bearer challenge field");
		memcpy(dest, v, n);
		dest[n] = 0;
		if (*p && *p != ',')
			fail("invalid Bearer challenge delimiter");
	}
	struct url u = parse(realm, local);
	(void)u;
	char a[3073], b[3073], url[11000];
	query(a, sizeof a, service);
	query(b, sizeof b, scope);
	int n = snprintf(url, sizeof url, "%s%cservice=%s&scope=%s", realm,
			 strchr(realm, '?') ? '&' : '?', a, b);
	if (n < 0 || (size_t)n >= sizeof url)
		fail("token URL too long");
	struct response r = registry_get(url, NULL, local, 1024 * 1024);
	if (r.status != 200)
		fail("anonymous token request failed");
	yyjson_doc *doc = json_parse(r.data, r.size);
	yyjson_val *j = yyjson_doc_get_root(doc), *t = json_get(j, "token");
	if (!t)
		t = json_get(j, "access_token");
	char *token = strdup(json_string(t));
	if (!token || !*token || strlen(token) > 8192)
		fail("invalid token");
	yyjson_doc_free(doc);
	free(r.data);
	return token;
}
