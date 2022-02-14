#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

// 远程的服务（就是集群链接服务）
static struct skynet_context * REMOTE = 0;
// 每个节点中有一个特殊的服务叫做 harbor (港口) ，当一个消息的目的地址的高 8 位和本节点不同时，消息被投递到 harbor 服务中，
// 它再通过 tcp 连接传输到目的节点的 harbor 服务中。
static unsigned int HARBOR = ~0;

static inline int
invalid_type(int type) {
	return type != PTYPE_SYSTEM && type != PTYPE_HARBOR;
}

// 发送给其他服务器集群
void 
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
	assert(invalid_type(rmsg->type) && REMOTE);
	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg) , source, PTYPE_SYSTEM , session);
}

// 句柄是否为远程句柄，就是集群里面其他服务器的句柄
int 
skynet_harbor_message_isremote(uint32_t handle) {
	assert(HARBOR != ~0);
	// 取高8位的值，低24位置0
	int h = (handle & ~HANDLE_MASK);
	return h != HARBOR && h !=0;
}

// HARBOR的高8位表示节点索引
// HARBOR服务地址是一个32bit整数，同一进程内的地址的高8bit相同。这8bit区分了一个服务处于那个节点。
void
skynet_harbor_init(int harbor) {
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT;
}

void
skynet_harbor_start(void *ctx) {
	// the HARBOR must be reserved to ensure the pointer is valid.
	// It will be released at last by calling skynet_harbor_exit
	skynet_context_reserve(ctx);
	REMOTE = ctx;
}

// 退出集群服务
void
skynet_harbor_exit() {
	struct skynet_context * ctx = REMOTE;
	REMOTE= NULL;
	if (ctx) {
		skynet_context_release(ctx);
	}
}
