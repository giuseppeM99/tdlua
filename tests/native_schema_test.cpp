#include "td/tl/tl_generate.h"
#include "td/tl/tl_simple.h"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using td::tl::simple::Constructor;
using td::tl::simple::CustomType;
using td::tl::simple::Function;
using td::tl::simple::Schema;

std::string cpp_name(std::string name)
{
    for (char &character : name) {
        if ((character < '0' || character > '9') &&
            (character < 'a' || character > 'z') &&
            (character < 'A' || character > 'Z')) {
            character = '_';
        }
    }
    return name;
}

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read_generated_sources(const std::string &directory,
                                   std::size_t shard_count)
{
    std::ostringstream result;
    for (std::size_t shard = 0; shard < shard_count; ++shard) {
        const std::string path = directory + "/native_codec_" +
                                 std::to_string(shard) + ".cpp";
        std::ifstream input(path.c_str(), std::ios::binary);
        require(static_cast<bool>(input), "missing generated shard: " + path);
        result << input.rdbuf();
    }

    const std::string dispatch_path = directory + "/native_codec_dispatch.cpp";
    std::ifstream dispatch(dispatch_path.c_str(), std::ios::binary);
    require(static_cast<bool>(dispatch), "missing generated dispatch: " + dispatch_path);
    result << dispatch.rdbuf();
    return result.str();
}

struct GeneratedSymbols final {
    std::set<std::string> readers;
    std::set<std::string> pushers;
    std::set<std::string> lua_dispatch;
    std::set<std::string> native_dispatch;
};

GeneratedSymbols collect_symbols(const std::string &generated)
{
    GeneratedSymbols symbols;
    std::istringstream lines(generated);
    std::string line;
    while (std::getline(lines, line)) {
        const auto collect_function = [&](const char *prefix,
                                           std::set<std::string> &destination) {
            const std::size_t start = line.find(prefix);
            if (start == std::string::npos) {
                return;
            }
            const std::size_t name_start = start + std::string(prefix).size();
            const std::size_t end = line.find('(', name_start);
            if (end != std::string::npos && end > name_start) {
                destination.insert(line.substr(name_start, end - name_start));
            }
        };
        collect_function("read_", symbols.readers);
        collect_function("push_", symbols.pushers);

        const std::string lua_prefix = "if (type == \"";
        const std::size_t lua_start = line.find(lua_prefix);
        if (lua_start != std::string::npos) {
            const std::size_t name_start = lua_start + lua_prefix.size();
            const std::size_t end = line.find("\")", name_start);
            if (end != std::string::npos) {
                symbols.lua_dispatch.insert(line.substr(name_start, end - name_start));
            }
        }

        const std::string native_prefix = "case td::td_api::";
        const std::size_t native_start = line.find(native_prefix);
        if (native_start != std::string::npos) {
            const std::size_t name_start = native_start + native_prefix.size();
            const std::size_t end = line.find("::ID:", name_start);
            if (end != std::string::npos) {
                symbols.native_dispatch.insert(line.substr(name_start, end - name_start));
            }
        }
    }
    return symbols;
}

void check_unique(std::set<std::string> &names, const std::string &name,
                  const char *category)
{
    require(names.insert(name).second,
            std::string("duplicate ") + category + " in TDLib schema: " + name);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: native_schema_test <td_api.tlo> <generated-dir> <shards>\n";
        return 2;
    }

    try {
        const auto config = td::tl::read_tl_config_from_file(argv[1]);
        const Schema schema(config);
        const std::string generated = read_generated_sources(
            argv[2], static_cast<std::size_t>(std::stoul(argv[3])));
        const GeneratedSymbols symbols = collect_symbols(generated);
        std::set<std::string> names;
        std::size_t constructor_count = 0;

        for (const CustomType *type : schema.custom_types) {
            if (type->name == "#" || type->name == "Type") {
                continue;
            }
            const std::string type_cpp = cpp_name(type->name);
            require(symbols.readers.find(type_cpp) != symbols.readers.end(),
                    "missing reader for TL type: " + type->name);
            require(symbols.pushers.find(type_cpp) != symbols.pushers.end(),
                    "missing pusher for TL type: " + type->name);

            for (const Constructor *constructor : type->constructors) {
                ++constructor_count;
                check_unique(names, constructor->name, "constructor");
                const std::string constructor_cpp = cpp_name(constructor->name);
                require(symbols.readers.find(constructor_cpp) != symbols.readers.end(),
                        "missing reader for constructor: " + constructor->name);
                require(symbols.pushers.find(constructor_cpp) != symbols.pushers.end(),
                        "missing pusher for constructor: " + constructor->name);
                require(symbols.lua_dispatch.find(constructor->name) !=
                            symbols.lua_dispatch.end(),
                        "constructor missing from Lua object dispatch: " +
                            constructor->name);
                require(symbols.native_dispatch.find(constructor_cpp) !=
                            symbols.native_dispatch.end(),
                        "constructor missing from native object dispatch: " +
                            constructor->name);
            }
        }

        for (const Function *function : schema.functions) {
            check_unique(names, function->name, "function");
            const std::string function_cpp = cpp_name(function->name);
            require(symbols.readers.find(function_cpp) != symbols.readers.end(),
                    "missing reader for function: " + function->name);
            require(symbols.lua_dispatch.find(function->name) !=
                        symbols.lua_dispatch.end(),
                    "function missing from native function dispatch: " +
                        function->name);
        }

        std::cout << "validated " << constructor_count << " constructors and "
                  << schema.functions.size() << " functions\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
