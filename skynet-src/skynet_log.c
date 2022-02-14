#include "skynet_log.h"
#include "skynet_timer.h"
#include "skynet.h"
#include "skynet_socket.h"
#include <string.h>
#include <time.h>

// 打开日志
FILE * 
skynet_log_open(struct skynet_context * ctx, uint32_t handle) {
	// 找到环境变量中日志的路径
	const char * logpath = skynet_getenv("logpath");
	if (logpath == NULL)
		return NULL;
	// 日志路径格式化
	size_t sz = strlen(logpath);
	char tmp[sz + 16];
	sprintf(tmp, "%s/%08x.log", logpath, handle);
	// 打开日志文件
	FILE *f = fopen(tmp, "ab");
	if (f) {
		uint32_t starttime = skynet_starttime();
		uint64_t currenttime = skynet_now();
		time_t ti = starttime + currenttime/100;
		// 写入一个“打开日志文件”的日志
		skynet_error(ctx, "Open log file %s", tmp);
		// 在日志文件中写入打开时间
		fprintf(f, "open time: %u %s", (uint32_t)currenttime, ctime(&ti));
		fflush(f);
	} else {
		skynet_error(ctx, "Open log file %s fail", tmp);
	}
	return f;
}

// 关闭日志文件
// 记录关闭日志文件的记录
// 将关闭时间写入日志文件中去
void
skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle) {
	skynet_error(ctx, "Close log file :%08x", handle);
	fprintf(f, "close time: %u\n", (uint32_t)skynet_now());
	fclose(f);
}

// 将buffer中sz长度的缓存以16进制的形式写入到日志文件中去
static void
log_blob(FILE *f, void * buffer, size_t sz) {
	size_t i;
	uint8_t * buf = buffer;
	for (i=0;i!=sz;i++) {
		fprintf(f, "%02x", buf[i]);
	}
}

// 写入
static void
log_socket(FILE * f, struct skynet_socket_message * message, size_t sz) {
	fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

	// 如果BUFF为空
	if (message->buffer == NULL) {
		const char *buffer = (const char *)(message + 1);
		sz -= sizeof(*message);
		// 从buf所指内存区域的前count个字节查找字符ch
		const char * eol = memchr(buffer, '\0', sz);
		if (eol) {
			sz = eol - buffer;
		}
		fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
	} else {
		sz = message->ud;
		log_blob(f, message->buffer, sz);
	}
	fprintf(f, "\n");
	fflush(f);
}

// 将日志写入文件
void 
skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz) {
	if (type == PTYPE_SOCKET) {
		log_socket(f, buffer, sz);
	} else {
		uint32_t ti = (uint32_t)skynet_now();
		fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
		log_blob(f, buffer, sz);
		fprintf(f,"\n");
		fflush(f);
	}
}
