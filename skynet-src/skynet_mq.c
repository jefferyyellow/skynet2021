#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.
// 0：表示消息队列不在全局消息队列中。
// 1：消息队列在消息队列中，或者消息正在分发
#define MQ_IN_GLOBAL 1
// 消息队列的初始过载数量
#define MQ_OVERLOAD 1024

// 服务消息队列(次级消息队列)
struct message_queue {
	// 自旋锁
	struct spinlock lock;
	// 拥有此消息队列的服务的id
	uint32_t handle;
	// 消息队列大小
	int cap;
	// 消息队列头index
	int head;
	// 消息队列尾index
	int tail;
	// 是否能释放消息
	int release;
	// 是否在全局消息队列中，0表示不是（分发的时候也不在全局队列中），1表示是
	int in_global;
	// 是否过载
	int overload;
	int overload_threshold;
	// 消息队列
	struct skynet_message *queue;
	// 下一个次级消息队列的指针(用于全局消息队列中组成一个单链表)
	struct message_queue *next;
};

// 全局消息队列
struct global_queue {
	// 全局消息队列头(本身是一个次级消息队列)
	struct message_queue *head;
	// 全局消息队列尾(本身是一个次级消息队列)
	struct message_queue *tail;
	// 自旋锁
	struct spinlock lock;
};
// 全局消息队列
static struct global_queue *Q = NULL;

// 将消息队列插入到全局消息队列的尾部
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	// 已经有元素了，将其放在链表尾部
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		// 没有元素，直接链接
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

// 从全局队列中弹出消息队列首节点
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	// 是否有元素
	if(mq) {
		// 首节点脱离链表
		q->head = mq->next;
		if(q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

// 创建新的消息队列
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	// 创建队列时(总是在服务创建和服务初始化之间),
	// 设置 in_global 标志以避免将其推送到全局队列。
	// 如果服务初始化成功,skynet_context_new将调用 skynet_mq_push 将其推送到全局队列。
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

// 释放消息队列
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

// 返回拥有此消息队列的服务的id
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

// 消息队列的长度，从下面可以看出，这是一个环状队列
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	// ----******-------
	//     head tail  cap
	if (head <= tail) {
		return tail - head;
	}
	// ****-------******
	//    tail    head  cap
	return tail + cap - head;
}

// 返回消息队列是否过载，如果过载，将过载的信息重置
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}

// 从消息队列中弹出一条消息
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	SPIN_LOCK(q)

	// 不为空
	if (q->head != q->tail) {
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		// 如果回转了
		if (head >= cap) {
			q->head = head = 0;
		}
		// 计算存在的消息的数量
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}
		// 如果长度达到过载的的阀值，就记录过载的信息，并且将阀值扩大到原来的两倍
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
		// 重置overload_threshold当队列为空时
		q->overload_threshold = MQ_OVERLOAD;
	}

	// 如果没有取到消息，就设置全局标志为0
	if (ret) {
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}

// 扩充消息队列
static void
expand_queue(struct message_queue *q) {
	// 以原来两倍的方式扩充容量
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	// 将原来的消息拷贝到新消息队列的最前面
	int i;
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;
	// 原来的释放掉，设置新的消息队列
	skynet_free(q->queue);
	q->queue = new_queue;
}

// 往消息队列中压入一条消息
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	// 压入消息，并且后移尾指针
	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	// 如果已满，需要扩充消息队列
	if (q->head == q->tail) {
		expand_queue(q);
	}

	// 如果不在全局队列中，需要设置在全局消息队列中的标志，然后压入全局消息队列
	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}
// 初始化全局消息列表
void 
skynet_mq_init() {
	// 分配全局消息列表内存
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	// 初始化全局消息列表的自旋锁
	SPIN_INIT(q);
	Q=q;
}

// 标记为释放消息
void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	// 如果不在全局消息队列中，压入全局消息队列中
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

// 将消息循环弹出，然后调用drop函数，然后释放消息队列
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	while(!skynet_mq_pop(q, &msg)) {
		drop_func(&msg, ud);
	}
	// 释放消息队列
	_release(q);
}

// 释放消息列表
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	// 如果已经标记为释放
	if (q->release) {
		SPIN_UNLOCK(q)
		// 释放消息队列
		_drop_queue(q, drop_func, ud);
	} else {
		// 将消息队列压入全局消息队列
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
