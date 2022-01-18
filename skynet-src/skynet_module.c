#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32
// 我们所写的C服务在编译成so库以后，会在某个时机被加载到一个modules的列表中，
// 当要创建该类服务的实例时，将从modules列表取出该服务的函数句柄，
// 调用create函数创建服务实例，并且init之后，将实例赋值给一个新的context对象后，
// 注册到skynet_context list中，一个新的服务就创建完成了。

// modules列表
struct modules {
	// module的数量
	int count;
	// module列表的自旋锁,避免多个线程同时向skynet_module写入数据，保证线程安全
	struct spinlock lock;
	// 由skynet配置表中的cpath指定，一般包含./cservice/?.so路径
	const char * path;
	// 存放服务模块的数组，最多32类
	struct skynet_module m[MAX_MODULE_TYPE];
};

static struct modules * M = NULL;

static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	// 计算路径和名字连接在一起需要的空间总和
	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		// 如果是;就跳过
		while (*path == ';') path++;
		if (*path == '\0') break;
		// 找到下一个;
		l = strchr(path, ';');
		// 如果没找到，就是将后面的都给它
		if (l == NULL) l = path + strlen(path);

		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		// 将名字连接在后面
		memcpy(tmp+i,name,name_size);
		// 最后一个是?,?是名字的占位符而已，需要将path中?后面的复制过来
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		// 打开对应的动态库
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

// 通过名字找到对应的模块
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

// 得到模块中的API
static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	// .后面的才是api的名字
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	return dlsym(mod->module, ptr);
}

// 得到_create,_init,_release,_signal的地址
static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");
	mod->init = get_api(mod, "_init");
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

// 通过模块名查询模块
struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	SPIN_LOCK(M)

	// 加锁以后第二次校验
	result = _query(name); // double check

	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
		// 打开对应的动态库
		void * dl = _try_open(M,name);
		if (dl) {
			// 设置名字和对应的模块
			M->m[index].name = name;
			M->m[index].module = dl;
			// 得到_create,_init,_release,_signal的地址
			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	SPIN_UNLOCK(M)

	return result;
}
// 插入一个模块
void 
skynet_module_insert(struct skynet_module *mod) {
	SPIN_LOCK(M)

	struct skynet_module * m = _query(mod->name);
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;
	M->m[index] = *mod;
	++M->count;

	SPIN_UNLOCK(M)
}

// 创建模块，调用模块的创建函数
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

// 初始化模块
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

// 释放模块
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

// 调用该signal即是调用xxx_signal
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

// 创建module列表
void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	// 初始module的数量为0
	m->count = 0;
	// 复制path
	m->path = skynet_strdup(path);
	// 初始自旋锁
	SPIN_INIT(m)

	M = m;
}
