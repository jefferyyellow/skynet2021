#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>

#include "skynet_daemon.h"

static int
check_pid(const char *pidfile) {
	int pid = 0;
	// 打开文件
	FILE *f = fopen(pidfile,"r");
	if (f == NULL)
		return 0;
	// 如果存在就读取进程id
	int n = fscanf(f,"%d", &pid);
	// 关闭文件
	fclose(f);

	if (n !=1 || pid == 0 || pid == getpid()) {
		return 0;
	}
	// kill -0 pid 不发送任何信号，但是系统会进行错误检查。
	// 所以经常用来检查一个进程是否存在，存在返回0；不存在返回1;
	// 错误码ESRCH：无此进程
	if (kill(pid, 0) && errno == ESRCH)
		return 0;

	return pid;
}

static int
write_pid(const char *pidfile) {
	FILE *f;
	int pid = 0;
	// 打开文件
	int fd = open(pidfile, O_RDWR|O_CREAT, 0644);
	if (fd == -1) {
		fprintf(stderr, "Can't create pidfile [%s].\n", pidfile);
		return 0;
	}
	// dopen取一个现存的文件描述符，并使一个标准的I/O流与该描述符相结合。此函数常用于由创建管道和网络通信通道函数获得的描述符。
	// 因为这些特殊类型的文件不能用标准I/O fopen函数打开，
	// 首先必须先调用设备专用函数以获得一个文件描述符，然后用fdopen使一个标准I/O流与该描述符相结合。
	f = fdopen(fd, "w+");
	if (f == NULL) {
		fprintf(stderr, "Can't open pidfile [%s].\n", pidfile);
		return 0;
	}
	// 锁定文件
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		int n = fscanf(f, "%d", &pid);
		fclose(f);
		if (n != 1) {
			fprintf(stderr, "Can't lock and read pidfile.\n");
		} else {
			fprintf(stderr, "Can't lock pidfile, lock is held by pid %d.\n", pid);
		}
		return 0;
	}
	// 写入文件
	pid = getpid();
	if (!fprintf(f,"%d\n", pid)) {
		fprintf(stderr, "Can't write pid.\n");
		close(fd);
		return 0;
	}
	// 冲刷
	fflush(f);

	return pid;
}

// 重定向标准输入、标准输出和标准错误到/dev/null
static int
redirect_fds() {
	int nfd = open("/dev/null", O_RDWR);
	if (nfd == -1) {
		perror("Unable to open /dev/null: ");
		return -1;
	}
	if (dup2(nfd, 0) < 0) {
		perror("Unable to dup2 stdin(0): ");
		return -1;
	}
	if (dup2(nfd, 1) < 0) {
		perror("Unable to dup2 stdout(1): ");
		return -1;
	}
	if (dup2(nfd, 2) < 0) {
		perror("Unable to dup2 stderr(2): ");
		return -1;
	}

	close(nfd);

	return 0;
}

// 将标准输入、标准输出以及标准错误重定向/dev/null
int
daemon_init(const char *pidfile) {
	// 检查是否已经运行中了
	int pid = check_pid(pidfile);
	// 如果找到pid
	if (pid) {
		fprintf(stderr, "Skynet is already running, pid = %d.\n", pid);
		return 1;
	}

#ifdef __APPLE__
	fprintf(stderr, "'daemon' is deprecated: first deprecated in OS X 10.5 , use launchd instead.\n");
#else
	// 守护进程模式，不改变当前目录和标准输入、标准输出以及标准错误
	if (daemon(1,1)) {
		fprintf(stderr, "Can't daemonize.\n");
		return 1;
	}
#endif
	// 将进程的id写入pidfile
	pid = write_pid(pidfile);
	if (pid == 0) {
		return 1;
	}
	// 将标准输入、标准输出以及标准错误重定向/dev/null
	if (redirect_fds()) {
		return 1;
	}

	return 0;
}

int
daemon_exit(const char *pidfile) {
	return unlink(pidfile);
}
