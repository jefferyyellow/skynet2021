#ifndef skynet_daemon_h
#define skynet_daemon_h
// 守护进程初始化
int daemon_init(const char *pidfile);
// 守护进程退出
int daemon_exit(const char *pidfile);

#endif
