#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

// 获取int型的环境变量：
// 如果对应的键存在，就返回对应的值，如果对应的键不存在，就设置key值为opt，
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

// 获取boolean型的环境变量：
// 如果对应的键存在，就返回对应的值，如果对应的键不存在，就设置key值为opt，
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

// 获取string型的环境变量：
// 如果对应的键存在，就返回对应的值，如果对应的键不存在，就设置key值为opt，
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

static void
_init_env(lua_State *L) {
	// 将nil入栈，作为初始key
	lua_pushnil(L);  /* first key */
	// 变量该函数进来时，栈顶上的那个表，由于push了一个nil，现在表的索引变成-2
	while (lua_next(L, -2) != 0) {
		// 检查键的类型一定是string
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		// 得到键
		const char * key = lua_tostring(L,-2);
		// 值的类型是bool型
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			// 设置环境变量
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			// 或者就是string型
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			// 设置环境变量
			skynet_setenv(key,value);
		}
		// 将值出栈，留下键，继续
		lua_pop(L,1);
	}
	// 将最后一个键也出栈，恢复栈平衡
	lua_pop(L,1);
}

/*
* SIGPIPE信号产生的原因：
* 简单来说，就是客户端程序向服务器端程序发送了消息，然后关闭客户端，服务器端返回消息的时候就会收到内核给的SIGPIPE信号。
* TCP的全双工信道其实是两条单工信道，client端调用close的时候，虽然本意是关闭两条信道，但是其实只能关闭它发送的那一条单工信道，
* 还是可以接受数据，server端还是可以发送数据，并不知道client端已经完全关闭了。
* 以下为引用：
* ”’对一个已经收到FIN包的socket调用read方法, 如果接收缓冲已空, 则返回0, 这就是常说的表示连接关闭.但第一次对其调用write方法时, 
* 如果发送缓冲没问题, 会返回正确写入(发送).但发送的报文会导致对端发送RST报文, 因为对端的socket已经调用了close, 完全关闭, 既不发送, 
* 也不接收数据.所以, 第二次调用write方法(假设在收到RST之后), 会生成SIGPIPE信号, 导致进程退出.”’
*/
// 忽略SIG_IGN信号
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

// 加载配置的lua脚本
static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	// 如果传入参数，那么第一个参数就是配置文件
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}
	// 全局节点的初始化
	skynet_globalinit();
	// 初始化环境变量需要的锁和lua虚拟机
	skynet_env_init();
	// 设置信号相关
	sigign();

	struct skynet_config config;

#ifdef LUA_CACHELIB
	// init the lock of code cache
	luaL_initcodecache();
#endif
	// 创建新的lua虚拟机
	struct lua_State *L = luaL_newstate();
	// 链接lua库
	luaL_openlibs(L);	// link lua lib

	// 加载解析配置文件的脚步
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	// 配置文件名压栈
	lua_pushstring(L, config_file);
	// 调用脚本解析配置文件
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	// 初始化lua环境变量
	_init_env(L);

	// 得到线程的数量，默认为8
	config.thread =  optint("thread",8);
	// c模块的路径
	config.module_path = optstring("cpath","./cservice/?.so");
	// 网络节点的编号
	config.harbor = optint("harbor", 1);
	// skynet 启动的第一个服务以及其启动参数。默认配置为 snlua bootstrap ，
	// 即启动一个名为 bootstrap 的 lua 服务。通常指的是 service/bootstrap.lua 这段代码。
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	// 配置daemon = "./skynet.pid" 可以以后台模式启动skynet 。注意，同时请配置 logger 项输出 log 
	config.daemon = optstring("daemon", NULL);
	// 它决定了 skynet 内建的 skynet_error 这个 C API 将信息输出到什么文件中。
	// 如果 logger 配置为 nil ，将输出到标准输出。你可以配置一个文件名来将信息记录在特定文件中。
	config.logger = optstring("logger", NULL);
	// 默认为 "logger" ，你可以配置为你定制的 log 服务（比如加上时间戳等更多信息）。可以参考 service_logger.c 来实现它。
	// 注：如果你希望用 lua 来编写这个服务，可以在这里填写 snlua ，然后在 logger 配置具体的 lua 服务的名字。
	// 在 examples 目录下，有 config.userlog 这个范例可供参考。
	config.logservice = optstring("logservice", "logger");
	// 默认开起性能分析，可以用来统计每个服务使用了多少 cpu 时间。在 DebugConsole 中可以查看。
	// 会对性能造成微弱的影响，设置为 false 可以关闭这个统计。
	config.profile = optboolean("profile", 1);
	// 关闭前面创建的分析配置文件的虚拟机
	lua_close(L);
	// skynet初始化完成后，开始启动运行
	skynet_start(&config);
	// 全局的退出时清理
	skynet_globalexit();

	return 0;
}
