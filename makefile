all : main 

main : main.cpp lua_seri.cpp
	g++ -g -o $@ $^ -llua

clean :
	rm -rf main 