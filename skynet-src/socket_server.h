#ifndef skynet_socket_server_h
#define skynet_socket_server_h

#include <stdint.h>
#include "socket_info.h"
#include "socket_buffer.h"
// 数据data到来的消息
#define SOCKET_DATA 0
// 关闭连接的消息
#define SOCKET_CLOSE 1
// 连接成功的消息
#define SOCKET_OPEN 2
// 被动连接成功的消息(Accept返回了连接的fd句柄，但此连接还未被假如epoll中管理)
#define SOCKET_ACCEPT 3
// 错误消息
#define SOCKET_ERR 4
// 退出socket消息
#define SOCKET_EXIT 5
// 接收到UDP数据
#define SOCKET_UDP 6
// 写缓存超出阈值警告
#define SOCKET_WARNING 7

// Only for internal use
#define SOCKET_RST 8
#define SOCKET_MORE 9

struct socket_server;
// socket_message对应于socket_server服务中的消息传输类型
struct socket_message {
	//应用层的socket fd句柄
	int id;
	//在skynet中对应一个Ator实体的handle句柄
	uintptr_t opaque;
	// 对于连接，ud是一个新的连接id，对于数据，ud就是数据的大小
	int ud;	// for accept, ud is new connection id ; for data, ud is size of data 
	// 数据指针
	char * data;
};
// 创建一个socket_server
struct socket_server * socket_server_create(uint64_t time);
// 释放一个socket_server的资源占用
void socket_server_release(struct socket_server *);
void socket_server_updatetime(struct socket_server *, uint64_t time);
// 封装了的epoll或kqueue，用来获取socket的网络事件或消息
int socket_server_poll(struct socket_server *, struct socket_message *result, int *more);
// 退出socket_server
void socket_server_exit(struct socket_server *);
// 关闭socket_server
void socket_server_close(struct socket_server *, uintptr_t opaque, int id);
// 停止socket
void socket_server_shutdown(struct socket_server *, uintptr_t opaque, int id);
// 启动socket监听（启动之前要先通过socket_server_listen()绑定端口）
void socket_server_start(struct socket_server *, uintptr_t opaque, int id);
void socket_server_pause(struct socket_server *, uintptr_t opaque, int id);

// return -1 when error
// 发送数据
int socket_server_send(struct socket_server *, struct socket_sendbuffer *buffer);
int socket_server_send_lowpriority(struct socket_server *, struct socket_sendbuffer *buffer);

// ctrl command below returns id
// 绑定监听ip端口
int socket_server_listen(struct socket_server *, uintptr_t opaque, const char * addr, int port, int backlog);
// 以非阻塞的方式连接服务器
int socket_server_connect(struct socket_server *, uintptr_t opaque, const char * addr, int port);
// 并不对应bind函数，而是将stdin、stout这类IO加入到epoll中管理
int socket_server_bind(struct socket_server *, uintptr_t opaque, int fd);

// for tcp
void socket_server_nodelay(struct socket_server *, int id);

struct socket_udp_address;

// create an udp socket handle, attach opaque with it . udp socket don't need call socket_server_start to recv message
// if port != 0, bind the socket . if addr == NULL, bind ipv4 0.0.0.0 . If you want to use ipv6, addr can be "::" and port 0.
/*
* 创建一个udp socket监听，并绑定skynet服务的handle，udp不需要像tcp那样要调用socket_server_start后才能接收消息
* 如果port != 0, 绑定socket，如果addr == NULL, 绑定 ipv4 0.0.0.0。如果想要使用ipv6，地址使用“::”，端口中port设为0
*/
int socket_server_udp(struct socket_server *, uintptr_t opaque, const char * addr, int port);
// set default dest address, return 0 when success
// 设置默认的目标地址，返回0表示成功
int _connect(struct socket_server *, int id, const char * addr, int port);
// If the socket_udp_address is NULL, use last call socket_server_udp_connect address instead
// You can also use socket_server_send 
/*
* 假如 socket_udp_address 是空的, 使用最后最后调用 socket_server_udp_connect 时传入的address代替
* 也可以使用 socket_server_send 来发送udp数据
*/
int socket_server_udp_send(struct socket_server *, const struct socket_udp_address *, struct socket_sendbuffer *buffer);
// extract the address of the message, struct socket_message * should be SOCKET_UDP
// 获取传入消息的IP地址 address, 传入的 socket_message * 必须是SOCKET_UDP类型
const struct socket_udp_address * socket_server_udp_address(struct socket_server *, struct socket_message *, int *addrsz);

struct socket_object_interface {
	const void * (*buffer)(const void *);
	size_t (*size)(const void *);
	void (*free)(void *);
};

// if you send package with type SOCKET_BUFFER_OBJECT, use soi.
void socket_server_userobject(struct socket_server *, struct socket_object_interface *soi);

struct socket_info * socket_server_info(struct socket_server *);

#endif
