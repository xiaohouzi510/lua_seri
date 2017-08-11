local function p(t)
	print("---------------")
	for k,v in pairs(t) do
		print(k,v)
	end
end

local pack_table = {"hello world",10086,33.06,one="this my first"}
p(pack_table)
local lightuserdata,len = seri.pack(pack_table)
local result = seri.unpack(lightuserdata,len)
p(result)