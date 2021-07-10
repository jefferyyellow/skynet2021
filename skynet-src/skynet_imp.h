#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H
// skynet配置
struct skynet_config {
	// 线程数量
	int thread;
	// 一个skynet网络最多支持255个节点。每个节点有必须有一个唯一的编号。
	int harbor;
	// 默认为 true, 可以用来统计每个服务使用了多少 cpu 时间。
	// 在 DebugConsole 中可以查看。会对性能造成微弱的影响，设置为 false 可以关闭这个统计。
	int profile;
	// daemon 配置 daemon = "./skynet.pid" 可以以后台模式启动 skynet 。注意，同时请配置 logger 项输出 log 。
	const char * daemon;
	const char * module_path;
	// skynet 启动的第一个服务以及其启动参数。默认配置为 snlua bootstrap ，即启动一个名为 bootstrap 的 lua 服务。
	// 通常指的是 service/bootstrap.lua 这段代码。
	const char * bootstrap;
	// 它决定了 skynet 内建的 skynet_error 这个 C API 将信息输出到什么文件中。如果 logger 配置为 nil ，将输出到标准输出。
	// 你可以配置一个文件名来将信息记录在特定文件中。
	const char * logger;
	// 默认为 "logger" ，你可以配置为你定制的 log 服务（比如加上时间戳等更多信息）。可以参考 service_logger.c 来实现它。
	// 注：如果你希望用 lua 来编写这个服务，可以在这里填写 snlua ，然后在 logger 配置具体的 lua 服务的名字。
	// 在examples目录下，有config.userlog这个范例可供参考。
	const char * logservice;
};
// 线程类型
// 工作线程
#define THREAD_WORKER 0
// 主线程
#define THREAD_MAIN 1
// socket线程
#define THREAD_SOCKET 2
// 计时器（时间）线程
#define THREAD_TIMER 3
// 监控线程
#define THREAD_MONITOR 4
// skynet初始化完成后，开始启动运行
void skynet_start(struct skynet_config * config);

#endif
