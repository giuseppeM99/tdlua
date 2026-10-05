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
        std::set<std::string> names;
        std::size_t constructor_count = 0;

        for (const CustomType *type : schema.custom_types) {
            if (type->name == "#" || type->name == "Type") {
                continue;
            }
            const std::string type_cpp = cpp_name(type->name);
            require(generated.find("read_" + type_cpp + "(") != std::string::npos,
                    "missing reader for TL type: " + type->name);
            require(generated.find("push_" + type_cpp + "(") != std::string::npos,
                    "missing pusher for TL type: " + type->name);

            for (const Constructor *constructor : type->constructors) {
                ++constructor_count;
                check_unique(names, constructor->name, "constructor");
                const std::string constructor_cpp = cpp_name(constructor->name);
                require(generated.find("read_" + constructor_cpp + "(") !=
                            std::string::npos,
                        "missing reader for constructor: " + constructor->name);
                require(generated.find("push_" + constructor_cpp + "(") !=
                            std::string::npos,
                        "missing pusher for constructor: " + constructor->name);
                require(generated.find("if (type == \"" + constructor->name +
                                     "\")") != std::string::npos,
                        "constructor missing from Lua object dispatch: " +
                            constructor->name);
                require(generated.find("case td::td_api::" + constructor_cpp +
                                     "::ID:") != std::string::npos,
                        "constructor missing from native object dispatch: " +
                            constructor->name);
            }
        }

        for (const Function *function : schema.functions) {
            check_unique(names, function->name, "function");
            const std::string function_cpp = cpp_name(function->name);
            require(generated.find("read_" + function_cpp + "(") !=
                        std::string::npos,
                    "missing reader for function: " + function->name);
            require(generated.find("if (type == \"" + function->name +
                                 "\")") != std::string::npos,
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
