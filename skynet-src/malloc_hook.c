#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <lua.h>
#include <stdio.h>

#include "malloc_hook.h"
#include "skynet.h"
#include "atomic.h"

// turn on MEMORY_CHECK can do more memory check, such as double free
// #define MEMORY_CHECK

#define MEMORY_ALLOCTAG 0x20140605
#define MEMORY_FREETAG 0x0badf00d

static ATOM_SIZET _used_memory = 0;
static ATOM_SIZET _memory_block = 0;

struct mem_data {
	uint32_t handle;
	ssize_t allocated;
};

struct mem_cookie {
	uint32_t handle;
#ifdef MEMORY_CHECK
	uint32_t dogtag;
#endif
};

#define SLOT_SIZE 0x10000
#define PREFIX_SIZE sizeof(struct mem_cookie)

static struct mem_data mem_stats[SLOT_SIZE];


#ifndef NOUSE_JEMALLOC

#include "jemalloc.h"

// for skynet_lalloc use
#define raw_realloc je_realloc
#define raw_free je_free

// 通过句柄得到分配的状态信息
static ssize_t*
get_allocated_field(uint32_t handle) {
	int h = (int)(handle & (SLOT_SIZE - 1));
	// 通过取余的方式得到下标
	struct mem_data *data = &mem_stats[h];
	uint32_t old_handle = data->handle;
	ssize_t old_alloc = data->allocated;
	// 如果原来没有信息的话
	if(old_handle == 0 || old_alloc <= 0) {
		// data->allocated may less than zero, because it may not count at start.
		// 如果不一致就设置
		if(!ATOM_CAS(&data->handle, old_handle, handle)) {
			return 0;
		}
		// 并且需要将原来的清零
		if (old_alloc < 0) {
			ATOM_CAS(&data->allocated, old_alloc, 0);
		}
	}
	// 如果句柄不一致，表示原来有东西了，就返回0
	if(data->handle != handle) {
		return 0;
	}
	// 返回状态信息的地址
	return &data->allocated;
}

// 更新分配的状态信息
inline static void
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	ATOM_FADD(&_used_memory, __n);
	ATOM_FINC(&_memory_block);
	// 得到已经分配的地址
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		// 增加分配__n的大小
		ATOM_FADD(allocated, __n);
	}
}

// 更新释放的状态信息
inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	ATOM_FSUB(&_used_memory, __n);
	ATOM_FDEC(&_memory_block);
	// 得到已经分配的地址
	ssize_t* allocated = get_allocated_field(handle);
	if(allocated) {
		// 减少分配__n的大小
		ATOM_FSUB(allocated, __n);
	}
}

inline static void*
fill_prefix(char* ptr) {
	uint32_t handle = skynet_current_handle();
	// 得到内存块的大小
	size_t size = je_malloc_usable_size(ptr);
	// 得到内存块的最后一部分是mem_cookie
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie));
	memcpy(&p->handle, &handle, sizeof(handle));
#ifdef MEMORY_CHECK
	uint32_t dogtag = MEMORY_ALLOCTAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	// 更新句柄分配的大小
	update_xmalloc_stat_alloc(handle, size);
	return ptr;
}

inline static void*
clean_prefix(char* ptr) {
	// 得到内存块的大小
	size_t size = je_malloc_usable_size(ptr);
	// 得到内存块的最后一部分是mem_cookie
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie));
	uint32_t handle;
	memcpy(&handle, &p->handle, sizeof(handle));
#ifdef MEMORY_CHECK
	uint32_t dogtag;
	memcpy(&dogtag, &p->dogtag, sizeof(dogtag));
	if (dogtag == MEMORY_FREETAG) {
		fprintf(stderr, "xmalloc: double free in :%08x\n", handle);
	}
	assert(dogtag == MEMORY_ALLOCTAG);	// memory out of bounds
	dogtag = MEMORY_FREETAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	// 更新句柄释放的大小
	update_xmalloc_stat_free(handle, size);
	return ptr;
}

// 内存分配光了，打印错误，并且终止程序
static void malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n",
		size);
	fflush(stderr);
	abort();
}

// 首先我们来看看 jemalloc 自己提供的统计信息，我们可以直接使用 je_malloc_stats_print(NULL, NULL, NULL) 来将 memory 的统计输出到 stderr 上面，
// 但这个函数输出的东西比较多，并不利于实时的查看。多数时候，我们都是使用 je_mallctl 函数，
void
memory_info_dump(const char* opts) {
	je_malloc_stats_print(0,0, opts);
}

bool
mallctl_bool(const char* name, bool* newval) {
	bool v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(bool));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	return v;
}

int
mallctl_cmd(const char* name) {
	return je_mallctl(name, NULL, NULL, NULL, 0);
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	size_t v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(size_t));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	// skynet_error(NULL, "name: %s, value: %zd\n", name, v);
	return v;
}

int
mallctl_opt(const char* name, int* newval) {
	int v = 0;
	size_t len = sizeof(v);
	if(newval) {
		int ret = je_mallctl(name, &v, &len, newval, sizeof(int));
		if(ret == 0) {
			skynet_error(NULL, "set new value(%d) for (%s) succeed\n", *newval, name);
		} else {
			skynet_error(NULL, "set new value(%d) for (%s) failed: error -> %d\n", *newval, name, ret);
		}
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}

	return v;
}

// hook : malloc, realloc, free, calloc
// 钩子：分配，重分配，释放，分配

// 分配
void *
skynet_malloc(size_t size) {
	void* ptr = je_malloc(size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	// 分配内存成功后处理
	return fill_prefix(ptr);
}

// 重分配
void *
skynet_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return skynet_malloc(size);
	// 释放内存前调用处理
	void* rawptr = clean_prefix(ptr);
	void *newptr = je_realloc(rawptr, size+PREFIX_SIZE);
	if(!newptr) malloc_oom(size);
	// 分配内存成功后处理
	return fill_prefix(newptr);
}

// 释放
void
skynet_free(void *ptr) {
	if (ptr == NULL) return;
	// 释放内存前调用处理
	void* rawptr = clean_prefix(ptr);
	je_free(rawptr);
}

// 分配
void *
skynet_calloc(size_t nmemb,size_t size) {
	void* ptr = je_calloc(nmemb + ((PREFIX_SIZE+size-1)/size), size );
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

// 内存对齐
void *
skynet_memalign(size_t alignment, size_t size) {
	void* ptr = je_memalign(alignment, size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

// 内存对齐分配
void *
skynet_aligned_alloc(size_t alignment, size_t size) {
	void* ptr = je_aligned_alloc(alignment, size + (size_t)((PREFIX_SIZE + alignment -1) & ~(alignment-1)));
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

int
skynet_posix_memalign(void **memptr, size_t alignment, size_t size) {
	int err = je_posix_memalign(memptr, alignment, size + PREFIX_SIZE);
	if (err) malloc_oom(size);
	fill_prefix(*memptr);
	return err;
}

#else

// for skynet_lalloc use
#define raw_realloc realloc
#define raw_free free

void
memory_info_dump(const char* opts) {
	skynet_error(NULL, "No jemalloc");
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_int64 %s.", name);
	return 0;
}

int
mallctl_opt(const char* name, int* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_opt %s.", name);
	return 0;
}

bool
mallctl_bool(const char* name, bool* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_bool %s.", name);
	return 0;
}

int
mallctl_cmd(const char* name) {
	skynet_error(NULL, "No jemalloc : mallctl_cmd %s.", name);
	return 0;
}

#endif

size_t
malloc_used_memory(void) {
	return ATOM_LOAD(&_used_memory);
}

size_t
malloc_memory_block(void) {
	return ATOM_LOAD(&_memory_block);
}

// 转存所有服务的内存信息
void
dump_c_mem() {
	int i;
	size_t total = 0;
	skynet_error(NULL, "dump all service mem:");
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			total += data->allocated;
			skynet_error(NULL, ":%08x -> %zdkb %db", data->handle, data->allocated >> 10, (int)(data->allocated % 1024));
		}
	}
	skynet_error(NULL, "+total: %zdkb",total >> 10);
}

// 复制字符串
char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	char * ret = skynet_malloc(sz+1);
	memcpy(ret, str, sz+1);
	return ret;
}

// 将ptr从原来的尺寸调整到新的尺寸
void *
skynet_lalloc(void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		raw_free(ptr);
		return NULL;
	} else {
		return raw_realloc(ptr, nsize);
	}
}

// lua接口：得到当前服务的内存数量
int
dump_mem_lua(lua_State *L) {
	int i;
	lua_newtable(L);
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			lua_pushinteger(L, data->allocated);
			lua_rawseti(L, -2, (lua_Integer)data->handle);
		}
	}
	return 1;
}

// 得到当前服务分配的内存信息
size_t
malloc_current_memory(void) {
	uint32_t handle = skynet_current_handle();
	int i;
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle == (uint32_t)handle && data->allocated != 0) {
			return (size_t) data->allocated;
		}
	}
	return 0;
}

// 打印调试信息，当前的服务分配的内存的数量
void
skynet_debug_memory(const char *info) {
	// for debug use
	uint32_t handle = skynet_current_handle();
	size_t mem = malloc_current_memory();
	fprintf(stderr, "[:%08x] %s %p\n", handle, info, (void *)mem);
}
