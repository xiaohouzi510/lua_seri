#include <lua.hpp>
#include "lua_seri.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#define BLOCK_SIZE 128
#define MAX_DEPTH 32
#define SURPLUS 32
#define COMBINE_BYTE(t,v) ((t)|((v)<<3))

enum edata_type
{
	edata_nil 	    = 0,
	edata_bool 	    = 1,
	edata_number    = 2,
	edata_userdata  = 3,
	edata_short_str = 4,
	edata_long_str  = 5,
	edata_table     = 6,
};

enum enumber_type
{
	enumber_zero 	   = 0,
	enumber_long_long  = 1,
	enumber_dword 	   = 2,
	enumber_word       = 3,
	enumber_byte 	   = 4,
	enumber_real 	   = 5,
};

//每个最小单位
struct block
{
	block() : m_next(NULL)
	{
		memset(m_buffer,0,sizeof(m_buffer));
	}
	block *m_next;
	char   m_buffer[BLOCK_SIZE];
};

struct write_block
{
	write_block() : m_head(NULL),m_current(NULL),m_len(0),m_ptr(0)
	{
		m_head = m_current = new block();
	}
	block       *m_head;
	block       *m_current;
	unsigned int m_len;
	unsigned int m_ptr;
};

struct read_block
{
	read_block() : m_buffer(0),m_len(0),m_ptr(0){}
	char *m_buffer;
	int   m_len;
	int   m_ptr; //可写的起止位置
};

static void pack_one(lua_State *L,write_block *wr,int index,int depth);
static void push_value(lua_State *L,read_block *rb,edata_type type,unsigned char surplus);

static void invalid_stream_line(lua_State *L, struct read_block *rb, int line)
{
	luaL_error(L, "Invalid serialize stream %d (line:%d)", rb->m_len, line);
}

#define invalid_stream(L,rb) invalid_stream_line(L,rb,__LINE__)

bool lua_isinteger(lua_State *L,int index)
{
	double number  = lua_tonumber(L,index);
	double integer = lua_tointeger(L,index);
	return number == integer;
}

static void wb_free(write_block *wr)
{
	block *b = wr->m_head;
	while(b != NULL)
	{
		block *cur = b;
		b = b->m_next;
		delete cur;
	}

	wr->m_head    = NULL;
	wr->m_current = NULL;
	wr->m_len 	  = 0;
	wr->m_ptr 	  = 0;
}

static void* rb_read(read_block *rb,unsigned int len)
{
	if(rb->m_len < len)
	{
		return NULL;
	}
	unsigned int ptr = rb->m_ptr;
	rb->m_ptr += len;
	rb->m_len -= len;
	return (void *)(rb->m_buffer + ptr);
}

static void write_push(write_block *wr,const void *data,unsigned int len)
{
	const char *buff = (const char *)data;
	while(true)
	{
		if(wr->m_ptr == BLOCK_SIZE)
		{
			wr->m_current->m_next = new block();
			wr->m_current = wr->m_current->m_next;
			wr->m_ptr = 0;
		}
		if(BLOCK_SIZE - wr->m_ptr >= len)
		{
			memcpy(wr->m_current->m_buffer + wr->m_ptr,buff,len);
			wr->m_ptr = wr->m_ptr + len;
			wr->m_len = wr->m_len + len;
			break;
		}
		else
		{
			unsigned int can_cpy = BLOCK_SIZE - wr->m_ptr;
			memcpy(wr->m_current->m_buffer + wr->m_ptr,buff,can_cpy);
			wr->m_ptr = wr->m_ptr + can_cpy;
			wr->m_len = wr->m_len + can_cpy;
			len 	  = len - can_cpy;
			buff 	 += can_cpy;
		}
	}
}

static void write_nil(write_block *wr)
{
	unsigned char type = edata_nil;
	write_push(wr,&type,sizeof(type));
}

static void write_integer(write_block *wr,lua_Integer v)
{
	//位移操作要用32位或64位,移操作会会被强转
	int type = (int)edata_number;
	unsigned char result_type = 0;
	if(v == 0)
	{
		result_type = COMBINE_BYTE(type,enumber_zero);
		write_push(wr,&result_type,sizeof(result_type));
	}
	else if(v != (int)v)
	{
		result_type = COMBINE_BYTE(type,enumber_long_long);
		int64_t v64 = (int64_t)v;
		write_push(wr,&result_type,sizeof(result_type));
		write_push(wr,&v64,sizeof(v64));
	}
	else if(v < 0)
	{
		result_type = COMBINE_BYTE(type,enumber_dword);
		int v32 = (int)v;
		write_push(wr,&result_type,sizeof(result_type));
		write_push(wr,&v32,sizeof(v32));
	}
	else if(v <= 255)
	{
		result_type = COMBINE_BYTE(type,enumber_byte);
		unsigned char v8 = (unsigned char)v;
		write_push(wr,&result_type,sizeof(result_type));
		write_push(wr,&v8,sizeof(v8));
	}
	else if(v <= 65535)
	{
		result_type = COMBINE_BYTE(type,enumber_word);
		unsigned short v16 = (unsigned short)v;
		write_push(wr,&result_type,sizeof(result_type));
		write_push(wr,&v16,sizeof(v16));
	}
	else
	{
		result_type = COMBINE_BYTE(type,enumber_dword);
		unsigned int v32 = (unsigned int)v;
		write_push(wr,&result_type,sizeof(result_type));
		write_push(wr,&v32,sizeof(v32));
	}
}

static void write_real(write_block *wr,double v)
{
	unsigned char type = COMBINE_BYTE(edata_number,enumber_real);
	write_push(wr,&type,sizeof(type));
	write_push(wr,&v,sizeof(v));
}

static void write_bool(write_block *wr,int boolean)
{
	unsigned char type = COMBINE_BYTE(edata_bool,boolean?1:0);
	write_push(wr,&type,sizeof(type));
}

static void write_string(write_block *wr,const char *str,unsigned int len)
{
	if(len < SURPLUS)
	{
		unsigned char type = COMBINE_BYTE(edata_short_str,len);
		write_push(wr,&type,sizeof(type));
		//防止空串
		if(len > 0)
		{
			write_push(wr,str,len);
		}
	}
	else
	{
		unsigned char type = edata_long_str;
		if(len <= 255)
		{
			type = COMBINE_BYTE(type,enumber_byte);
			write_push(wr,&type,sizeof(type));
			unsigned char v = (unsigned char)len;
			write_push(wr,&v,sizeof(v));
		}
		else if(len <= 65535)
		{
			type = COMBINE_BYTE(type,enumber_word);
			write_push(wr,&type,sizeof(type));
			unsigned short v = (unsigned short)len;
			write_push(wr,&v,sizeof(v));
		}
		else
		{
			type = COMBINE_BYTE(type,enumber_dword);
			write_push(wr,&type,sizeof(type));
			unsigned int v = (unsigned int)len;
			write_push(wr,&v,sizeof(v));
		}

		write_push(wr,str,len);
	}
}

static void write_point(write_block *wr,void *data)
{
	unsigned char type = edata_userdata;
	write_push(wr,&type,sizeof(type));
	write_push(wr,data,sizeof(data));
}

static unsigned int write_table_array(lua_State *L,write_block *wr,int index,int depth)
{
	int array_size = lua_objlen(L,index);
	if(array_size > SURPLUS)
	{
		unsigned char type = COMBINE_BYTE(edata_table,SURPLUS-1);;
		write_push(wr,&type,sizeof(type));
		write_integer(wr,(lua_Integer)array_size);
	}
	else
	{
		unsigned char type = COMBINE_BYTE(edata_table,array_size);
		write_push(wr,&type,sizeof(type));
	}

	for(int i = 1;i <= array_size;++i)
	{
		lua_rawgeti(L,index,i);
		pack_one(L,wr,-1,depth);
		lua_pop(L,1);
	}

	return array_size;
}

static void write_table_hash(lua_State *L,write_block *wr,int index,int depth,int array_size)
{
	lua_pushnil(L);
	while(lua_next(L,index) != 0)
	{
		if(lua_isinteger(L,-2))
		{
			lua_Integer key = lua_tointeger(L,-2);
			if(key > 0 && key <= array_size)
			{
				lua_pop(L,1);
				continue;
			}
		}
		pack_one(L,wr,-2,depth);
		pack_one(L,wr,-1,depth);
		lua_pop(L,1);
	}

	write_nil(wr);
}

static void write_table(lua_State *L,write_block *wr,int index,int depth)
{
	int array_size = write_table_array(L,wr,index,depth);
	write_table_hash(L,wr,index,depth,array_size);
}

static void pack_one(lua_State *L,write_block *wr,int index,int depth)
{
	if(depth > MAX_DEPTH)
	{
		wb_free(wr);
		luaL_error(L,"serialize can not pack too depth table");
	}

	int type = lua_type(L,index);
	switch(type)
	{
	case LUA_TNIL:
		{
			write_nil(wr);
			break;
		}
	case LUA_TNUMBER:
		{
			if(lua_isinteger(L,index))
			{
				write_integer(wr,lua_tointeger(L,index));
			}
			else
			{
				write_real(wr,lua_tonumber(L,index));
			}
			break;
		}
	case LUA_TBOOLEAN:
		{
			write_bool(wr,lua_toboolean(L,index));
			break;
		}
	case LUA_TSTRING:
		{
			size_t len      = 0;
			const char *str = lua_tolstring(L,index,&len);
			write_string(wr,str,len);
			break;
		}
	case LUA_TLIGHTUSERDATA:
		{
			write_point(wr,lua_touserdata(L,index));
			break;
		}
	case LUA_TTABLE:
		{
			if(index < 0)
			{
				index = lua_gettop(L) + index +1;
			}
			write_table(L,wr,index,depth+1);
			break;
		}
	default:
		{
			wb_free(wr);
			luaL_error(L,"lua rerialize unsupport type %s",lua_typename(L,type));
			break;
		}
	}
}

static void pack_all(lua_State *L,write_block *wr)
{
	int count = lua_gettop(L);
	for(int i = 1;i <= count;++i)
	{
		pack_one(L,wr,i,0);
	}
}

static void seri(lua_State *L,write_block *wr)
{
	char *buffer = (char*)malloc(wr->m_len);
	block *cur   = wr->m_head;
	int cur_ptr  = 0;
	while(cur != NULL)
	{
		if(cur != wr->m_current)
		{
			memcpy(buffer + cur_ptr,cur->m_buffer,BLOCK_SIZE);
			cur_ptr += BLOCK_SIZE;
		}
		else
		{
			memcpy(buffer + cur_ptr,cur->m_buffer,wr->m_ptr);
			cur_ptr += wr->m_ptr;
		}

		cur = cur->m_next;
	}

	lua_pushlightuserdata(L,buffer);
	lua_pushinteger(L,wr->m_len);
}

double get_real(lua_State *L,read_block *rb)
{
	double *v = (double*)rb_read(rb,sizeof(double));
	if(v == NULL)
	{
		invalid_stream(L,rb);
	}
	return *v;
}

static lua_Integer get_integer(lua_State *L,read_block *rb,unsigned char surplus)
{
	switch((enumber_type)surplus)
	{
	case enumber_zero:
		{
			return 0;
		}
	case enumber_long_long:
		{
			int64_t *v = (int64_t*)rb_read(rb,sizeof(int64_t));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}
			return (lua_Integer)(*v);
		}
	case enumber_dword:
		{
			int *v = (int*)rb_read(rb,sizeof(int));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}
			return (lua_Integer)(*v);
		}
	case enumber_word:
		{
			short *v = (short*)rb_read(rb,sizeof(short));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}
			return (lua_Integer)(*v);
		}
	case enumber_byte:
		{
			unsigned char *v = (unsigned char*)rb_read(rb,sizeof(unsigned char));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}
			return (lua_Integer)(*v);
		}
	default:
		{
			invalid_stream(L,rb);
			break;
		}
	}

	return 0;
}

static void* get_point(lua_State *L,read_block *rb)
{
	void *v = rb_read(rb,sizeof(void*));
	if(v == NULL)
	{
		invalid_stream(L,rb);
	}
	return v;
}

static void get_buffer(lua_State *L,read_block *rb,unsigned int len)
{
	char *v = (char*)rb_read(rb,len);
	if(v == NULL)
	{
		invalid_stream(L,rb);
	}
	lua_pushlstring(L,v,len);
}

unsigned int get_buffer_size(lua_State *L,read_block *rb,unsigned char surplus)
{
	switch((enumber_type)surplus)
	{
	case enumber_dword:
		{
			unsigned int *v = (unsigned int*)rb_read(rb,sizeof(unsigned int));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}
			return (*v);
		}
	case enumber_word:
		{
			unsigned short *v = (unsigned short*)rb_read(rb,sizeof(unsigned short));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}

			return (unsigned short)(*v);
		}
	case enumber_byte:
		{
			unsigned char *v = (unsigned char*)rb_read(rb,sizeof(unsigned char));
			if(v == NULL)
			{
				invalid_stream(L,rb);
			}
			return (unsigned char)(*v);
		}
		default:
		{
			invalid_stream(L,rb);
		}
	}

	return 0;
}

static void unpack_one(lua_State *L,read_block *rb)
{
	unsigned char *type = (unsigned char*)rb_read(rb,sizeof(unsigned char));
	if(type == NULL)
	{
		invalid_stream(L,rb);
	}
	push_value(L,rb,(edata_type)((*type)&0x7),(*type) >> 3);
}

static void unpack_table(lua_State *L,read_block *rb,unsigned char array_size)
{
	if(SURPLUS -1 == array_size)
	{
		unsigned char *type   = (unsigned char*)rb_read(rb,sizeof(unsigned char));
		unsigned char surplus = (int)(*type) >> 3;
		array_size = get_integer(L,rb,surplus);
	}

	lua_createtable(L,array_size,0);
	for(int i = 1;i <= array_size;++i)
	{
		unpack_one(L,rb);
		lua_rawseti(L,-2,i);
	}

	while(true)
	{
		unpack_one(L,rb);
		if(lua_isnil(L,-1))
		{
			lua_pop(L,1);
			break;
		}
		unpack_one(L,rb);
		lua_rawset(L,-3);
	}
}

static void push_value(lua_State *L,read_block *rb,edata_type type,unsigned char surplus)
{
	switch(type)
	{
	case edata_nil:
		{
			lua_pushnil(L);
			break;
		}
	case edata_bool:
		{
			lua_pushboolean(L,surplus);
			break;
		}
	case edata_number:
		{
			if(surplus == enumber_real)
			{
				lua_pushnumber(L,get_real(L,rb));
			}
			else
			{
				lua_pushinteger(L,get_integer(L,rb,surplus));
			}
			break;
		}
	case edata_userdata:
		{
			lua_pushlightuserdata(L,get_point(L,rb));
			break;
		}
	case edata_short_str:
		{
			get_buffer(L,rb,surplus);
			break;
		}
	case edata_long_str:
		{
			get_buffer(L,rb,get_buffer_size(L,rb,surplus));
			break;
		}
	case edata_table:
		{
			unpack_table(L,rb,surplus);
			break;
		}
	}
}

int lua_seri_pack(lua_State *L)
{
	write_block wr;
	pack_all(L,&wr);
	seri(L,&wr);
	wb_free(&wr);

	return 2;
}

int lua_seri_unpack(lua_State *L)
{
	if(lua_gettop(L) != 2)
	{
		luaL_error(L,"serialize unpack param count error");
	}

	if(lua_type(L,1) != LUA_TLIGHTUSERDATA || lua_type(L,2) != LUA_TNUMBER)
	{
		luaL_error(L,"serialize unpack param type error");
	}

	char *buffer = (char*)lua_touserdata(L,1);
	int len      = lua_tointeger(L,2);

	if(len == 0)
	{
		luaL_error(L,"serialize unpack len is 0");
	}

	read_block rb;
	rb.m_buffer = buffer;
	rb.m_len    = len;

	while(true)
	{
		unsigned char *type = (unsigned char*)rb_read(&rb,sizeof(unsigned char));
		if(type == NULL)
		{
			break;
		}
		push_value(L,&rb,(edata_type)((*type)&0x7),(*type) >> 3);
	}

	return lua_gettop(L) - 2;
}
