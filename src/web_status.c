#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "web_status.h"

#define REQUEST_BUF_SIZE 512
#define JSON_BUF_SIZE 512
#define ACCEPT_POLL_TIMEOUT_MS 1000

struct web_server_ctx {
	int listen_fd;
	struct app_web_status *status;
	volatile bool *quit_flag;
};

static pthread_t g_server_thread;
static bool g_server_running;
static struct web_server_ctx g_ctx;

void
app_web_status_init(struct app_web_status *status)
{
	memset(status, 0, sizeof(*status));
	status->start_time = time(NULL);
	pthread_mutex_init(&status->lock, NULL);
}

void
app_web_status_destroy(struct app_web_status *status)
{
	pthread_mutex_destroy(&status->lock);
}

void
app_web_status_update(struct app_web_status *status, uint64_t records_in,
		       uint64_t records_dropped, uint64_t records_forwarded)
{
	pthread_mutex_lock(&status->lock);
	status->records_in = records_in;
	status->records_dropped = records_dropped;
	status->records_forwarded = records_forwarded;
	pthread_mutex_unlock(&status->lock);
}

/* Same shape as dpdk-app-example's web_status.c read_request_line() -
 * this server only ever needs the method+path from the first line, not
 * a general HTTP parser. */
static void
read_request_line(int fd, char *line_out, size_t line_out_size)
{
	char buf[REQUEST_BUF_SIZE];
	size_t total = 0;
	bool have_line = false;

	line_out[0] = '\0';

	while (total < sizeof(buf) - 1) {
		ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
		if (n <= 0)
			break;
		total += (size_t)n;
		buf[total] = '\0';

		if (!have_line) {
			char *eol = strstr(buf, "\r\n");
			if (eol != NULL) {
				size_t len = (size_t)(eol - buf);
				if (len >= line_out_size)
					len = line_out_size - 1;
				memcpy(line_out, buf, len);
				line_out[len] = '\0';
				have_line = true;
			}
		}
		if (strstr(buf, "\r\n\r\n") != NULL)
			break;
	}
}

static void
send_response(int fd, const char *status_line, const char *content_type, const char *body)
{
	char header[256];
	int header_len = snprintf(header, sizeof(header),
		"HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		status_line, content_type, strlen(body));
	if (header_len < 0)
		return;
	(void)send(fd, header, (size_t)header_len, MSG_NOSIGNAL);
	(void)send(fd, body, strlen(body), MSG_NOSIGNAL);
}

static void
handle_connection(int fd, struct app_web_status *status)
{
	char request_line[REQUEST_BUF_SIZE];
	read_request_line(fd, request_line, sizeof(request_line));

	if (strncmp(request_line, "GET /status.json", strlen("GET /status.json")) == 0) {
		char json[JSON_BUF_SIZE];

		pthread_mutex_lock(&status->lock);
		snprintf(json, sizeof(json),
			"{\n"
			"  \"mode\": \"loomtabulator\",\n"
			"  \"uptime_sec\": %ld,\n"
			"  \"records_in\": %" PRIu64 ",\n"
			"  \"records_dropped\": %" PRIu64 ",\n"
			"  \"records_forwarded\": %" PRIu64 "\n"
			"}\n",
			(long)(time(NULL) - status->start_time),
			status->records_in, status->records_dropped, status->records_forwarded);
		pthread_mutex_unlock(&status->lock);

		send_response(fd, "200 OK", "application/json", json);
	} else {
		send_response(fd, "404 Not Found", "text/plain", "not found\n");
	}
}

static void *
server_thread_main(void *arg)
{
	struct web_server_ctx *ctx = arg;

	while (!*ctx->quit_flag) {
		struct pollfd pfd = { .fd = ctx->listen_fd, .events = POLLIN };
		int ret = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
		if (ret <= 0)
			continue;

		int conn_fd = accept(ctx->listen_fd, NULL, NULL);
		if (conn_fd < 0)
			continue;

		handle_connection(conn_fd, ctx->status);
		close(conn_fd);
	}
	return NULL;
}

int
web_status_start(uint16_t web_port, struct app_web_status *status, volatile bool *quit_flag)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "web_status: socket() failed: %s\n", strerror(errno));
		return -1;
	}

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port = htons(web_port),
	};

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "web_status: bind() to port %u failed: %s\n", web_port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 8) != 0) {
		fprintf(stderr, "web_status: listen() failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	g_ctx.listen_fd = fd;
	g_ctx.status = status;
	g_ctx.quit_flag = quit_flag;

	int ret = pthread_create(&g_server_thread, NULL, server_thread_main, &g_ctx);
	if (ret != 0) {
		fprintf(stderr, "web_status: pthread_create failed: %s\n", strerror(ret));
		close(fd);
		return -1;
	}

	g_server_running = true;
	return 0;
}

void
web_status_stop(void)
{
	if (!g_server_running)
		return;
	pthread_join(g_server_thread, NULL);
	close(g_ctx.listen_fd);
	g_server_running = false;
}
