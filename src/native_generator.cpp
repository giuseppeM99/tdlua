#include "td/tl/tl_generate.h"
#include "td/tl/tl_simple.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

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

    void write(const std::string &header_path, const std::string &source_directory,
               std::size_t shard_count)
    {
        collect_vectors();
        std::ostringstream header;
        write_header(header);
        write_if_different(header_path, header.str());

        std::ostringstream source;
        write_source(source);
        write_shards(source.str(), source_directory, shard_count);
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

    static bool is_definition_start(const std::string &line)
    {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return false;
        }
        std::string candidate = line.substr(first);
        if (candidate.compare(0, 7, "static ") == 0) {
            candidate.erase(0, 7);
        }
        const bool result = candidate.compare(0, 4, "td::") == 0 ||
               candidate.compare(0, 5, "std::") == 0 ||
               candidate.compare(0, 5, "void ") == 0;
        return result;
    }

    static std::vector<std::string> split_definitions(const std::string &source,
                                                       std::string &preamble)
    {
        std::vector<std::string> definitions;
        std::size_t line_start = 0;
        std::size_t gap_start = 0;

        while (line_start < source.size()) {
            const std::size_t line_end = source.find('\n', line_start);
            const std::size_t end = line_end == std::string::npos ? source.size() : line_end + 1;
            const std::string line = source.substr(line_start, end - line_start);
            if (is_definition_start(line) && line.find('{') != std::string::npos) {
                preamble += source.substr(gap_start, line_start - gap_start);
                std::size_t brace = source.find('{', line_start);
                int depth = 0;
                bool in_string = false;
                char quote = '\0';
                for (std::size_t i = brace; i < source.size(); ++i) {
                    const char character = source[i];
                    if (in_string) {
                        if (character == '\\') {
                            ++i;
                        } else if (character == quote) {
                            in_string = false;
                        }
                        continue;
                    }
                    if (character == '\"' || character == '\'') {
                        in_string = true;
                        quote = character;
                    } else if (character == '{') {
                        ++depth;
                    } else if (character == '}' && --depth == 0) {
                        const std::size_t definition_end =
                            source.find('\n', i) == std::string::npos ? source.size()
                                                                       : source.find('\n', i) + 1;
                        definitions.push_back(source.substr(line_start, definition_end - line_start));
                        gap_start = definition_end;
                        line_start = definition_end;
                        break;
                    }
                }
                continue;
            }
            line_start = end;
        }
        preamble += source.substr(gap_start);
        const std::string namespace_end = "\n}  // namespace tdlua_native\n";
        const std::size_t namespace_position = preamble.rfind(namespace_end);
        if (namespace_position != std::string::npos) {
            preamble.erase(namespace_position);
        }
        if (definitions.empty()) {
            throw std::runtime_error("native generator emitted no codec definitions");
        }
        return definitions;
    }

    static std::uint64_t stable_hash(const std::string &value)
    {
        // FNV-1a is deliberately used instead of std::hash: generated shard
        // assignment must be identical across standard libraries and hosts.
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char character : value) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static std::string definition_symbol(const std::string &definition)
    {
        const std::size_t open = definition.find('(');
        if (open == std::string::npos) {
            return definition;
        }
        const std::size_t end = definition.find_last_not_of(" \t\n", open - 1);
        const std::size_t begin = definition.find_last_of(" \t\n:*", end);
        return definition.substr(begin == std::string::npos ? 0 : begin + 1,
                                 end - (begin == std::string::npos ? 0 : begin + 1) + 1);
    }

    static bool is_dispatch_symbol(const std::string &symbol)
    {
        return symbol == "read_function" || symbol == "push_object" ||
               symbol == "from_lua" || symbol == "from_lua_object";
    }

    static void write_if_different(const std::string &path, const std::string &contents)
    {
        std::ifstream input(path.c_str(), std::ios::binary);
        if (input) {
            std::ostringstream existing;
            existing << input.rdbuf();
            if (existing.str() == contents) {
                return;
            }
        }

        const std::string temporary_path = path + ".tmp";
        std::ofstream output(temporary_path.c_str(),
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("unable to open native codec output file: " + path);
        }
        output << contents;
        if (!output) {
            output.close();
            std::remove(temporary_path.c_str());
            throw std::runtime_error("unable to write native codec output file: " + path);
        }
        output.close();
#ifdef _WIN32
        const BOOL replaced = MoveFileExA(
            temporary_path.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!replaced) {
#else
        if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
#endif
            std::remove(temporary_path.c_str());
            throw std::runtime_error("unable to replace native codec output file: " + path);
        }
    }

    void write_shards(const std::string &source, const std::string &directory,
                      std::size_t shard_count) const
    {
        if (shard_count == 0) {
            throw std::runtime_error("native codec shard count must be positive");
        }
        std::string preamble;
        std::vector<std::string> definitions = split_definitions(source, preamble);
        std::vector<std::vector<std::string>> shards(shard_count);
        std::vector<std::string> dispatch;
        for (const std::string &definition : definitions) {
            const std::string symbol = definition_symbol(definition);
            if (is_dispatch_symbol(symbol)) {
                dispatch.push_back(definition);
                continue;
            }
            const std::size_t target = static_cast<std::size_t>(
                stable_hash(symbol) % shard_count);
            shards[target].push_back(definition);
        }

        for (std::size_t shard = 0; shard < shard_count; ++shard) {
            std::ostringstream output;
            output << preamble;
            for (const std::string &definition : shards[shard]) {
                output << definition << '\n';
            }
            output << "}  // namespace tdlua_native\n";
            write_if_different(directory + "/native_codec_" +
                                   std::to_string(shard) + ".cpp",
                               output.str());
        }

        std::ostringstream dispatch_output;
        dispatch_output << preamble;
        for (const std::string &definition : dispatch) {
            dispatch_output << definition << '\n';
        }
        dispatch_output << "}  // namespace tdlua_native\n";
        write_if_different(directory + "/native_codec_dispatch.cpp",
                           dispatch_output.str());
    }

    void write_header(std::ostream &out) const
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

    void write_source(std::ostream &out)
    {
        out << "#include \"tdlua/native_codec.h\"\n"
            << "#include \"tdlua/native_codec_runtime.h\"\n\n"
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

    void write_forward_declarations(std::ostream &out) const
    {
        for (const CustomType *type : custom_types_) {
            out << "td::td_api::object_ptr<td::td_api::" << native_type_name(type)
                << "> read_" << type_name(type)
                << "(lua_State *L, int index, const std::string &path);\n";
        }
        for (const Function *function : schema_.functions) {
            out << "td::td_api::object_ptr<td::td_api::" << function_name(function)
                << "> read_" << function_name(function)
                << "(lua_State *L, int index, const std::string &path);\n";
        }
        out << "td::td_api::object_ptr<td::td_api::Function> read_function("
            << "lua_State *L, int index, const std::string &path);\n";
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                out << "td::td_api::object_ptr<td::td_api::" << constructor_name(constructor)
                    << "> read_" << constructor_name(constructor)
                    << "(lua_State *L, int index, const std::string &path);\n";
            }
        }
        out << "\n";
    }

    void write_vector_declarations(std::ostream &out) const
    {
        for (const Type *type : vectors_) {
            out << cpp_type(type) << " " << vector_names_.find(vector_token(type))->second
                << "(lua_State *L, int index, const std::string &path);\n";
        }
        out << "\n";
    }

    void write_push_forward_declarations(std::ostream &out) const
    {
        for (const Type *type : vectors_) {
            out << "void push_" << vector_token(type) << "(lua_State *L, const "
                << cpp_type(type) << " &value);\n";
        }
        for (const CustomType *type : custom_types_) {
            out << "void push_" << type_name(type) << "(lua_State *L, const td::td_api::"
                << native_type_name(type) << " &value);\n";
            for (const Constructor *constructor : type->constructors) {
                out << "void push_" << constructor_name(constructor)
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

    void write_constructor_readers(std::ostream &out) const
    {
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "td::td_api::object_ptr<td::td_api::" << ctor << "> read_" << ctor
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

    void write_custom_readers(std::ostream &out) const
    {
        for (const CustomType *type : custom_types_) {
            out << "td::td_api::object_ptr<td::td_api::" << native_type_name(type)
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

    void write_function_readers(std::ostream &out) const
    {
        for (const Function *function : schema_.functions) {
            const std::string name = function_name(function);
            out << "td::td_api::object_ptr<td::td_api::" << name << "> read_" << name
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

    void write_function_reader(std::ostream &out) const
    {
        out << "td::td_api::object_ptr<td::td_api::Function> read_function("
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

    void write_vector_definitions(std::ostream &out) const
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

    void write_constructor_pushers(std::ostream &out) const
    {
        for (const CustomType *type : custom_types_) {
            for (const Constructor *constructor : type->constructors) {
                const std::string ctor = constructor_name(constructor);
                out << "void push_" << ctor << "(lua_State *L, const td::td_api::" << ctor
                    << " &value) {\n"
                    << "    lua_newtable(L);\n"
                    << "    tdlua_native::push_type(L, \"" << constructor->name << "\");\n";
                if (constructor->args.empty()) {
                    out << "    (void)value;\n";
                }
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

    void write_vector_pushers(std::ostream &out) const
    {
        for (const Type *type : vectors_) {
            out << "void push_" << vector_token(type) << "(lua_State *L, const "
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

    void write_custom_pushers(std::ostream &out) const
    {
        for (const CustomType *type : custom_types_) {
            out << "void push_" << type_name(type) << "(lua_State *L, const td::td_api::"
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

    void write_object_pusher(std::ostream &out) const
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

    void write_public_functions(std::ostream &out) const
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
    if (argc != 5) {
        std::cerr << "usage: native_generator <td_api.tlo> <header> <shard-directory> <shard-count>\n";
        return 2;
    }
    try {
        const auto config = td::tl::read_tl_config_from_file(argv[1]);
        const td::tl::simple::Schema schema(config);
        Generator generator(schema);
        generator.write(argv[2], argv[3], static_cast<std::size_t>(std::stoul(argv[4])));
    } catch (const std::exception &error) {
        std::cerr << "native_generator: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
