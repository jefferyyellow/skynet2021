#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// reserve high 8 bits for remote id
// 保留高8位给远程id
#define HANDLE_MASK 0xffffff
// 低24位为本地的句柄，高8位给集群用
#define HANDLE_REMOTE_SHIFT 24

struct skynet_context;
// 服务注册
uint32_t skynet_handle_register(struct skynet_context *);
// 实例(service)退出
int skynet_handle_retire(uint32_t handle);
// 通过handle得到实例
struct skynet_context * skynet_handle_grab(uint32_t handle);
// 所有的实例都退出
void skynet_handle_retireall();
// 通过名字搜索实例句柄
uint32_t skynet_handle_findname(const char * name);
// 命名一个句柄
const char * skynet_handle_namehandle(uint32_t handle, const char *name);
// 初始化全局的服务列表
void skynet_handle_init(int harbor);

#endif
