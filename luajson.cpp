#include <rapidjson/reader.h>
#include <rapidjson/writer.h>
#include <vector>
#include <limits>

#include "luajson.h"

//This too was brutally copied from https://github.com/xpol/lua-rapidjson/blob/master/src/values.hpp#L69
//Because i'm not able to make a good parser that does not segfault :(
//And if i try to make it i get something that's preatty close to this
class Decoder {
public:
    explicit Decoder(lua_State* aL) : L(aL) {stack_.reserve(32);}

    bool Null() {
        lua_pushnil(L);
        context_.submit(L);
        return true;
    }
    bool Bool(bool b) {
        lua_pushboolean(L, b);
        context_.submit(L);
        return true;
    }
    bool Int(int i) {
        lua_pushinteger(L, i);
        context_.submit(L);
        return true;
    }
    bool Uint(unsigned u) {
        if (sizeof(lua_Integer) > sizeof(unsigned int) || u <= static_cast<unsigned>(std::numeric_limits<lua_Integer>::max()))
            lua_pushinteger(L, static_cast<lua_Integer>(u));
        else
            lua_pushnumber(L, static_cast<lua_Number>(u));
        context_.submit(L);
        return true;
    }
    bool Int64(int64_t i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i));
        context_.submit(L);
        return true;
    }
    bool Uint64(uint64_t u) {
        if (sizeof(lua_Integer) > sizeof(uint64_t) || u <= static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max()))
            lua_pushinteger(L, static_cast<lua_Integer>(u));
        else
            lua_pushnumber(L, static_cast<lua_Number>(u));
        context_.submit(L);
        return true;
    }
    bool Double(double d) {
        lua_pushnumber(L, static_cast<lua_Number>(d));
        context_.submit(L);
        return true;
    }
    bool RawNumber(const char* str, rapidjson::SizeType length, bool copy) {
        lua_pushlstring(L, str, length);
        context_.submit(L);
        return true;
    }
    bool String(const char* str, rapidjson::SizeType length, bool copy) {
        lua_pushlstring(L, str, length);
        context_.submit(L);
        return true;
    }
    bool StartObject() {
        lua_newtable(L);
        stack_.push_back(context_);
        context_ = Ctx::Object();
        return true;
    }
    bool Key(const char* str, rapidjson::SizeType length, bool copy) const {
        lua_pushlstring(L, str, length);
        return true;
    }
    bool EndObject(rapidjson::SizeType memberCount) {
        context_ = stack_.back();
        stack_.pop_back();
        context_.submit(L);
        return true;
    }
    bool StartArray() {
        lua_createtable(L, 0, 0);

        stack_.push_back(context_);
        context_ = Ctx::Array();
        return true;
    }
    bool EndArray(rapidjson::SizeType elementCount) {
        assert(elementCount == context_.index_);
        context_ = stack_.back();
        stack_.pop_back();
        context_.submit(L);
        return true;
    }
private:
    struct Ctx {
        Ctx() : index_(0), fn_(&topFn) {}
        Ctx(const Ctx& rhs) : index_(rhs.index_), fn_(rhs.fn_)
        {
        }
        const Ctx& operator=(const Ctx& rhs) {
            if (this != &rhs) {
                index_ = rhs.index_;
                fn_ = rhs.fn_;
            }
            return *this;
        }
        static Ctx Object() {
            return Ctx(&objectFn);
        }
        static Ctx Array()
        {
            return Ctx(&arrayFn);
        }
        void submit(lua_State* L)
        {
            fn_(L, this);
        }

        int index_;
        void(*fn_)(lua_State* L, Ctx* ctx);
    private:
        explicit Ctx(void(*f)(lua_State* L, Ctx* ctx)) : index_(0), fn_(f) {}


        static void objectFn(lua_State* L, Ctx* ctx)
        {
            lua_rawset(L, -3);
        }

        static void arrayFn(lua_State* L, Ctx* ctx)
        {
            lua_rawseti(L, -2, ++ctx->index_);
        }
        static void topFn(lua_State* L, Ctx* ctx)
        {
        }
    };

    lua_State* L;
    std::vector <Ctx> stack_;
    Ctx context_;
};

void lua_pushjson(lua_State *L, std::string jstr)
{
    Decoder handler(L);
    //auto reader = new rapidjson::Reader();
    rapidjson::Reader reader;
    rapidjson::StringStream stream(jstr.c_str());
    reader.Parse(stream, handler);
    //delete reader;
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
    //auto writer = new rapidjson::Writer<rapidjson::StringBuffer> (s);
    rapidjson::Writer<rapidjson::StringBuffer> writer(s);
    encode(L, &writer);
    //delete writer;
    str = s.GetString();
}
