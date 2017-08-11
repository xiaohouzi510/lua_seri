#ifndef LUA_SERI_H
#define LUA_SERI_H

struct lua_State;

int lua_seri_pack(lua_State *L);

int lua_seri_unpack(lua_State *L);

#endif