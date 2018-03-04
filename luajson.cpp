#include <rapidjson/reader.h>
#include <rapidjson/writer.h>

#include "luajson.h"

class Decoder {
private:
    lua_State *L;
    size_t current;
    bool isarr;
    bool isobj;
    void end()
    {
        lua_checkstack(L, 4);
        if(isobj) {
            lua_settable(L, -3);
        } else if(isarr) {
            lua_rawseti(L, -2, static_cast<lua_Integer>(++current));
        }
    }
public:
    explicit Decoder(lua_State *L)
    {
        this->L  = L;
        this->isarr = false;
        this->isobj = false;
        this->current = 0;
    }
    bool Null()
    {
        lua_pushnil(L);
        end();
        return true;
    }
    bool Bool(bool b)
    {
        lua_pushboolean(L, b);
        end();
        return true;
    }
    bool Int(int i)
    {
        lua_pushinteger(L, i);
        end();
        return true;
    }
    bool Uint(unsigned i)
    {
        lua_pushinteger(L, i);
        end();
        return true;
    }
    bool Int64(int64_t i)
    {
        lua_pushinteger(L, i);
        end();
        return true;
    }
    bool Uint64(uint64_t i)
    {
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        end();
        return true;
    }
    bool Double(double d)
    {
        lua_pushnumber(L, d);
        end();
        return true;
    }
    bool RawNumber(const char* str, size_t length, bool copy)
    {
        lua_pushlstring(L, str, length);
        end();
        return true;
    }
    bool String(const char* str, size_t length, bool copy)
    {
        lua_pushlstring(L, str, length);
        end();
        return true;
    }
    bool StartObject()
    {
        lua_newtable(L);
        isobj = true;
        return true;
    }
    bool Key(const char* str, size_t length, bool copy)
    {
        lua_pushlstring(L, str, length);
        return true;
    }
    bool EndObject(size_t memberCount)
    {
        isobj = false;
        end();
        return true;
    }
    bool StartArray()
    {
        lua_newtable(L);
        current = 0;
        isarr = true;
        return true;
    }
    bool EndArray(size_t elementCount)
    {
        isarr = false;
        end();
        return true;
    }
};

void lua_pushjson(lua_State *L, std::string jstr)
{
    Decoder handler(L);
    rapidjson::Reader reader;
    rapidjson::StringStream stream(jstr.c_str());
    reader.Parse(stream, handler);
}


//mostly copied from https://github.com/vysheng/tdbot/blob/master/clilua.cpp#L10

template<class T>
void encode(lua_State *L, T *writer)
{
    int type = lua_type(L, -1);
    if (type == LUA_TNIL) {
        writer->Null();
    } else if (type == LUA_TBOOLEAN) {
        writer->Bool(lua_toboolean(L, -1));
    } else if (type == LUA_TNUMBER) {
        lua_Number x = lua_tonumber(L, -1);
        lua_Integer xi = lua_tointeger(L, -1);
        if(x == xi) {
            writer->Int64(xi);
        } else {
            writer->Double(x);
        }
        return;
    } else if (type == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        writer->String(s, len);
        return;
    } else if (type == LUA_TTABLE) {

        bool arr = true;
        int size = 0;

        lua_pushnil(L);
        while (lua_next (L, -2)) {
            if (!(arr && lua_type(L, -2) == LUA_TNUMBER && lua_tointeger(L, -2)-1 == size)) {
                arr = false;
            }
            size++;
            lua_pop(L, 1);
        }

        if (arr) {
            writer->StartArray();
        } else {
            writer->StartObject();
        }

        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (arr) {
                encode(L, writer);
                lua_pop(L, 1);
            } else {
                size_t len;

                if (lua_type(L, -2) == LUA_TNUMBER) {
                    lua_Number x = lua_tonumber(L, -2);
                    lua_Integer x64 = lua_tointeger(L, -2);
                    if (x == x64) {
                        writer->Key(std::to_string(x64-1).c_str());
                        encode(L, writer);
                    } else {
                        writer->Key(std::to_string(x-1).c_str());
                        encode(L, writer);
                    }
                } else {
                    const char *key = lua_tolstring(L, -2, &len);
                    writer->Key(key, len);
                    encode(L, writer);
                }
                lua_pop(L, 1);
            }
        }
        if (arr) {
            writer->EndArray();
        } else {
            writer->EndObject();
        }
    } else {
        return;
    }
}

void lua_getjson(lua_State *L, std::string &str)
{
    rapidjson::StringBuffer s;
    rapidjson::Writer<rapidjson::StringBuffer> writer(s);
    encode(L, &writer);
    str = s.GetString();
}
