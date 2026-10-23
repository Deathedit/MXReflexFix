#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Memory
{
    // IDA-style pattern ("8B 70 ?? 8B 01"), returns the first match inside the module image.
    inline std::uint8_t* PatternScan(HMODULE module, const char* signature)
    {
        std::vector<int> bytes;
        for (const char* c = signature; *c;)
        {
            if (*c == ' ')
            {
                ++c;
                continue;
            }
            if (*c == '?')
            {
                bytes.push_back(-1);
                while (*c == '?')
                    ++c;
                continue;
            }
            bytes.push_back(static_cast<int>(std::strtoul(c, const_cast<char**>(&c), 16)));
        }

        auto* base = reinterpret_cast<std::uint8_t*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
        const std::size_t count = bytes.size();
        if (count == 0 || count > imageSize)
            return nullptr;

        for (std::size_t i = 0; i <= imageSize - count; ++i)
        {
            bool found = true;
            for (std::size_t j = 0; j < count; ++j)
            {
                if (bytes[j] != -1 && base[i + j] != static_cast<std::uint8_t>(bytes[j]))
                {
                    found = false;
                    break;
                }
            }
            if (found)
                return base + i;
        }
        return nullptr;
    }
}

namespace Util
{
    // graphics.cfg is "Key=Value" lines; '#' starts a comment.
    inline std::map<std::string, std::string> ReadGraphicsCfg(const std::filesystem::path& path)
    {
        std::map<std::string, std::string> values;
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            auto trim = [](std::string s) {
                const auto first = s.find_first_not_of(" \t\r");
                const auto last = s.find_last_not_of(" \t\r");
                return first == std::string::npos ? std::string{} : s.substr(first, last - first + 1);
            };
            values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return values;
    }

    inline std::optional<int> ToInt(const std::map<std::string, std::string>& values, const std::string& key)
    {
        const auto it = values.find(key);
        if (it == values.end())
            return std::nullopt;
        try
        {
            return std::stoi(it->second);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
