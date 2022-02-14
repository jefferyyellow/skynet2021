#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

#define GLOBALNAME_LENGTH 16
#define REMOTE_MAX 256

struct remote_name {
	char name[GLOBALNAME_LENGTH];
	uint32_t handle;
};

struct remote_message {
	struct remote_name destination;
	const void * message;
	size_t sz;
	int type;
};

// 消息发送给服务器集群
void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);
// 句柄是否为远程句柄，就是集群里面其他服务器的句柄
int skynet_harbor_message_isremote(uint32_t handle);
// 初始化集群服务
void skynet_harbor_init(int harbor);
// 开启集群服务
void skynet_harbor_start(void * ctx);
// 集群服务退出
void skynet_harbor_exit();

#endif
