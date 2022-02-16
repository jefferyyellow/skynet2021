#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"
#include "atomic.h"
#include "spinlock.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16
#define MAX_EVENT 64
#define MIN_READ_BUFFER 64
#define SOCKET_TYPE_INVALID 0			// 初始状态
#define SOCKET_TYPE_RESERVE 1			// 保留状态
#define SOCKET_TYPE_PLISTEN 2			// 监听前状态
#define SOCKET_TYPE_LISTEN 3			// 监听中状态
#define SOCKET_TYPE_CONNECTING 4		// 连接中状态
#define SOCKET_TYPE_CONNECTED 5			// 已连接状态
#define SOCKET_TYPE_HALFCLOSE_READ 6	// 关闭读的半连接状态
#define SOCKET_TYPE_HALFCLOSE_WRITE 7	// 关闭写的半连接状态
#define SOCKET_TYPE_PACCEPT 8			// 已接收，但是未添加到epoll(需要start)
#define SOCKET_TYPE_BIND 9				// 绑定系统fd(stdin,stdout,stderr)

#define MAX_SOCKET (1<<MAX_SOCKET_P)

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)
#define ID_TAG16(id) ((id>>MAX_SOCKET_P) & 0xffff)

#define PROTOCOL_TCP 0					// TCP协议
#define PROTOCOL_UDP 1					// UDP协议
#define PROTOCOL_UDPv6 2				// UDPv6协议
#define PROTOCOL_UNKNOWN 255			// 未知协议

#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type

#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

#define WARNING_SIZE (1024*1024)

#define USEROBJECT ((size_t)(-1))

struct write_buffer {
	struct write_buffer * next; 	// 由于是链表结构，这个用于指向下个节点
	const void *buffer;				// 要发送的内存数据块
	char *ptr;						// 已发送数据的偏移位置，也就是下次发送从这开始
	size_t sz;						// 剩余多少数据没有发送
	bool userobject;				// 是否是用户数据,标记初始化发送对象时有没有用socket_server中的soi接口
	uint8_t udp_address[UDP_ADDRESS_SIZE]; // udp专用，这个数据发送的目标地址
};

#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))
// 写缓冲队列
struct wb_list {
	struct write_buffer * head; 	//写缓冲区的头指针
	struct write_buffer * tail;		//写缓冲区的尾指针
};

struct socket_stat {
	uint64_t rtime;
	uint64_t wtime;
	uint64_t read;
	uint64_t write;
};

struct socket {
	uintptr_t opaque;				// 所属服务在skynet中对应的handle
	struct wb_list high;			// 高优先级的写缓存
	struct wb_list low;				// 低优先级的写缓存
	int64_t wb_size;				// 套接字写入缓存的大小，会随着缓存的大小变化，为高优先级和低优先级的和
	struct socket_stat stat;		// 统计信息
	ATOM_ULONG sending;
	int fd;							// 对应内存分配的fd(文件描述)
	int id;							// 应用层维护一个与fd对应的id句柄,用于在socket_server结构体中定位存储套接字信息
	ATOM_INT type;					// socket结构体所处的状态，绑定套接字时，即套接字的状态
	uint8_t protocol;				// 协议类型，如上
	bool reading;
	bool writing;
	bool closing;
	ATOM_INT udpconnecting;			// 大于0标记该套接字正在进行关联ip地址操作，用于UDP协议
	int64_t warn_size;				// 阈值，写缓存超过的阈值，每超过一次阈值就会翻倍
	union {
		int size;					// 在 TCP 协议下使用, 表示一次性最多读取的字节数
		uint8_t udp_address[UDP_ADDRESS_SIZE]; // udp绑定的地址
	} p;
	struct spinlock dw_lock;		// 写缓存锁
	int dw_offset;					// 已经发送的数据偏移位置
	const void * dw_buffer;			// 已发送一部分的全部数据缓存
	size_t dw_size;					// 已发送一部分的全部数据的大小
};

// socket服务器
struct socket_server {
	// 创建时间
	volatile uint64_t time;
	// 接受ctrl的管道句柄
	int recvctrl_fd;
	// 发送ctrl的管道句柄
	int sendctrl_fd;
	// 检查控制命令
	int checkctrl;
	// epoll或kevent的句柄
	poll_fd event_fd;
	// 应用层分配id用的
	ATOM_INT alloc_id;
	// epoll_wait返回的事件数
	int event_n;
	// 当前处理的事件序号
	int event_index;
	// 初始化发送对象时用
	struct socket_object_interface soi;
	//epoll_wait 返回的事件集合
	struct event ev[MAX_EVENT];
	// 所有套接字相关的信息 65535个slot
	struct socket slot[MAX_SOCKET];
	// 临时数据的保存，比如保存对方的地址信息等
	char buffer[MAX_INFO];
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	//用于select的fd集
	fd_set rfds;
};

struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;
	size_t sz;
	const void * buffer;
};

struct request_send_udp {
	struct request_send send;
	uint8_t address[UDP_ADDRESS_SIZE];
};

// 设置udp的地址
struct request_setudp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
	int id;
	int shutdown;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_resumepause {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
	Q query info
 */

struct request_package {
	uint8_t header[8];	// 6 bytes dummy
	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_resumepause resumepause;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct send_object {
	const void * buffer;
	size_t sz;
	void (*free_func)(void *);
};

#define MALLOC skynet_malloc
#define FREE skynet_free

struct socket_lock {
	struct spinlock *lock;
	int count;
};

static inline void
socket_lock_init(struct socket *s, struct socket_lock *sl) {
	sl->lock = &s->dw_lock;
	sl->count = 0;
}

static inline void
socket_lock(struct socket_lock *sl) {
	if (sl->count == 0) {
		spinlock_lock(sl->lock);
	}
	++sl->count;
}

static inline int
socket_trylock(struct socket_lock *sl) {
	if (sl->count == 0) {
		if (!spinlock_trylock(sl->lock))
			return 0;	// lock failed
	}
	++sl->count;
	return 1;
}

static inline void
socket_unlock(struct socket_lock *sl) {
	--sl->count;
	if (sl->count <= 0) {
		assert(sl->count == 0);
		spinlock_unlock(sl->lock);
	}
}

static inline int
socket_invalid(struct socket *s, int id) {
	return (s->id != id || ATOM_LOAD(&s->type) == SOCKET_TYPE_INVALID);
}

// 发送实体的初始化
static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, const void *object, size_t sz) {
	// 如果是USEROBJECT，调用接口取得buffer和buffer大小
	if (sz == USEROBJECT) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;
	} else {
		// 直接用传入的
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}

static void
dummy_free(void *ptr) {
	(void)ptr;
}

static inline void
send_object_init_from_sendbuffer(struct socket_server *ss, struct send_object *so, struct socket_sendbuffer *buf) {
	switch (buf->type) {
	case SOCKET_BUFFER_MEMORY:
		send_object_init(ss, so, buf->buffer, buf->sz);
		break;
	case SOCKET_BUFFER_OBJECT:
		send_object_init(ss, so, buf->buffer, USEROBJECT);
		break;
	case SOCKET_BUFFER_RAWPOINTER:
		so->buffer = buf->buffer;
		so->sz = buf->sz;
		so->free_func = dummy_free;
		break;
	default:
		// never get here
		so->buffer = NULL;
		so->sz = 0;
		so->free_func = NULL;
		break;
	}
}

static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {
	if (wb->userobject) {
		ss->soi.free((void *)wb->buffer);
	} else {
		FREE((void *)wb->buffer);
	}
	FREE(wb);
}

// 设置keepalive的选项
static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}

// 给socket分配一个id
static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		// 加1
		int id = ATOM_FINC(&(ss->alloc_id))+1;
		if (id < 0) {
			id = ATOM_FAND(&(ss->alloc_id), 0x7fffffff) & 0x7fffffff;
		}
		// 通过id得到分配的slot的内存
		struct socket *s = &ss->slot[HASH_ID(id)];
		int type_invalid = ATOM_LOAD(&s->type);
		// 找到合适的slot
		if (type_invalid == SOCKET_TYPE_INVALID) {
			if (ATOM_CAS(&s->type, type_invalid, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->protocol = PROTOCOL_UNKNOWN;
				// socket_server_udp_connect may inc s->udpconncting directly (from other thread, before new_fd), 
				// so reset it to 0 here rather than in new_fd.
				ATOM_INIT(&s->udpconnecting, 0);
				s->fd = -1;
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}

// 清空写入缓冲区列表
static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}

// 创建socket服务器
struct socket_server * 
socket_server_create(uint64_t time) {
	int i;
	int fd[2];
	// 创建事件池
	poll_fd efd = sp_create();
	if (sp_invalid(efd)) {
		skynet_error(NULL, "socket-server: create event pool failed.");
		return NULL;
	}
	// pipe函数可用于创建一个管道，以实现进程间的通信。
	if (pipe(fd)) {
		sp_release(efd);
		skynet_error(NULL, "socket-server: create socket pair failed.");
		return NULL;
	}
	// 加入事件池管理
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		skynet_error(NULL, "socket-server: can't add server fd to event pool.");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	// socket服务器
	struct socket_server *ss = MALLOC(sizeof(*ss));
	ss->time = time;
	ss->event_fd = efd;
	ss->recvctrl_fd = fd[0];
	ss->sendctrl_fd = fd[1];
	ss->checkctrl = 1;

	// 最大65535个slot
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		ATOM_INIT(&s->type, SOCKET_TYPE_INVALID);
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
		spinlock_init(&s->dw_lock);
	}

	// 服务器其他字段初始化
	ATOM_INIT(&ss->alloc_id , 0);
	ss->event_n = 0;
	ss->event_index = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));
	FD_ZERO(&ss->rfds);
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

// 更新时间
void
socket_server_updatetime(struct socket_server *ss, uint64_t time) {
	ss->time = time;
}

// 释放写入BUFF列表
static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {
	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}

// 释放缓冲器
static void
free_buffer(struct socket_server *ss, struct socket_sendbuffer *buf) {
	void *buffer = (void *)buf->buffer;
	
	switch (buf->type) {
	// 内存
	case SOCKET_BUFFER_MEMORY:
		FREE(buffer);
		break;
	// 释放对象
	case SOCKET_BUFFER_OBJECT:
		ss->soi.free(buffer);
		break;
	case SOCKET_BUFFER_RAWPOINTER:
		break;
	}
}

// clone缓冲区
static const void *
clone_buffer(struct socket_sendbuffer *buf, size_t *sz) {
	switch (buf->type) {
	case SOCKET_BUFFER_MEMORY:
		*sz = buf->sz;
		return buf->buffer;
	case SOCKET_BUFFER_OBJECT:
		*sz = USEROBJECT;
		return buf->buffer;
	case SOCKET_BUFFER_RAWPOINTER:
		// It's a raw pointer, we need make a copy
		*sz = buf->sz;
		void * tmp = MALLOC(*sz);
		memcpy(tmp, buf->buffer, *sz);
		return tmp;
	}
	// never get here
	*sz = 0;
	return NULL;
}

// 关闭socket
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
	uint8_t type = ATOM_LOAD(&s->type);
	if (type == SOCKET_TYPE_INVALID) {
		return;
	}

	// 释放socket的缓冲区
	assert(type != SOCKET_TYPE_RESERVE);
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);
	// 从事件池中删除socket句柄
	sp_del(ss->event_fd, s->fd);
	socket_lock(l);
	// 关闭socket
	if (type != SOCKET_TYPE_BIND) {
		if (close(s->fd) < 0) {
			perror("close socket:");
		}
	}
	ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
	// 释放双缓冲区
	if (s->dw_buffer) {
		struct socket_sendbuffer tmp;
		tmp.buffer = s->dw_buffer;
		tmp.sz = s->dw_size;
		tmp.id = s->id;
		tmp.type = (tmp.sz == USEROBJECT) ? SOCKET_BUFFER_OBJECT : SOCKET_BUFFER_MEMORY;
		free_buffer(ss, &tmp);
		s->dw_buffer = NULL;
	}
	socket_unlock(l);
}

// 释放服务器
void 
socket_server_release(struct socket_server *ss) {
	int i;
	struct socket_message dummy;
	// 强制关闭所有的socket，
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		struct socket_lock l;
		socket_lock_init(s, &l);
		if (ATOM_LOAD(&s->type) != SOCKET_TYPE_RESERVE) {
			force_close(ss, s, &l, &dummy);
		}
		// 销毁锁
		spinlock_destroy(&s->dw_lock);
	}
	// 销毁控制的管道句柄
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);
	// 释放事件集合句柄
	sp_release(ss->event_fd);
	FREE(ss);
}

// 检查
static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}

// 设置socket的在事件集合中可写
static inline int
enable_write(struct socket_server *ss, struct socket *s, bool enable) {
	if (s->writing != enable) {
		s->writing = enable;
		return sp_enable(ss->event_fd, s->fd, s, s->reading, enable);
	}
	return 0;
}

// 设置socket的在事件集合中可读
static inline int
enable_read(struct socket_server *ss, struct socket *s, bool enable) {
	if (s->reading != enable) {
		s->reading = enable;
		return sp_enable(ss->event_fd, s->fd, s, enable, s->writing);
	}
	return 0;
}

// 处理新创建的socket
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool reading) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	assert(ATOM_LOAD(&s->type) == SOCKET_TYPE_RESERVE);

	// 加入到事件集合中
	if (sp_add(ss->event_fd, fd, s)) {
		ATOM_STORE(&s->type, SOCKET_TYPE_INVALID);
		return NULL;
	}

	// 设置socket的id和句柄
	s->id = id;
	s->fd = fd;
	s->reading = true;
	s->writing = false;
	s->closing = false;
	ATOM_INIT(&s->sending , ID_TAG16(id) << 16 | 0);
	s->protocol = protocol;
	s->p.size = MIN_READ_BUFFER;
	s->opaque = opaque;
	s->wb_size = 0;
	s->warn_size = 0;
	check_wb_list(&s->high);
	check_wb_list(&s->low);
	s->dw_buffer = NULL;
	s->dw_size = 0;
	memset(&s->stat, 0, sizeof(s->stat));
	// 设置为监听可读事件
	if (enable_read(ss, s, reading)) {
		ATOM_STORE(&s->type , SOCKET_TYPE_INVALID);
		return NULL;
	}
	return s;
}

// 统计读的字节数和读取时间
static inline void
stat_read(struct socket_server *ss, struct socket *s, int n) {
	s->stat.read += n;
	s->stat.rtime = ss->time;
}

// 统计写的字节数和写入时间
static inline void
stat_write(struct socket_server *ss, struct socket *s, int n) {
	s->stat.write += n;
	s->stat.wtime = ss->time;
}

// return -1 when connecting
// 打开socket
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {
	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;

	struct socket *ns;
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;
	char port[16];
	sprintf(port, "%d", request->port);
	memset(&ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC; 
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	// 得到连接的地址信息
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {
		result->data = (void *)gai_strerror(status);
		goto _failed_getaddrinfo;
	}
	int sock= -1;

	// getaddrinfo返回的是链表，意味着可能有多个
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
		// 创建多个socket
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		socket_keepalive(sock);
		sp_nonblocking(sock);
		// 请求连接
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}

	// 如果socket创建没有成功，结果的数据就是错误信息
	if (sock < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	// 处理新创建的socket
	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	// 连接成功
	if(status == 0) {
		ATOM_STORE(&ns->type , SOCKET_TYPE_CONNECTED);
		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;
		// 这个函数转换网络二进制结构到ASCII类型的地址
		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			result->data = ss->buffer;
		}
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {
		ATOM_STORE(&ns->type , SOCKET_TYPE_CONNECTING);
		// 设置可写，在信号集合里面注册侦听可写的
		if (enable_write(ss, ns, true)) {
			result->data = "enable write failed";
			goto _failed;
		}
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
_failed_getaddrinfo:
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERR;
}

// 报告错误
static int
report_error(struct socket *s, struct socket_message *result, const char *err) {
	result->id = s->id;
	result->ud = 0;
	result->opaque = s->opaque;
	result->data = (char *)err;
	return SOCKET_ERR;
}

// 关闭写
static int
close_write(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	// 如果socket正在关闭中
	if (s->closing) {
		// 强制关闭
		force_close(ss,s,l,result);
		return SOCKET_RST;
	} else {
		int t = ATOM_LOAD(&s->type);
		// 如果socket已经是读关闭了
		if (t == SOCKET_TYPE_HALFCLOSE_READ) {
			// recv 0 before, ignore the error and close fd
			// 那就直接强制关闭
			force_close(ss,s,l,result);
			return SOCKET_RST;
		}
		// 如果socket已经关闭写了
		if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
			// already raise SOCKET_ERR
			// 直接返回就好
			return SOCKET_RST;
		}
		ATOM_STORE(&s->type, SOCKET_TYPE_HALFCLOSE_WRITE);
		// 关闭写，并且在信号集中将写信号关闭掉
		shutdown(s->fd, SHUT_WR);
		enable_write(ss, s, false);
		return report_error(s, result, strerror(errno));
	}
}

// 将写缓冲队列发送出去（tcp）
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
	// 遍历写缓存列表
	while (list->head) {
		struct write_buffer * tmp = list->head;
		// 死循环用来发送数据
		for (;;) {
			ssize_t sz = write(s->fd, tmp->ptr, tmp->sz);
			if (sz < 0) {
				// 打断或者阻塞
				switch(errno) {
				case EINTR:
					continue;
				case AGAIN_WOULDBLOCK:
					return -1;
				}
				// 其他原因出错，就直接关闭写
				return close_write(ss, s, l, result);
			}
			// 发送成功，增加统计数据
			stat_write(ss,s,(int)sz);
			s->wb_size -= sz;
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			break;
		}
		// 遍历到下一个节点
		list->head = tmp->next;
		// 释放已经发送的节点
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

// UDP socket 地址
static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	// 第一个字节是类型
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	uint16_t port = 0;
	// 紧接着后面两个字节是端口
	memcpy(&port, udp_address+1, sizeof(uint16_t));
	// 根据协议
	switch (s->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		// 后面的都是地址32位
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		// ip v6的地址是128位
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}

// 丢弃list中tmp和tmp之前的
static void
drop_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct write_buffer *tmp) {
	s->wb_size -= tmp->sz;
	list->head = tmp->next;
	if (list->head == NULL)
		list->tail = NULL;
	write_buffer_free(ss,tmp);
}

// 将写缓冲队列发送出去（Udp）
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	// 遍历缓冲区列表
	while (list->head) {
		struct write_buffer * tmp = list->head;
		union sockaddr_all sa;
		// 得到udp地址
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);
		if (sasz == 0) {
			skynet_error(NULL, "socket-server : udp (%d) type mismatch.", s->id);
			drop_udp(ss, s, list, tmp);
			return -1;
		}
		// 发送出去
		int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);
		if (err < 0) {
			switch(errno) {
			case EINTR:
			case AGAIN_WOULDBLOCK:
				return -1;
			}
			skynet_error(NULL, "socket-server : udp (%d) sendto error %s.",s->id, strerror(errno));
			// 出错了，丢弃掉
			drop_udp(ss, s, list, tmp);
			return -1;
		}
		// 加入统计
		stat_write(ss,s,tmp->sz);
		// 减少写入缓冲区的大小
		s->wb_size -= tmp->sz;
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}

// 将写缓冲区的数据发送出去
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_lock *l, struct socket_message *result) {
	// 根据不同的协议发送
	if (s->protocol == PROTOCOL_TCP) {
		return send_list_tcp(ss, s, list, l, result);
	} else {
		return send_list_udp(ss, s, list, result);
	}
}

// 检查写入缓冲区的头结点是否完整
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}

// socket的高优先级写入缓冲区为空，将低优先级的头结点移到高优先级列表
static void
raise_uncomplete(struct socket * s) {
	struct wb_list *low = &s->low;
	struct write_buffer *tmp = low->head;
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	// 将低优先级的头结点移到高优先级列表
	struct wb_list *high = &s->high;
	assert(high->head == NULL);

	tmp->next = NULL;
	high->head = high->tail = tmp;
}

// 发送缓冲区是否为空
static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */
/*
	每个socket有两个写入缓存列表，高优先级和低优先级
	1.高优先级列表尽可能快的发送。
	2.如果高优先级为空，尝试发送低优先级列表
	3.如果低优先级列表的头结点是不完整的（上次发送了一部分），将低优先节点移动到高优先级列表（调用raise_uncomplete）
	4.如果两个列表都是空的，关闭事件（调用check_close）
*/
static int
send_buffer_(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	assert(!list_uncomplete(&s->low));
	// step 1
	// 高优先级列表尽可能快的发送。
	int ret = send_list(ss,s,&s->high,l,result);
	if (ret != -1) {
		if (ret == SOCKET_ERR) {
			// HALFCLOSE_WRITE
			return SOCKET_ERR;
		}
		// SOCKET_RST (ignore)
		return -1;
	}
	// 如果高优先级为空
	if (s->high.head == NULL) {
		// step 2
		// 尝试发送低优先级列表
		if (s->low.head != NULL) {
			int ret = send_list(ss,s,&s->low,l,result);
			if (ret != -1) {
				if (ret == SOCKET_ERR) {
					// HALFCLOSE_WRITE
					return SOCKET_ERR;
				}
				// SOCKET_RST (ignore)
				return -1;
			}
			// step 3
			// 如果低优先级列表的头结点是不完整的（上次发送了一部分），将低优先节点移动到高优先级列表（调用raise_uncomplete）
			if (list_uncomplete(&s->low)) {
				raise_uncomplete(s);
				return -1;
			}
			if (s->low.head)
				return -1;
		} 
		// step 4
		assert(send_buffer_empty(s) && s->wb_size == 0);
		// 如果两个列表都是空的，关闭事件（调用check_close）
		if (s->closing) {
			// finish writing
			force_close(ss, s, l, result);
			return -1;
		}
		// 关闭写
		int err = enable_write(ss, s, false);

		if (err) {
			return report_error(s, result, "disable write failed");
		}

		if(s->warn_size > 0){
			s->warn_size = 0;
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;
			return SOCKET_WARNING;
		}
	}

	return -1;
}

// 发送缓冲区
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	// 直接写堵塞，发送延后
	if (!socket_trylock(l))
		return -1;	// blocked by direct write, send later.
	if (s->dw_buffer) {
		// add direct write buffer before high.head
		// 在high.head加一个直接的写buffer
		struct write_buffer * buf = MALLOC(SIZEOF_TCPBUFFER);
		struct send_object so;
		buf->userobject = send_object_init(ss, &so, (void *)s->dw_buffer, s->dw_size);
		buf->ptr = (char*)so.buffer+s->dw_offset;
		buf->sz = so.sz - s->dw_offset;
		buf->buffer = (void *)s->dw_buffer;
		s->wb_size+=buf->sz;
		// 挂接在高优先级前面
		if (s->high.head == NULL) {
			s->high.head = s->high.tail = buf;
			buf->next = NULL;
		} else {
			buf->next = s->high.head;
			s->high.head = buf;
		}
		s->dw_buffer = NULL;
	}

	// 发送缓冲区
	int r = send_buffer_(ss,s,l,result);
	socket_unlock(l);

	return r;
}

// 在发送缓冲区后面增加
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size) {
	struct write_buffer * buf = MALLOC(size);
	struct send_object so;
	// 创建发送的object
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	buf->ptr = (char*)so.buffer;
	buf->sz = so.sz;
	buf->buffer = request->buffer;
	buf->next = NULL;
	// 附加在后面
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}
	return buf;
}

// UDP的方式，在发送缓冲区后面增加
static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER);
	// 拷贝发送地址
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	s->wb_size += buf->sz;
}

// 在高优先级发送缓冲区后增加（TCPBUFFER）
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER);
	s->wb_size += buf->sz;
}

// 在低优先级发送缓冲区后增加（TCPBUFFER）
static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER);
	s->wb_size += buf->sz;
}

// 触发写入
static int
trigger_write(struct socket_server *ss, struct request_send * request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	// socket是否可用
	if (socket_invalid(s, id))
		return -1;
	// 打开可写
	if (enable_write(ss, s, true)) {
		return report_error(s, result, "enable write failed");
	}
	return -1;
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */
// 当发送一个数据包，我们可以设置优先级：高优先级或低优先级
// 如果socket的缓冲区为空，直接通过句柄写入
// 如果只写了一部分，将剩余的部分增加在高优先级中（虽然优先级可能是低优先级）
// 或者就将数据包增加在高优先级或者低优先级列表
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	struct send_object so;
	// 初始化发送对象
	send_object_init(ss, &so, request->buffer, request->sz);
	uint8_t type = ATOM_LOAD(&s->type);
	// 如果socket的状态已经不正常，就不发送了
	if (type == SOCKET_TYPE_INVALID || s->id != id
		|| type == SOCKET_TYPE_HALFCLOSE_WRITE
		|| type == SOCKET_TYPE_PACCEPT
		|| s->closing) {
		so.free_func((void *)request->buffer);
		return -1;
	}

	// 如果是侦听socket，不能发送数据
	if (type == SOCKET_TYPE_PLISTEN || type == SOCKET_TYPE_LISTEN) {
		skynet_error(NULL, "socket-server: write to listen fd %d.", id);
		so.free_func((void *)request->buffer);
		return -1;
	}
	// 发送缓冲区是否为空
	if (send_buffer_empty(s)) {
		// 如果是TCP，直接加入到高优先级队列后面
		if (s->protocol == PROTOCOL_TCP) {
			append_sendbuffer(ss, s, request);	// add to high priority list, even priority == PRIORITY_LOW
		} else {
			// udp
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			if (sasz == 0) {
				// udp type mismatch, just drop it.
				skynet_error(NULL, "socket-server: udp socket (%d) type mismatch.", id);
				so.free_func((void *)request->buffer);
				return -1;
			}
			// 直接发送
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
			// 没有发送完全，就加入到对应的优先级队列中
			if (n != so.sz) {
				append_sendbuffer_udp(ss,s,priority,request,udp_address);
			} else {
				stat_write(ss,s,n);
				so.free_func((void *)request->buffer);
				return -1;
			}
		}
		// 将socket设置为可写
		if (enable_write(ss, s, true)) {
			return report_error(s, result, "enable write failed");
		}
	} else {
		// 根据不同的协议加入不同的队列
		if (s->protocol == PROTOCOL_TCP) {
			if (priority == PRIORITY_LOW) {
				append_sendbuffer_low(ss, s, request);
			} else {
				append_sendbuffer(ss, s, request);
			}
		} else {
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			append_sendbuffer_udp(ss,s,priority,request,udp_address);
		}
	}
	if (s->wb_size >= WARNING_SIZE && s->wb_size >= s->warn_size) {
		s->warn_size = s->warn_size == 0 ? WARNING_SIZE *2 : s->warn_size*2;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = s->wb_size%1024 == 0 ? s->wb_size/1024 : s->wb_size/1024 + 1;
		result->data = NULL;
		return SOCKET_WARNING;
	}
	return -1;
}

// 初始化侦听socket
static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {
	int id = request->id;
	int listen_fd = request->fd;
	// 初始化侦听socket
	struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	// 设置类型为侦听socket
	ATOM_STORE(&s->type , SOCKET_TYPE_PLISTEN);
	return -1;
_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = "reach skynet socket number limit";
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERR;
}

// 没有更多需要发送的数据了
static inline int
nomore_sending_data(struct socket *s) {
	return (send_buffer_empty(s) && s->dw_buffer == NULL && (ATOM_LOAD(&s->sending) & 0xffff) == 0)
		|| (ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_WRITE);
}

// 关闭socket的读
static void
close_read(struct socket_server *ss, struct socket * s, struct socket_message *result) {
	// Don't read socket later
	ATOM_STORE(&s->type , SOCKET_TYPE_HALFCLOSE_READ);
	enable_read(ss,s,false);
	shutdown(s->fd, SHUT_RD);
	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;
}

// socket是否为读状态
static inline int
halfclose_read(struct socket *s) {
	return ATOM_LOAD(&s->type) == SOCKET_TYPE_HALFCLOSE_READ;
}

// SOCKET_CLOSE can be raised (only once) in one of two conditions.
// See https://github.com/cloudwu/skynet/issues/1346 for more discussion.
// 1. close socket by self, See close_socket()
// 2. recv 0 or eof event (close socket by remote), See forward_message_tcp()
// It's able to write data after SOCKET_CLOSE (In condition 2), but if remote is closed, SOCKET_ERR may raised.
// SOCKET_CLOSE只有在下面两种情况之一触发
// 1.自己关闭socket，查看close_socket
// 2.接收到0长度的数据，或者eof事件（socket被对方关闭），查看forward_message_tcp
// 在第二种情况，在socket关闭以后是可以写数据的，但是对方已经关闭，会触发SOCKET_ERR
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	// socket不可用
	if (socket_invalid(s, id)) {
		// The socket is closed, ignore
		return -1;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	// 关闭读
	int shutdown_read = halfclose_read(s);
	// 如果要求关闭，或者没有数据需要发送
	if (request->shutdown || nomore_sending_data(s)) {
		// If socket is SOCKET_TYPE_HALFCLOSE_READ, Do not raise SOCKET_CLOSE again.
		int r = shutdown_read ? -1 : SOCKET_CLOSE;
		// 强制关闭
		force_close(ss,s,&l,result);
		return r;
	}

	// 设置socket为关闭中
	s->closing = true;
	if (!shutdown_read) {
		// don't read socket after socket.close()
		close_read(ss, s, result);
		return SOCKET_CLOSE;
	}
	// recv 0 before (socket is SOCKET_TYPE_HALFCLOSE_READ) and waiting for sending data out.
	return -1;
}

// 初始化一个绑定socket
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	// 初始化一个绑定socket
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERR;
	}
	// 设置socket为非阻塞
	sp_nonblocking(request->fd);
	ATOM_STORE(&s->type , SOCKET_TYPE_BIND);
	result->data = "binding";
	return SOCKET_OPEN;
}

// 恢复socket
static int
resume_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	struct socket *s = &ss->slot[HASH_ID(id)];
	// socket是否可用
	if (socket_invalid(s, id)) {
		result->data = "invalid socket";
		return SOCKET_ERR;
	}
	// socket是否为关闭读的状态
	if (halfclose_read(s)) {
		// The closing socket may be in transit, so raise an error. See https://github.com/cloudwu/skynet/issues/1374
		result->data = "socket closed";
		return SOCKET_ERR;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	// 如果socket不可读，就设置为可读
	if (enable_read(ss, s, true)) {
		result->data = "enable read failed";
		return SOCKET_ERR;
	}
	uint8_t type = ATOM_LOAD(&s->type);
	if (type == SOCKET_TYPE_PACCEPT || type == SOCKET_TYPE_PLISTEN) {
		ATOM_STORE(&s->type , (type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN);
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;
	} else if (type == SOCKET_TYPE_CONNECTED) {
		// todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	// if s->type == SOCKET_TYPE_HALFCLOSE_WRITE , SOCKET_CLOSE message will send later
	return -1;
}

// 暂停socket
static int
pause_socket(struct socket_server *ss, struct request_resumepause *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return -1;
	}
	// 将socket设置为不可读
	if (enable_read(ss, s, false)) {
		return report_error(s, result, "enable read failed");
	}
	return -1;
}

// 设置socekt的选项
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return;
	}
	int v = request->value;
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}

// 从管道阻塞读
static void
block_readpipe(int pipefd, void *buffer, int sz) {
	for (;;) {
		int n = read(pipefd, buffer, sz);
		if (n<0) {
			if (errno == EINTR)
				continue;
			skynet_error(NULL, "socket-server : read pipe error %s.",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}

// 是否有命令过来
static int
has_cmd(struct socket_server *ss) {
	struct timeval tv = {0,0};
	int retval;

	FD_SET(ss->recvctrl_fd, &ss->rfds);

	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);
	if (retval == 1) {
		return 1;
	}
	return 0;
}

// 加入upd的socket
static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;
	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	// 新的socket
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}
	// 设置为已连接
	ATOM_STORE(&ns->type , SOCKET_TYPE_CONNECTED);
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}

// 设置udp地址
static int
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return -1;
	}
	int type = request->address[0];
	if (type != s->protocol) {
		// protocol mismatch
		return report_error(s, result, "protocol mismatch");
	}

	// 设置地址
	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	ATOM_FDEC(&s->udpconnecting);
	return -1;
}

// sending自增长
static inline void
inc_sending_ref(struct socket *s, int id) {
	if (s->protocol != PROTOCOL_TCP)
		return;
	for (;;) {
		unsigned long sending = ATOM_LOAD(&s->sending);
		if ((sending >> 16) == ID_TAG16(id)) {
			// sending可能溢出（但是很少会这样），必须在这等到socket线程减少它
			if ((sending & 0xffff) == 0xffff) {
				// s->sending may overflow (rarely), so busy waiting here for socket thread dec it. see issue #794
				continue;
			}
			// inc sending only matching the same socket id
			// 只有匹配相同的socket id的才会自增sending
			if (ATOM_CAS(&s->sending, sending, sending + 1))
				return;
			// atom inc failed, retry
		} else {
			// socket id changed, just return
			return;
		}
	}
}

// sending的自减
static inline void
dec_sending_ref(struct socket_server *ss, int id) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	// Notice: udp may inc sending while type == SOCKET_TYPE_RESERVE
	if (s->id == id && s->protocol == PROTOCOL_TCP) {
		assert((ATOM_LOAD(&s->sending) & 0xffff) != 0);
		ATOM_FDEC(&s->sending);
	}
}

// return type
// 控制命令的处理函数，返回处理的类型
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {
	int fd = ss->recvctrl_fd;
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];
	// 阻塞读取管道得到，控制命令的类型和长度
	block_readpipe(fd, header, sizeof(header));
	int type = header[0];
	int len = header[1];
	// 读取消息内容
	block_readpipe(fd, buffer, len);
	// ctrl command only exist in local fd, so don't worry about endian.
	switch (type) {
	case 'R':
		return resume_socket(ss,(struct request_resumepause *)buffer, result);
	case 'S':
		return pause_socket(ss,(struct request_resumepause *)buffer, result);
	case 'B':
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':
		return open_socket(ss, (struct request_open *)buffer, result);
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'W':
		return trigger_write(ss, (struct request_send *)buffer, result);
	case 'D':
	case 'P': {
		int priority = (type == 'D') ? PRIORITY_HIGH : PRIORITY_LOW;
		struct request_send * request = (struct request_send *) buffer;
		int ret = send_socket(ss, request, result, priority, NULL);
		dec_sending_ref(ss, request->id);
		return ret;
	}
	case 'A': {
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}
	case 'C':
		return set_udp_address(ss, (struct request_setudp *)buffer, result);
	case 'T':
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
	case 'U':
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		skynet_error(NULL, "socket-server: Unknown ctrl %c.",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
// 处理tcp消息
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message * result) {
	int sz = s->p.size;
	// 分配读取的内存
	char * buffer = MALLOC(sz);
	// 从socket读取数据
	int n = (int)read(s->fd, buffer, sz);
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
		case AGAIN_WOULDBLOCK:
			break;
		// 报错
		default:
			return report_error(s, result, strerror(errno));
		}
		// 其他两种情况返回-1
		return -1;
	}
	// 读取到0的时候，一般都是对方已经关闭
	if (n==0) {
		FREE(buffer);
		// 如果已经是关闭中了
		if (s->closing) {
			// Rare case : if s->closing is true, reading event is disable, and SOCKET_CLOSE is raised.
			// 如果没有数据要发送了，就直接关了
			if (nomore_sending_data(s)) {
				force_close(ss,s,l,result);
			}
			return -1;
		}
		int t = ATOM_LOAD(&s->type);
		if (t == SOCKET_TYPE_HALFCLOSE_READ) {
			// Rare case : Already shutdown read.
			return -1;
		}
		// 如果是半关闭状态，就彻底关闭
		if (t == SOCKET_TYPE_HALFCLOSE_WRITE) {
			// Remote shutdown read (write error) before.
			force_close(ss,s,l,result);
		} else {
			// 将读关闭了
			close_read(ss, s, result);
		}
		return SOCKET_CLOSE;
	}

	// 如果是读关闭状态，直接将受到的数据抛弃
	if (halfclose_read(s)) {
		// discard recv data (Rare case : if socket is HALFCLOSE_READ, reading event is disable.)
		FREE(buffer);
		return -1;
	}

	// 读的数据统计
	stat_read(ss,s,n);

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = buffer;

	// 根据这次读取的数据长度，调整下次预分配的读取内存大小
	if (n == sz) {
		s->p.size *= 2;
		return SOCKET_MORE;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->p.size /= 2;
	}

	return SOCKET_DATA;
}

// 将Udp的地址序列化到数据流
static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address+addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address+addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address+addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address+addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

// 处理upd消息
static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message * result) {
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);
	// 从udp接收数据
	int n = recvfrom(s->fd, ss->udpbuffer,MAX_UDP_PACKAGE,0,&sa.s,&slen);
	if (n<0) {
		switch(errno) {
		case EINTR:
		case AGAIN_WOULDBLOCK:
			return -1;
		}
		int error = errno;
		// close when error
		// 出现错误直接关闭
		force_close(ss, s, l, result);
		result->data = strerror(error);
		return SOCKET_ERR;
	}
	// 读取统计
	stat_read(ss,s,n);

	uint8_t * data;
	if (slen == sizeof(sa.v4)) {
		if (s->protocol != PROTOCOL_UDP)
			return -1;
		data = MALLOC(n + 1 + 2 + 4);
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		// 将地址放在最后面
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}
	// 将数据拷贝到最前面
	memcpy(data, ss->udpbuffer, n);

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = (char *)data;

	return SOCKET_UDP;
}

// 查看连接的信息（是否出错，或者完全连接完成）
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_lock *l, struct socket_message *result) {
	int error;
	socklen_t len = sizeof(error);  
	// 得到错误
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
	if (code < 0 || error) {  
		error = code < 0 ? errno : error;
		// 出错了就强制关闭
		force_close(ss, s, l, result);
		result->data = strerror(error);
		return SOCKET_ERR;
	} else {
		ATOM_STORE(&s->type , SOCKET_TYPE_CONNECTED);
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		// 没有更多需要发送的数据
		if (nomore_sending_data(s)) {
			// 关闭读
			if (enable_write(ss, s, false)) {
				force_close(ss,s,l, result);
				result->data = "disable write failed";
				return SOCKET_ERR;
			}
		}
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		// 得到对方的信息
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			// 将数值格式转化为点分十进制的ip地址格式
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// 将地址的信息转换成点分十进制的ip地址格式，保存在buffer中
static int
getname(union sockaddr_all *u, char *buffer, size_t sz) {
	char tmp[INET6_ADDRSTRLEN];
	void * sin_addr = (u->s.sa_family == AF_INET) ? (void*)&u->v4.sin_addr : (void *)&u->v6.sin6_addr;
	// 将地址转换为点分十进制的地址格式
	if (inet_ntop(u->s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		int sin_port = ntohs((u->s.sa_family == AF_INET) ? u->v4.sin_port : u->v6.sin6_port);
		snprintf(buffer, sz, "%s:%d", tmp, sin_port);
		return 1;
	} else {
		buffer[0] = '\0';
		return 0;
	}
}

// return 0 when failed, or -1 when file limit
// 失败返回0，或者如果是文件数量限制就返回-1
// 返回接受连接的信息
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	// 接受连接
	int client_fd = accept(s->fd, &u.s, &len);
	// 出错处理
	if (client_fd < 0) {
		// 达到了文件的限制
		if (errno == EMFILE || errno == ENFILE) {
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = strerror(errno);
			return -1;
		} else {
			return 0;
		}
	}
	// 分配一个id
	int id = reserve_id(ss);
	if (id < 0) {
		close(client_fd);
		return 0;
	}
	// 设置socket的keepalive和非阻塞选项
	socket_keepalive(client_fd);
	sp_nonblocking(client_fd);
	// 初始化socket信息
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}
	// accept new one connection
	// 写入统计信息
	stat_read(ss,s,1);

	ATOM_STORE(&ns->type , SOCKET_TYPE_PACCEPT);
	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = id;
	result->data = NULL;
	// 得到对方的地址信息并序列化到buffer中
	if (getname(&u, ss->buffer, sizeof(ss->buffer))) {
		result->data = ss->buffer;
	}

	return 1;
}

// 清除关闭事件
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERR) {
		int id = result->id;
		int i;
		// 事件处理
		for (i=ss->event_index; i<ss->event_n; i++) {
			struct event *e = &ss->ev[i];
			struct socket *s = e->s;
			if (s) {
				if (socket_invalid(s, id) && s->id == id) {
					e->s = NULL;
					break;
				}
			}
		}
	}
}

// return type
// socket服务器的poll操作
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {
	for (;;) {
		// 检查命令控制
		if (ss->checkctrl) {
			// 如果有命令数据
			if (has_cmd(ss)) {
				// 控制命令的处理函数，从管道中读取数据，然后进行处理
				int type = ctrl_cmd(ss, result);
				// 如果控制命令是关闭socket或者socket出错，将事件集合里面的socket清除掉
				if (type != -1) {
					clear_closed_event(ss, result, type);
					return type;
				} else
					continue;
			} else {
				ss->checkctrl = 0;
			}
		}
		// 如果处理的事件索引已经到最大，表示已经处理完了
		if (ss->event_index == ss->event_n) {
			// 取事件集合中的事件
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			// 再一次开启检查命令的标志
			ss->checkctrl = 1;
			if (more) {
				*more = 0;
			}
			// 重置事件索引
			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				int err = errno;
				// 出现错误，报错
				if (err != EINTR) {
					skynet_error(NULL, "socket-server: %s", strerror(err));
				}
				continue;
			}
		}

		// 取出事件
		struct event *e = &ss->ev[ss->event_index++];
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}
		struct socket_lock l;
		socket_lock_init(s, &l);
		// 根据socket的状态
		switch (ATOM_LOAD(&s->type)) {
		// 如果是正在连接中，查看是否已经连接完成或者出错
		case SOCKET_TYPE_CONNECTING:
			return report_connect(ss, s, &l, result);
		// 如果是侦听socket，接受连接
		case SOCKET_TYPE_LISTEN: {
			int ok = report_accept(ss, s, result);
			if (ok > 0) {
				return SOCKET_ACCEPT;
			} if (ok < 0 ) {
				return SOCKET_ERR;
			}
			// when ok == 0, retry
			break;
		}
		case SOCKET_TYPE_INVALID:
			skynet_error(NULL, "socket-server: invalid socket");
			break;
		default:
			// 可读
			if (e->read) {
				int type;
				// 根据不同的协议，使用不同的读取函数
				if (s->protocol == PROTOCOL_TCP) {
					// tcp读取数据
					type = forward_message_tcp(ss, s, &l, result);
					if (type == SOCKET_MORE) {
						--ss->event_index;
						return SOCKET_DATA;
					}
				} else {
					// Udp读取数据
					type = forward_message_udp(ss, s, &l, result);
					if (type == SOCKET_UDP) {
						// try read again
						--ss->event_index;
						return SOCKET_UDP;
					}
				}
				if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERR) {
					// Try to dispatch write message next step if write flag set.
					e->read = false;
					--ss->event_index;
				}
				if (type == -1)
					break;				
				return type;
			}
			// 可写
			if (e->write) {
				// 发送写缓冲区
				int type = send_buffer(ss, s, &l, result);
				if (type == -1)
					break;
				return type;
			}
			// 出错了
			if (e->error) {
				int error;
				socklen_t len = sizeof(error);  
				int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  
				const char * err = NULL;
				if (code < 0) {
					err = strerror(errno);
				} else if (error != 0) {
					err = strerror(error);
				} else {
					err = "Unknown error";
				}
				// 报错
				return report_error(s, result, err);
			}
			// 结束了
			if (e->eof) {
				// For epoll (at least), FIN packets are exchanged both ways.
				// See: https://stackoverflow.com/questions/52976152/tcp-when-is-epollhup-generated
				int halfclose = halfclose_read(s);
				force_close(ss, s, &l, result);
				if (!halfclose) {
					return SOCKET_CLOSE;
				}
			}
			break;
		}
	}
}

// 发送请求
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	// 设置类型和长度
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {
		// 写入，header后面就是内容
		ssize_t n = write(ss->sendctrl_fd, &request->header[6], len+2);
		if (n<0) {
			if (errno != EINTR) {
				skynet_error(NULL, "socket-server : send ctrl command error %s.", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}

// open socket的请求，将参数组成一个请求消息
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);
	if (len + sizeof(req->u.open) >= 256) {
		skynet_error(NULL, "socket-server : Invalid addr %s.",addr);
		return -1;
	}
	// 分配一个id
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;
	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

// 连接服务器的请求
int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);
	return request.u.open.id;
}

// 是否能直接写入，不需要先放到缓冲区里面去
static inline int
can_direct_write(struct socket *s, int id) {
	return s->id == id && nomore_sending_data(s) && ATOM_LOAD(&s->type) == SOCKET_TYPE_CONNECTED && ATOM_LOAD(&s->udpconnecting) == 0;
}

// return -1 when error, 0 when success
// 服务器发送数据
int 
socket_server_send(struct socket_server *ss, struct socket_sendbuffer *buf) {
	int id = buf->id;
	// 得到对应的socket
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id) || s->closing) {
		free_buffer(ss, buf);
		return -1;
	}

	struct socket_lock l;
	socket_lock_init(s, &l);

	// 是否可用直接写入
	if (can_direct_write(s,id) && socket_trylock(&l)) {
		// may be we can send directly, double check
		// 再一次检查是否可用直接写入
		if (can_direct_write(s,id)) {
			// send directly
			struct send_object so;
			// 初始化发送Obj
			send_object_init_from_sendbuffer(ss, &so, buf);
			ssize_t n;
			if (s->protocol == PROTOCOL_TCP) {
				// 直接写入
				n = write(s->fd, so.buffer, so.sz);
			} else {
				union sockaddr_all sa;
				// 得到udp的地址
				socklen_t sasz = udp_socket_address(s, s->p.udp_address, &sa);
				if (sasz == 0) {
					skynet_error(NULL, "socket-server : set udp (%d) address first.", id);
					socket_unlock(&l);
					so.free_func((void *)buf->buffer);
					return -1;
				}
				// 直接发送
				n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
			}
			if (n<0) {
				// ignore error, let socket thread try again
				n = 0;
			}
			// 统计写入
			stat_write(ss,s,n);
			if (n == so.sz) {
				// write done
				// 写入成功，将buff释放
				socket_unlock(&l);
				so.free_func((void *)buf->buffer);
				return 0;
			}
			// write failed, put buffer into s->dw_* , and let socket thread send it. see send_buffer()
			// 如果写入失败了，将buff放到dw_buffer
			s->dw_buffer = clone_buffer(buf, &s->dw_size);
			s->dw_offset = n;

			socket_unlock(&l);

			inc_sending_ref(s, id);

			struct request_package request;
			request.u.send.id = id;
			request.u.send.sz = 0;
			request.u.send.buffer = NULL;

			// let socket thread enable write event
			// 通知socket线程打开写事件
			send_request(ss, &request, 'W', sizeof(request.u.send));

			return 0;
		}
		socket_unlock(&l);
	}

	inc_sending_ref(s, id);

	struct request_package request;
	request.u.send.id = id;
	request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);

	send_request(ss, &request, 'D', sizeof(request.u.send));
	return 0;
}

// return -1 when error, 0 when success
// 服务器发送低优先级队列
// 发送低优先队列请求
int 
socket_server_send_lowpriority(struct socket_server *ss, struct socket_sendbuffer *buf) {
	int id = buf->id;

	// 找到socket，检验socket是否可用
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		free_buffer(ss, buf);
		return -1;
	}

	inc_sending_ref(s, id);

	struct request_package request;
	request.u.send.id = id;
	request.u.send.buffer = clone_buffer(buf, &request.u.send.sz);

	send_request(ss, &request, 'P', sizeof(request.u.send));
	return 0;
}

// 发送服务器退出的请求
void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}

// 发送服务器关闭socket的请求
void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 0;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// 发送服务器停止socket的请求
void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 1;
	request.u.close.opaque = opaque;
	send_request(ss, &request, 'K', sizeof(request.u.close));
}

// return -1 means failed
// or return AF_INET or AF_INET6
static int
do_bind(const char *host, int port, int protocol, int *family) {
	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	// 如果没有传入IP，就是使用INADDR_ANY，也就是“0.0.0.0”
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY
	}
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	// 得到地址
	status = getaddrinfo( host, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	// 创建socket
	*family = ai_list->ai_family;
	fd = socket(*family, ai_list->ai_socktype, 0);
	if (fd < 0) {
		goto _failed_fd;
	}
	// 设置为可重用
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}
	// 绑定socket
	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
	if (status != 0)
		goto _failed;

	freeaddrinfo( ai_list );
	return fd;
_failed:
	close(fd);
_failed_fd:
	freeaddrinfo( ai_list );
	return -1;
}

// 侦听
static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
	// 绑定地址
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);
	if (listen_fd < 0) {
		return -1;
	}

	// 侦听
	if (listen(listen_fd, backlog) == -1) {
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}

// socket server侦听
int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	// 发送侦听的请求
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return id;
	}
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;
	send_request(ss, &request, 'L', sizeof(request.u.listen));
	return id;
}

// socket绑定的请求
int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}

// 设置一个恢复socket的请求
void
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.resumepause.id = id;
	request.u.resumepause.opaque = opaque;
	send_request(ss, &request, 'R', sizeof(request.u.resumepause));
}

// 设置一个Socket开启的请求
void
socket_server_pause(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.resumepause.id = id;
	request.u.resumepause.opaque = opaque;
	send_request(ss, &request, 'S', sizeof(request.u.resumepause));
}
// 设置socket的选项的请求
void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}

// 设置服务器的实体接口
void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP
// UDP服务器创建
int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	if (port != 0 || addr != NULL) {
		// bind
		// 绑定端口
		fd = do_bind(addr, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {
		family = AF_INET;
		// 创建一个UDP的socket
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}
	sp_nonblocking(fd);

	// 分配ID
	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return -1;
	}

	// 创建UDP的请求
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;

	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

// udp发送
int 
socket_server_udp_send(struct socket_server *ss, const struct socket_udp_address *addr, struct socket_sendbuffer *buf) {
	int id = buf->id;
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		free_buffer(ss, buf);
		return -1;
	}

	// 地址
	const uint8_t *udp_address = (const uint8_t *)addr;
	int addrsz;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6
		break;
	default:
		free_buffer(ss, buf);
		return -1;
	}

	struct socket_lock l;
	socket_lock_init(s, &l);
	// 是否可用直接写入
	if (can_direct_write(s,id) && socket_trylock(&l)) {
		// may be we can send directly, double check
		// 再一次检查是否可用直接写入
		if (can_direct_write(s,id)) {
			// send directly
			struct send_object so;
			send_object_init_from_sendbuffer(ss, &so, buf);
			union sockaddr_all sa;
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);
			if (sasz == 0) {
				socket_unlock(&l);
				so.free_func((void *)buf->buffer);
				return -1;
			}
			// 直接发送
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);
			if (n >= 0) {
				// sendto succ
				stat_write(ss,s,n);
				socket_unlock(&l);
				so.free_func((void *)buf->buffer);
				return 0;
			}
		}
		socket_unlock(&l);
		// let socket thread try again, udp doesn't care the order
	}

	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.buffer = clone_buffer(buf, &request.u.send_udp.send.sz);

	memcpy(request.u.send_udp.address, udp_address, addrsz);
	// 发送发送UPD数据包的请求
	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return 0;
}

// UDP连接
int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (socket_invalid(s, id)) {
		return -1;
	}
	struct socket_lock l;
	socket_lock_init(s, &l);
	socket_lock(&l);
	// socket是否可用
	if (socket_invalid(s, id)) {
		socket_unlock(&l);
		return -1;
	}
	ATOM_FINC(&s->udpconnecting);
	socket_unlock(&l);

	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;

	// 得到地址
	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}

	// 将UDP地址序列化到BUFF中
	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );
	// 发送设置UDP地址的请求
	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) +addrsz);

	return 0;
}

// 从消息中解析出UDP地址的长度和地址内容
const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}

// 创建一个socket_info，挂接在last前面
struct socket_info *
socket_info_create(struct socket_info *last) {
	struct socket_info *si = skynet_malloc(sizeof(*si));
	memset(si, 0 , sizeof(*si));
	si->next = last;
	return si;
}

// 释放si以及后面的节点
void
socket_info_release(struct socket_info *si) {
	while (si) {
		struct socket_info *temp = si;
		si = si->next;
		skynet_free(temp);
	}
}

// 查询socket的信息，写入socket_info中
static int
query_info(struct socket *s, struct socket_info *si) {
	union sockaddr_all u;
	socklen_t slen = sizeof(u);
	int closing = 0;
	switch (ATOM_LOAD(&s->type)) {
	case SOCKET_TYPE_BIND:
		si->type = SOCKET_INFO_BIND;
		si->name[0] = '\0';
		break;
	case SOCKET_TYPE_LISTEN:
		si->type = SOCKET_INFO_LISTEN;
		if (getsockname(s->fd, &u.s, &slen) == 0) {
			getname(&u, si->name, sizeof(si->name));
		}
		break;
	case SOCKET_TYPE_HALFCLOSE_READ:
	case SOCKET_TYPE_HALFCLOSE_WRITE:
		closing = 1;
	case SOCKET_TYPE_CONNECTED:
		if (s->protocol == PROTOCOL_TCP) {
			si->type = closing ? SOCKET_INFO_CLOSING : SOCKET_INFO_TCP;
			if (getpeername(s->fd, &u.s, &slen) == 0) {
				getname(&u, si->name, sizeof(si->name));
			}
		} else {
			si->type = SOCKET_INFO_UDP;
			if (udp_socket_address(s, s->p.udp_address, &u)) {
				getname(&u, si->name, sizeof(si->name));
			}
		}
		break;
	default:
		return 0;
	}
	si->id = s->id;
	si->opaque = (uint64_t)s->opaque;
	si->read = s->stat.read;
	si->write = s->stat.write;
	si->rtime = s->stat.rtime;
	si->wtime = s->stat.wtime;
	si->wbuffer = s->wb_size;
	si->reading = s->reading;
	si->writing = s->writing;

	return 1;
}

// 创建socker服务器的所有socket的信息
struct socket_info *
socket_server_info(struct socket_server *ss) {
	int i;
	struct socket_info * si = NULL;
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket * s = &ss->slot[i];
		int id = s->id;
		struct socket_info temp;
		if (query_info(s, &temp) && s->id == id) {
			// socket_server_info may call in different thread, so check socket id again
			si = socket_info_create(si);
			temp.next = si->next;
			*si = temp;
		}
	}
	return si;
}
