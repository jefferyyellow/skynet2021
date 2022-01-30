#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

struct timer_event {
	uint32_t handle;
	int session;
};

// 单个定时器节点
struct timer_node {
	struct timer_node *next;
	uint32_t expire;
};

//定时器链表
struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};
// 在skynet里，时间精度是0.01秒，这对于游戏服务器来说已经足够了，定义1滴答 = 0.01秒，1秒 = 100滴答。
// 其核心思想是：每个定时器设置一个到期的滴答数，与当前系统的滴答数(启动时是0，然后1滴答1滴答往后跳)比较差值，
// 如果差值interval比较小（0 <= interval <= 2 ^ 8 - 1），表示定时器即将到来，需要严格关注，
// 把它们保存在2 ^ 8个定时器链表里；如果interval越大，表示定时器越远，可以不用太关注，
// 划分成4个等级，2 ^ 8 <= interval <= 2 ^ (8 + 6) - 1，2 ^ (8 + 6) <= interval <= 2 ^ (8 + 6 + 6)，...，
// 每个等级只需要2 ^ 6个定时器链表保存，比如对于2 ^ 8 <= interval <= 2 ^ (8 + 6) - 1的定时器，
// 将interval >> 8相同的值idx保存在第一个等级位置为idx的链表里。
// 这样做的优势是：不用为每一个interval创建一个链表，而只需要2 ^ 8 + 4 * (2 ^ 6)个链表，大大节省了内存。
struct timer {
	// 保存2^8个即将到来的定时器链表
	struct link_list near[TIME_NEAR];
	// 四个等级的计时器列表
	struct link_list t[4][TIME_LEVEL];
	struct spinlock lock;
	//启动到现在走过的滴答数，等同于current
	uint32_t time;
	// 开始的系统时间，单位为秒
	uint32_t starttime;
	// current是starttime到现在的时间经历的百分之一秒的时间数：单位:百分之一秒
	uint64_t current;
	// 得到系统启动到现在的时间,单位:百分之一秒
	uint64_t current_point;
};

static struct timer * TI = NULL;

// 清空list链表
static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;
	uint32_t current_time=T->time;
	
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
		link(&T->near[time&TIME_NEAR_MASK],node);
	} else {
		int i;
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}

		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}

static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);
	memcpy(node+1,arg,sz);

	SPIN_LOCK(T);

		node->expire=time+T->time;
		add_node(T,node);

	SPIN_UNLOCK(T);
}

static void
move_list(struct timer *T, int level, int idx) {
	struct timer_node *current = link_clear(&T->t[level][idx]);
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);
		current=temp;
	}
}

static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;
	uint32_t ct = ++T->time;
	if (ct == 0) {
		move_list(T, 3, 0);
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i=0;

		while ((ct & (mask-1))==0) {
			int idx=time & TIME_LEVEL_MASK;
			if (idx!=0) {
				move_list(T, i, idx);
				break;				
			}
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}

static inline void
dispatch_list(struct timer_node *current) {
	do {
		struct timer_event * event = (struct timer_event *)(current+1);
		struct skynet_message message;
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		skynet_context_push(event->handle, &message);
		
		struct timer_node * temp = current;
		current=current->next;
		skynet_free(temp);	
	} while (current);
}

static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;
	
	while (T->near[idx].head.next) {
		struct timer_node *current = link_clear(&T->near[idx]);
		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		dispatch_list(current);
		SPIN_LOCK(T);
	}
}

static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	timer_execute(T);

	// shift time first, and then dispatch timer message
	timer_shift(T);

	timer_execute(T);

	SPIN_UNLOCK(T);
}

// 创建一个全局的时间管理结构
static struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;
	// 将即将到来的定时器链表清空
	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	// 将分级的各个定时器链表清空
	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	// 初始化自旋锁
	SPIN_INIT(r)

	r->current = 0;

	return r;
}

int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time <= 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second
// CLOCK_REALTIME 是wall time
// walltime字面意思是挂钟时间，实际上就是指的是现实的时间，这是由变量xtime来记录的。
// 系统每次启动时将CMOS上的RTC时间读入xtime，这个值是"自1970-01-01起经历的秒数、本秒中经历的纳秒数"，
// 每来一个timer interrupt，也需要去更新xtime。
static void
systime(uint32_t *sec, uint32_t *cs) {
	struct timespec ti;
	// CLOCK_REALTIME：相对时间，从1970.1.1到目前的时间。更改系统时间会更改获取的值。它以系统时间为坐标。
	// 字面意思: wall time挂钟时间，表示现实的时间，由变量xtime来记录的。
	clock_gettime(CLOCK_REALTIME, &ti);
	// sec记录的是秒
	*sec = (uint32_t)ti.tv_sec;
	// 注意：cs返回的是百分之一秒,cs应该是：centi-:百,百分之一
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
}

// CLOCK_MONOTONIC:从系统启动这一刻起开始计时,不受系统时间被用户改变的影响
static uint64_t
gettime() {
	uint64_t t;
	struct timespec ti;
	//CLOCK_MONOTONIC:从系统启动这一刻起开始计时,不受系统时间被用户改变的影响
	clock_gettime(CLOCK_MONOTONIC, &ti);
	// 注意这个都是计算百分之一秒为单位，使用秒乘以100
	t = (uint64_t)ti.tv_sec * 100;
	// 纳秒除以10的7次方，也是转换为百分之一秒
	t += ti.tv_nsec / 10000000;
	return t;
}

// 更新时间
void
skynet_updatetime(void) {
	// 得到现在的时间点
	uint64_t cp = gettime();
	// 如果现在的时间点比上次的还小，打印错误，并赋值
	if(cp < TI->current_point) {
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} 
	// 如果时间超过百分之一秒
	else if (cp != TI->current_point) {
		uint32_t diff = (uint32_t)(cp - TI->current_point);
		// 更新时间
		TI->current_point = cp;
		// 加上偏差
		TI->current += diff;
		int i;
		// 更新计时器
		for (i=0;i<diff;i++) {
			timer_update(TI);
		}
	}
}

// 
uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

uint64_t 
skynet_now(void) {
	return TI->current;
}

// 初始时计时器
void 
skynet_timer_init(void) {
	// 创建一个全局的时间管理结构
	TI = timer_create_timer();
	uint32_t current = 0;
	// 获取开始的系统时间，注意：starttime的单位是秒，而current的单位是百分之一秒
	systime(&TI->starttime, &current);
	TI->current = current;
	// 得到系统启动到现在的时间，百分之一秒为计时单位
	TI->current_point = gettime();
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

uint64_t
skynet_thread_time(void) {
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
}
