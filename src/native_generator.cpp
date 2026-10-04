#include "td/tl/tl_generate.h"
#include "td/tl/tl_simple.h"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using td::tl::simple::Constructor;
using td::tl::simple::CustomType;
using td::tl::simple::Function;
using td::tl::simple::Schema;
using td::tl::simple::Type;

class Generator final {
public:
    explicit Generator(const Schema &schema)
        : schema_(schema), custom_types_(), vectors_(), vector_names_()
    {
        for (const CustomType *type : schema_.custom_types) {
            // '#' is the TL natural-number placeholder used by the schema
            // parser. It is not a generated td_api object.
            // 'Type' is the schema's generic type parameter used by vector
            // declarations. It also is not emitted as a td_api object.
            if (type->name != "#" && type->name != "Type") {
                custom_types_.push_back(type);
            }
        }
    }

    void write(const std::string &header_path, const std::string &source_path)
    {
        std::ofstream header(header_path.c_str());
        std::ofstream source(source_path.c_str());
        if (!header || !source) {
            throw std::runtime_error("unable to open native codec output files");
        }

        collect_vectors();
        write_header(header);
        write_source(source);
    }

private:
    const Schema &schema_;
    std::vector<const CustomType *> custom_types_;
    std::vector<const Type *> vectors_;
    std::map<std::string, std::string> vector_names_;

    static std::string cpp_name(const std::string &name)
    {
        return td::tl::simple::gen_cpp_name(name);
    }

    static std::string type_name(const CustomType *type)
    {
        return cpp_name(type->name);
    }

    static std::string constructor_name(const Constructor *constructor)
    {
        return cpp_name(constructor->name);
    }

    static std::string native_type_name(const CustomType *type)
    {
        if (type->constructors.size() == 1) {
            return constructor_name(type->constructors.front());
        }
        return type_name(type);
    }

    static std::string function_name(const Function *function)
    {
        return cpp_name(function->name);
    }

    static std::string vector_token(const Type *type)
    {
        if (type->type != Type::Vector) {
            throw std::runtime_error("internal error: expected vector type");
        }
        const Type *child = type->vector_value_type;
        switch (child->type) {
        case Type::Int32: return "int32";
        case Type::Int53: return "int53";
        case Type::Int64: return "int64";
        case Type::Double: return "double";
        case Type::String: return "string";
        case Type::Bytes: return "bytes";
        case Type::Bool: return "bool";
        case Type::Custom: return "custom_" + type_name(child->custom);
        case Type::Vector: return "vector_" + vector_token(child);
        }
        throw std::runtime_error("internal error: unknown vector type");
    }

    static std::string cpp_type(const Type *type)
    {
        switch (type->type) {
        case Type::Int32: return "std::int32_t";
        case Type::Int53: return "std::int64_t";
        case Type::Int64: return "std::int64_t";
        case Type::Double: return "double";
        case Type::String: return "std::string";
        case Type::Bytes: return "std::string";
        case Type::Bool: return "bool";
        case Type::Custom:
            return "td::td_api::object_ptr<td::td_api::" + native_type_name(type->custom) + ">";
        case Type::Vector:
            return "std::vector<" + cpp_type(type->vector_value_type) + ">";
        }
        throw std::runtime_error("internal error: unknown type");
    }

    void collect_vectors_from(const Type *type)
    {
        if (type->type != Type::Vector) {
            return;
        }
        const std::string token = vector_token(type);
        if (vector_names_.find(token) == vector_names_.end()) {
            vector_names_[token] = "read_vector_" + token;
            vectors_.push_back(type);
        }
        collect_vectors_from(type->vector_value_type);
    }

    void collect_vectors()
    {
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                for (const auto &argument : constructor->args) {
                    collect_vectors_from(argument.type);
                }
            }
        }
        for (const Function *function : schema_.functions) {
            for (const auto &argument : function->args) {
                collect_vectors_from(argument.type);
            }
        }
    }

    void write_header(std::ofstream &out) const
    {
        out << "#pragma once\n\n"
            << "#include <td/telegram/td_api.h>\n\n"
            << "#include <string>\n\n"
            << "struct lua_State;\n\n"
            << "namespace tdlua_native {\n\n"
            << "td::td_api::object_ptr<td::td_api::Function> from_lua("
            << "lua_State *L, int index, const std::string &path);\n"
            << "td::td_api::object_ptr<td::td_api::Object> from_lua_object("
            << "lua_State *L, int index, const std::string &path);\n"
            << "void push_object(lua_State *L, const td::td_api::Object &object);\n\n"
            << "}  // namespace tdlua_native\n";
    }

    void write_source(std::ofstream &out)
    {
        out << "#include \"native_codec.h\"\n"
            << "#include \"native_codec_runtime.h\"\n\n"
            << "#include <td/telegram/td_api.hpp>\n\n"
            << "#include <cstdint>\n"
            << "#include <string>\n"
            << "#include <vector>\n\n"
            << "namespace tdlua_native {\n\n";

        write_forward_declarations(out);
        write_vector_declarations(out);
        write_constructor_readers(out);
        write_function_readers(out);
        write_custom_readers(out);
        write_function_reader(out);
        write_vector_definitions(out);
        write_push_forward_declarations(out);
        write_vector_pushers(out);
        write_constructor_pushers(out);
        write_custom_pushers(out);
        write_object_pusher(out);
        write_public_functions(out);

        out << "\n}  // namespace tdlua_native\n";
    }

    void write_forward_declarations(std::ofstream &out) const
    {
        for (const CustomType *type : custom_types_) {
            out << "static td::td_api::object_ptr<td::td_api::" << native_type_name(type)
                << "> read_" << type_name(type)
                << "(lua_State *L, int index, const std::string &path);\n";
        }
        for (const Function *function : schema_.functions) {
            out << "static td::td_api::object_ptr<td::td_api::" << function_name(function)
                << "> read_" << function_name(function)
                << "(lua_State *L, int index, const std::string &path);\n";
        }
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                out << "static td::td_api::object_ptr<td::td_api::" << constructor_name(constructor)
                    << "> read_" << constructor_name(constructor)
                    << "(lua_State *L, int index, const std::string &path);\n";
            }
        }
        out << "\n";
    }

    void write_vector_declarations(std::ofstream &out) const
    {
        for (const Type *type : vectors_) {
            out << "static " << cpp_type(type) << " " << vector_names_.find(vector_token(type))->second
                << "(lua_State *L, int index, const std::string &path);\n";
        }
        out << "\n";
    }

    void write_push_forward_declarations(std::ofstream &out) const
    {
        for (const Type *type : vectors_) {
            out << "static void push_" << vector_token(type) << "(lua_State *L, const "
                << cpp_type(type) << " &value);\n";
        }
        for (const CustomType *type : custom_types_) {
            out << "static void push_" << type_name(type) << "(lua_State *L, const td::td_api::"
                << native_type_name(type) << " &value);\n";
            for (const Constructor *constructor : type->constructors) {
                out << "static void push_" << constructor_name(constructor)
                    << "(lua_State *L, const td::td_api::" << constructor_name(constructor)
                    << " &value);\n";
            }
        }
        out << "\n";
    }

    std::string read_expression(const Type *type, const std::string &index,
                                const std::string &path) const
    {
        switch (type->type) {
        case Type::Int32:
            return "tdlua_native::read_int32(L, " + index + ", " + path + ")";
        case Type::Int53:
            return "tdlua_native::read_int53(L, " + index + ", " + path + ")";
        case Type::Int64:
            return "tdlua_native::read_int64(L, " + index + ", " + path + ")";
        case Type::Double:
            return "tdlua_native::read_double(L, " + index + ", " + path + ")";
        case Type::String:
        case Type::Bytes:
            return "tdlua_native::read_string(L, " + index + ", " + path + ")";
        case Type::Bool:
            return "tdlua_native::read_bool(L, " + index + ", " + path + ")";
        case Type::Custom:
            return "read_" + type_name(type->custom) + "(L, " + index + ", " + path + ")";
        case Type::Vector:
            return vector_names_.find(vector_token(type))->second + "(L, " + index + ", " + path + ")";
        }
        throw std::runtime_error("internal error: unknown read type");
    }

    std::string push_statement(const Type *type, const std::string &expression) const
    {
        switch (type->type) {
        case Type::Int32:
            return "tdlua_native::push_int32(L, " + expression + ");";
        case Type::Int53:
        case Type::Int64:
            return "tdlua_native::push_integer(L, " + expression + ");";
        case Type::Double:
            return "tdlua_native::push_double(L, " + expression + ");";
        case Type::String:
        case Type::Bytes:
            return "tdlua_native::push_string(L, " + expression + ");";
        case Type::Bool:
            return "tdlua_native::push_bool(L, " + expression + ");";
        case Type::Custom:
            return "push_" + type_name(type->custom) + "(L, *" + expression + ");";
        case Type::Vector:
            return "push_" + vector_token(type) + "(L, " + expression + ");";
        }
        throw std::runtime_error("internal error: unknown push type");
    }

    void write_constructor_readers(std::ofstream &out) const
    {
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "static td::td_api::object_ptr<td::td_api::" << ctor << "> read_" << ctor
                    << "(lua_State *L, int index, const std::string &path) {\n"
                    << "    tdlua_native::require_table(L, index, path);\n"
                    << "    auto result = td::td_api::make_object<td::td_api::" << ctor << ">();\n";
                for (const auto &argument : constructor->args) {
                    const std::string field = td::tl::simple::gen_cpp_field_name(argument.name);
                    const std::string lua_field = argument.name;
                    const std::string field_path = "tdlua_native::path_field(path, \"" + lua_field + "\")";
                    out << "    {\n"
                        << "        tdlua_native::Field field(L, index, \"" << lua_field << "\");\n"
                        << "        result->" << field << " = "
                        << read_expression(argument.type, "field.index()", field_path) << ";\n"
                        << "    }\n";
                }
                out << "    return result;\n}\n\n";
            }
        }
    }

    void write_custom_readers(std::ofstream &out) const
    {
        for (const CustomType *type : custom_types_) {
            out << "static td::td_api::object_ptr<td::td_api::" << native_type_name(type)
                << "> read_" << type_name(type)
                << "(lua_State *L, int index, const std::string &path) {\n"
                << "    if (lua_isnil(L, index)) {\n"
                << "        return nullptr;\n"
                << "    }\n"
                << "    const std::string type = tdlua_native::type_name(L, index, path);\n";
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "    if (type == \"" << constructor->name << "\") {\n"
                    << "        return read_" << ctor << "(L, index, path);\n"
                    << "    }\n";
            }
            out << "    throw tdlua_native::CodecError(\"tdlua: unknown type '" << "\" + type + \""
                << "' at \" + path);\n}\n\n";
        }
    }

    void write_function_readers(std::ofstream &out) const
    {
        for (const Function *function : schema_.functions) {
            const std::string name = function_name(function);
            out << "static td::td_api::object_ptr<td::td_api::" << name << "> read_" << name
                << "(lua_State *L, int index, const std::string &path) {\n"
                << "    tdlua_native::require_table(L, index, path);\n"
                << "    auto result = td::td_api::make_object<td::td_api::" << name << ">();\n";
            for (const auto &argument : function->args) {
                const std::string field = td::tl::simple::gen_cpp_field_name(argument.name);
                const std::string lua_field = argument.name;
                const std::string field_path = "tdlua_native::path_field(path, \"" + lua_field + "\")";
                out << "    {\n"
                    << "        tdlua_native::Field field(L, index, \"" << lua_field << "\");\n"
                    << "        result->" << field << " = "
                    << read_expression(argument.type, "field.index()", field_path) << ";\n"
                    << "    }\n";
            }
            out << "    return result;\n}\n\n";
        }
    }

    void write_function_reader(std::ofstream &out) const
    {
        out << "static td::td_api::object_ptr<td::td_api::Function> read_function("
            << "lua_State *L, int index, const std::string &path) {\n"
            << "    const std::string type = tdlua_native::type_name(L, index, path);\n";
        for (const Function *function : schema_.functions) {
            const std::string name = function_name(function);
            out << "    if (type == \"" << function->name << "\") {\n"
                << "        return read_" << name << "(L, index, path);\n"
                << "    }\n";
        }
        out << "    throw tdlua_native::CodecError(\"tdlua: unknown function '\" + type + \"' at \" + path);\n}\n\n";
    }

    void write_vector_definitions(std::ofstream &out) const
    {
        for (const Type *type : vectors_) {
            out << cpp_type(type) << " " << vector_names_.find(vector_token(type))->second
                << "(lua_State *L, int index, const std::string &path) {\n"
                << "    const lua_Integer length = tdlua_native::array_length(L, index, path);\n"
                << "    " << cpp_type(type) << " result;\n"
                << "    result.reserve(static_cast<std::size_t>(length));\n"
                << "    for (lua_Integer i = 1; i <= length; ++i) {\n"
                << "        tdlua_native::Element element(L, index, i);\n"
                << "        result.push_back(";
            out << read_expression(type->vector_value_type, "element.index()",
                                   "tdlua_native::path_index(path, i)") << ");\n"
                << "    }\n"
                << "    return result;\n}\n\n";
        }
    }

    void write_constructor_pushers(std::ofstream &out) const
    {
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "static void push_" << ctor << "(lua_State *L, const td::td_api::" << ctor
                    << " &value) {\n"
                    << "    lua_newtable(L);\n"
                    << "    tdlua_native::push_type(L, \"" << constructor->name << "\");\n";
                for (const auto &argument : constructor->args) {
                    const std::string field = td::tl::simple::gen_cpp_field_name(argument.name);
                    out << "    lua_pushstring(L, \"" << argument.name << "\");\n";
                    if (argument.type->type == Type::Custom) {
                        out << "    if (value." << field << ") {\n"
                            << "        " << push_statement(argument.type, "value." + field) << "\n"
                            << "    } else {\n"
                            << "        lua_pushnil(L);\n"
                            << "    }\n";
                    } else {
                        out << "    " << push_statement(argument.type, "value." + field) << "\n";
                    }
                    out << "    lua_rawset(L, -3);\n";
                }
                out << "}\n\n";
            }
        }
    }

    void write_vector_pushers(std::ofstream &out) const
    {
        for (const Type *type : vectors_) {
            out << "static void push_" << vector_token(type) << "(lua_State *L, const "
                << cpp_type(type) << " &value) {\n"
                << "    lua_newtable(L);\n"
                << "    for (std::size_t i = 0; i < value.size(); ++i) {\n"
                << "        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));\n";
            if (type->vector_value_type->type == Type::Custom) {
                out << "        if (value[i]) {\n"
                    << "            " << push_statement(type->vector_value_type, "value[i]") << "\n"
                    << "        } else {\n"
                    << "            lua_pushnil(L);\n"
                    << "        }\n";
            } else {
                out << "        " << push_statement(type->vector_value_type, "value[i]") << "\n";
            }
            out << "        lua_rawset(L, -3);\n"
                << "    }\n"
                << "}\n\n";
        }
    }

    void write_custom_pushers(std::ofstream &out) const
    {
        for (const CustomType *type : custom_types_) {
            out << "static void push_" << type_name(type) << "(lua_State *L, const td::td_api::"
                << native_type_name(type) << " &value) {\n";
            if (type->constructors.size() == 1) {
                const std::string ctor = constructor_name(type->constructors.front());
                out << "    push_" << ctor << "(L, value);\n"
                    << "    return;\n}\n\n";
                continue;
            }
            out << "    switch (value.get_id()) {\n";
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "    case td::td_api::" << ctor << "::ID:\n"
                    << "        push_" << ctor << "(L, static_cast<const td::td_api::"
                    << ctor << " &>(value));\n"
                    << "        return;\n";
            }
            out << "    default:\n"
                << "        throw tdlua_native::CodecError(\"tdlua: unsupported TDLib object type\");\n"
                << "    }\n}\n\n";
        }
    }

    void write_object_pusher(std::ofstream &out) const
    {
        out << "void push_object(lua_State *L, const td::td_api::Object &value) {\n"
            << "    switch (value.get_id()) {\n";
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "    case td::td_api::" << ctor << "::ID:\n"
                    << "        push_" << ctor << "(L, static_cast<const td::td_api::"
                    << ctor << " &>(value));\n"
                    << "        return;\n";
            }
        }
        out << "    default:\n"
            << "        throw tdlua_native::CodecError(\"tdlua: unsupported TDLib object type\");\n"
            << "    }\n}\n\n";
    }

    void write_public_functions(std::ofstream &out) const
    {
        out << "td::td_api::object_ptr<td::td_api::Function> from_lua("
            << "lua_State *L, int index, const std::string &path) {\n"
            << "    return read_function(L, index, path);\n"
            << "}\n\n"
            << "td::td_api::object_ptr<td::td_api::Object> from_lua_object("
            << "lua_State *L, int index, const std::string &path) {\n"
            << "    const std::string type = tdlua_native::type_name(L, index, path);\n";
        for (const CustomType *custom : custom_types_) {
            for (const Constructor *constructor : custom->constructors) {
                out << "    if (type == \"" << constructor->name << "\") {\n"
                    << "        return read_" << type_name(custom) << "(L, index, path);\n"
                    << "    }\n";
            }
        }
        out << "    throw tdlua_native::CodecError(\"tdlua: unknown object type '\" + type + \"' at \" + path);\n"
            << "}\n";
    }
};

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: native_generator <td_api.tlo> <header> <source>\n";
        return 2;
    }
    try {
        const auto config = td::tl::read_tl_config_from_file(argv[1]);
        const td::tl::simple::Schema schema(config);
        Generator generator(schema);
        generator.write(argv[2], argv[3]);
    } catch (const std::exception &error) {
        std::cerr << "native_generator: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
