#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

struct monitor {
	// 监视的工作线程数
	int count;
	struct skynet_monitor ** m;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	int sleep;
	int quit;
};

struct worker_parm {
	// 全局监控信息
	struct monitor *m;
	// 工作线程的ID（索引）
	int id;
	// 权重
	int weight;
};

static volatile int SIG = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}
// 是否终止了
#define CHECK_ABORT if (skynet_context_total()==0) break;

// 创建线程，失败打印错误信息并且退出程序
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

// 唤醒线程
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		// 给睡眠的工作线程发信号，“虚假唤醒”是无害的
		pthread_cond_signal(&m->cond);
	}
}

// socket线程函数
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	// 初始化线程类型
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		// 唤醒工作线程
		wakeup(m,0);
	}
	return NULL;
}

// 是否监视信息占用的内存
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	// 释放各个工作线程的监视信息
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	// 释放各个工作线程的监视信息
	skynet_free(m->m);
	// 是否全局监视信息
	skynet_free(m);
}


// 监控线程函数
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	// 初始化为监控线程函数
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		CHECK_ABORT
		// 监控所有的工作线程
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen
	// 日志文件重开
	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}

// 时间线程函数
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	// 初始化时间线程为当前的线程类型
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		// 更新时间
		skynet_updatetime();
		skynet_socket_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	// wakeup socket thread
	// 唤醒socket线程
	skynet_socket_exit();
	// wakeup all worker thread
	// 唤醒所有的工作线程
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

// 工作线程函数
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	// 初始化为工作线程
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		// 消息分发
		q = skynet_context_message_dispatch(sm, q, weight);
		// 返回空，就是没有消息了
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				// 睡觉去吧
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				// 进入等待信号
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

static void
start(int thread) {
	pthread_t pid[thread+3];
	// 分配全局监视需要的内存
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	// 监视的线程数量
	m->count = thread;
	m->sleep = 0;

	// 分配单个线程监视信息所需内存
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}

	// 初始化全局监视的互斥体
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	// 初始化全局监视的条件变量
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 创建监视线程，时间线程和socket线程
	create_thread(&pid[0], thread_monitor, m);
	create_thread(&pid[1], thread_timer, m);
	create_thread(&pid[2], thread_socket, m);

	// 权重
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		// 全局监控信息
		wp[i].m = m;
		// 工作线程id
		wp[i].id = i;
		// 设置权重，超过数组的都设置为0
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		// 创建工作线程
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	// 等待所有的线程退出
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	// 是否监视
	free_monitor(m);
}

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	int arg_pos;
	sscanf(cmdline, "%s", name);  
	arg_pos = strlen(name);
	if (arg_pos < sz) {
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		strncpy(args, cmdline + arg_pos, sz);
	} else {
		args[0] = '\0';
	}
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

// skynet初始化完成后，开始启动运行
void 
skynet_start(struct skynet_config * config) {
	// SIGHUP会在以下3种情况下被发送给相应的进程：
	// 1、终端关闭时，该信号被发送到session首进程以及作为job提交的进程（即用 & 符号提交的进程）
	// 2、session首进程退出时，该信号被发送到该session中的前台进程组和后台进程组中的每一个进程
	// 3、若进程的退出，导致一个进程组变成了孤儿进程组，且新出现的孤儿进程组中有进程处于停止状态，则SIGHUP和SIGCONT信号会按顺序先后发送到新孤儿进程组中的每一个进程。
	// register SIGHUP for log file reopen
	// 注册SIGHUP以便log文件重新打开,注意结合守护进程的情况
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	// 守护进程方式
	if (config->daemon) {
		// 创建守护进程
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	// 初始化港口信息
	skynet_harbor_init(config->harbor);
	// 初始化全局的服务列表
	skynet_handle_init(config->harbor);
	// 初始化消息列表
	skynet_mq_init();
	// 初始化模块
	skynet_module_init(config->module_path);
	// 
	skynet_timer_init();
	// 创建服务器
	skynet_socket_init();
	skynet_profile_enable(config->profile);

	// 默认为"logger"，你可以配置为你定制的log服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	// 在全局的名称关联表中，
	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

	bootstrap(ctx, config->bootstrap);
	// 启动
	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	// 通知集群退出
	skynet_harbor_exit();
	// 释放socket
	skynet_socket_free();
	// 如果是后台程序，进行后台程序清理
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
