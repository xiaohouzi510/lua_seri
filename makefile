all : main 

main : main.cpp lua_seri.cpp
	g++ -g -o $@ $^ -llua -ldl
clean :
	rm -rf main 
