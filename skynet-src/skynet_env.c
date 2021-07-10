#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

// 环境变量
struct skynet_env {
	// 自旋锁
	struct spinlock lock;
	// Lua状态机
	lua_State *L;
};

static struct skynet_env *E = NULL;

const char * 
skynet_getenv(const char *key) {
	SPIN_LOCK(E)

	lua_State *L = E->L;
	// 获取key对应的值
	lua_getglobal(L, key);
	// 将值（放在栈顶）转换成string
	const char * result = lua_tostring(L, -1);
	// 恢复栈
	lua_pop(L, 1);

	SPIN_UNLOCK(E)

	return result;
}

// 设置环境变量
void 
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)
	
	lua_State *L = E->L;
	// 取得全局key对应的键
	lua_getglobal(L, key);
	// 以前没有设置过值（检查栈顶的值）
	assert(lua_isnil(L, -1));
	// 将值出栈
	lua_pop(L,1);
	// 将要设置的值入栈
	lua_pushstring(L,value);
	// 设置全局key,valu对
	lua_setglobal(L,key);

	SPIN_UNLOCK(E)
}

// 初始化环境变量的锁和状态机
// 初始化自旋锁，并创建承载环境变量的lua虚拟机
void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	// 初始化锁
	SPIN_INIT(E)
	// 创建lua虚拟机
	E->L = luaL_newstate();
}
