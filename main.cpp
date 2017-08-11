#include <stdio.h>
#include <lua.hpp>
#include "lua_seri.h"

static const struct luaL_reg s_seri_reg [] = {
	{"pack",lua_seri_pack},
	{"unpack",lua_seri_unpack},
    {NULL, NULL}
};

void register_seri(lua_State *L)
{
	lua_newtable(L);
	luaL_register(L, "seri", s_seri_reg);
}

int main(int argc,char *argv[])
{
	lua_State *L = luaL_newstate();
	if(L == NULL)
	{
		printf("create luastate error\n");
		return 0;
	}

	luaL_openlibs(L);
	register_seri(L);
	const char *filename = "test.lua";
	if(luaL_loadfile(L,filename) != 0)
	{
		printf("lua load file %s error %s\n",filename,lua_tostring(L,-1));
		lua_close(L);
		return 0;
	}
	if(lua_pcall(L,0,0,0) != 0)
	{
		printf("lua call error %s\n",lua_tostring(L,-1));
		lua_close(L);
		return 0;
	}
	lua_close(L);
	return 0;
}