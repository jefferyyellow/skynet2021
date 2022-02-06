#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000
// 这个结构用于记录，服务对应的别名，当应用层为某个服务命名时，会写到这里来
struct handle_name {
	// 服务别名
	char * name;
	// 服务id
	uint32_t handle;
};

// handle_storage用于管理创建出来的skynet_context实例
struct handle_storage {
	// 读写锁
	struct rwlock lock;
	// 港口ID
	uint32_t harbor;
	// 创建下一个服务时，该服务的slot idx，一般会先判断该slot是否被占用
	uint32_t handle_index;
	// slot的大小，一定是2^n，初始值是4
	int slot_size;
	// 服务列表（skynet_context list）
	struct skynet_context ** slot;
	// 别名列表大小，大小为2^n
	int name_cap;
	// 别名数量
	int name_count;
	// 别名列表
	struct handle_name *name;
};
// 全局的服务列表中
static struct handle_storage *H = NULL;

uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;
		uint32_t handle = s->handle_index;
		for (i=0;i<s->slot_size;i++,handle++) {
			if (handle > HANDLE_MASK) {
				// 0 is reserved
				handle = 1;
			}
			int hash = handle & (s->slot_size-1);
			if (s->slot[hash] == NULL) {
				s->slot[hash] = ctx;
				s->handle_index = handle + 1;

				rwlock_wunlock(&s->lock);

				handle |= s->harbor;
				return handle;
			}
		}
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
		for (i=0;i<s->slot_size;i++) {
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}


// 实例(service)退出
int
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);

	// 通过句柄找到实例
	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;
		// 遍历所有的别名,把匹配的释放掉
		// 注意:把释放掉的后面的还往前挪,具体就是通过i和j的操作实现
		for (i=0; i<n; ++i) {
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}
		s->name_count = j;
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		// 减少实例(service)的引用计数
		skynet_context_release(ctx);
	}

	return ret;
}

// 所有的实例都退出
void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		// 遍历所有的slot
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
				handle = skynet_context_handle(ctx);
			rwlock_runlock(&s->lock);
			if (handle != 0) {
				if (skynet_handle_retire(handle)) {
					++n;
				}
			}
		}
		if (n==0)
			return;
	}
}

// 通过handle得到实例
struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	// 锁读锁
	rwlock_rlock(&s->lock);
	// 通过hash计算在slot的位置
	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	// 进行再一次的校验
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		skynet_context_grab(result);
	}

	// 解锁读锁
	rwlock_runlock(&s->lock);

	return result;
}

// 通过名字搜索句柄
uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	// 下面是一个2分法搜索过程
	int begin = 0;
	int end = s->name_count - 1;
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		// 通过比较别名来匹配
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}
// 在before前面插入名字
static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	// 如果名字的数量已经大于等于容量
	if (s->name_count >= s->name_cap) {
		// 容量增加2
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		// 拷贝before前面的部分
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		// 拷贝before后面的部分,包括before
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		// 释放原来的
		skynet_free(s->name);
		// 使用新的
		s->name = n;
	} else {
		int i;
		// 将后面的往后移
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	// 将名字插入进去
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

// 插入handle的名字name
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	// 二分法查找名字,如果有重复的,直接返回
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		// 对比名字
		int c = strcmp(n->name, name);
		// 找到了
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	// 复制一个名字并返回
	char * result = skynet_strdup(name);
	// 插入到begin前面
	_insert_name_before(s, result, handle, begin);

	return result;
}

const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);
	// 插入一个实例句柄的名字
	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

// 初始化全局的服务列表
void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	// 分配内存，默认值分配4个slot
	struct handle_storage * s = skynet_malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));
	// 初始化读写锁
	rwlock_init(&s->lock);
	// reserve 0 for system
	// 记录港口索引
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	// 初始化创建下一个服务时slot index为1
	s->handle_index = 1;
	// 别名列表大小默认为1
	s->name_cap = 2;
	// 别名列表数量为0
	s->name_count = 0;
	// 别名列表分配内存
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

