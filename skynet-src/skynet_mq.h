#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>

struct skynet_message {
	// 消息发送方的服务地址
	uint32_t source;
    // 如果这是一个回应消息，那么要通过session找回对应的一次请求，在lua层，我们每次调用call的时候，都会往对  
    // 方的消息队列中，push一个消息，并且生成一个session，然后将本地的协程挂起，挂起时，会以session为key，协程句  
    // 柄为值，放入一个table中，当回应消息送达时，通过session找到对应的协程，并将其唤醒。后面章节会详细讨论
	int session;
	// 消息地址
	void * data;
	// 消息大小
	size_t sz;
};

// type is encoding in skynet_message.sz high 8bit
// 消息类型掩码
#define MESSAGE_TYPE_MASK (SIZE_MAX >> 8)
// 消息类型的偏移量
#define MESSAGE_TYPE_SHIFT ((sizeof(size_t)-1) * 8)

struct message_queue;
// 在全局队列中插入一个队列
void skynet_globalmq_push(struct message_queue * queue);
// 从全局队列中弹出消息队列首节点
struct message_queue * skynet_globalmq_pop(void);
// 创建新的消息队列
struct message_queue * skynet_mq_create(uint32_t handle);
void skynet_mq_mark_release(struct message_queue *q);

typedef void (*message_drop)(struct skynet_message *, void *);

void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud);
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
// 从消息队列中弹出一条消息
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);
// 往消息队列中压入一条消息
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

// return the length of message queue, for debug
// 消息队列的长度，从下面可以看出，这是一个环状队列
int skynet_mq_length(struct message_queue *q);
// 返回消息队列是否过载，如果过载，将过载的信息重置
int skynet_mq_overload(struct message_queue *q);
// 初始化全局消息列表
void skynet_mq_init();

#endif
