#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>

// 统一使用的句柄类型
typedef int poll_fd;

// 转存的内核通知的结构体
struct event {
	void * s;		// 通知的句柄
	bool read;		// 是否可读
	bool write;		// 是否可写
	bool error;		// 是否出现错误
	bool eof;
};

static bool sp_invalid(poll_fd fd);
static poll_fd sp_create();
static void sp_release(poll_fd fd);
static int sp_add(poll_fd fd, int sock, void *ud);
static void sp_del(poll_fd fd, int sock);
static int sp_enable(poll_fd, int sock, void *ud, bool read_enable, bool write_enable);
static int sp_wait(poll_fd, struct event *e, int max);
static void sp_nonblocking(int sock);

#ifdef __linux__
#include "socket_epoll.h"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#include "socket_kqueue.h"
#endif

#endif
