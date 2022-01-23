#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// reserve high 8 bits for remote id
// 保留高8位给远程id
#define HANDLE_MASK 0xffffff
// 低24位为本地的句柄，高8位给集群用
#define HANDLE_REMOTE_SHIFT 24

struct skynet_context;

uint32_t skynet_handle_register(struct skynet_context *);
int skynet_handle_retire(uint32_t handle);
struct skynet_context * skynet_handle_grab(uint32_t handle);
void skynet_handle_retireall();

uint32_t skynet_handle_findname(const char * name);
const char * skynet_handle_namehandle(uint32_t handle, const char *name);
// 初始化全局的服务列表
void skynet_handle_init(int harbor);

#endif
