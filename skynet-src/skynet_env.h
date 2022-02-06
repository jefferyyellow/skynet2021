#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H
// 得到环境变量
const char * skynet_getenv(const char *key);
// 设置环境变量
void skynet_setenv(const char *key, const char *value);
// 环境变量初始化
void skynet_env_init();

#endif
