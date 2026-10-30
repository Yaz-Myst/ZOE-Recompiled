#include "ps2recomp/Emitters/function_table_emitter.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps2recomp
{
    FunctionTableEmitter::FunctionTableEmitter(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::string FunctionTableEmitter::emit(const std::vector<Function> &functions, const std::map<uint32_t, std::string> &stubs)
    {
        (void)stubs;

        CodeGenerator &cg = m_codeGenerator;
        std::vector<std::pair<uint32_t, std::string>> entries;
        std::unordered_set<uint32_t> registeredAddresses;

        auto addEntry = [&](uint32_t address, const std::string &name)
        {
            if (name.empty())
            {
                return;
            }
            if ((address & 3u) != 0u)
            {
                std::ostringstream oss;
                oss << "Unaligned function table entry for " << name << " at 0x" << std::hex << address;

                if (cg.m_reporter)
                {
                    cg.m_reporter->errorAt("function-table", name, address, oss.str());
                }

                throw std::runtime_error(oss.str());
            }
            if (!registeredAddresses.insert(address).second)
            {
                return;
            }
            entries.emplace_back(address, name);
        };

        std::vector<std::pair<uint32_t, std::string>> normalFunctions;
        std::vector<std::pair<uint32_t, std::string>> stubFunctions;
        std::vector<std::pair<uint32_t, std::string>> systemCallFunctions;
        std::vector<std::pair<uint32_t, std::string>> libraryFunctions;

        for (const auto &function : functions)
        {
            if (!function.isRecompiled && !function.isStub && !function.isSkipped)
                continue;

            std::string generatedName = cg.getFunctionName(function.start);

            if (function.isSkipped)
            {
                libraryFunctions.emplace_back(function.start, generatedName);
            }
            else if (function.isStub)
            {
                const auto target = PS2Recompiler::resolveStubTarget(function.name);
                if (target == StubTarget::Syscall)
                {
                    systemCallFunctions.emplace_back(function.start, generatedName);
                }
                else
                {
                    stubFunctions.emplace_back(function.start, generatedName);
                }
            }
            else
            {
                normalFunctions.emplace_back(function.start, generatedName);
            }
        }

        if (cg.m_bootstrapInfo.valid)
        {
            std::string entryTarget = cg.m_bootstrapInfo.entryName;
            if (entryTarget.empty())
            {
                entryTarget = cg.getFunctionName(cg.m_bootstrapInfo.entry);
            }
            if (entryTarget.empty())
            {
                throw std::runtime_error("No entry function name available for registration.");
            }
            addEntry(cg.m_bootstrapInfo.entry, entryTarget);
        }

        for (const auto &[address, name] : normalFunctions)
        {
            addEntry(address, name);
        }

        for (const auto &[ownerStart, targets] : cg.m_resumeEntryTargetsByOwner)
        {
            const std::string ownerName = cg.getFunctionName(ownerStart);
            if (ownerName.empty())
            {
                continue;
            }

            for (uint32_t target : targets)
            {
                addEntry(target, ownerName);
            }
        }

        for (const auto &[address, name] : stubFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : systemCallFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : libraryFunctions)
        {
            addEntry(address, name);
        }

        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b)
                  { return a.first < b.first; });

        uint32_t tableBase = 0u;
        uint32_t tableEnd = 0u;
        uint32_t slotCount = 0u;
        if (!entries.empty())
        {
            tableBase = entries.front().first & ~3u;
            tableEnd = (entries.back().first + 4u + 3u) & ~3u;
            slotCount = (tableEnd - tableBase) >> 2;
        }

        std::stringstream ss;
        ss << "#include \"ps2_runtime.h\"\n";
        ss << "#include <ps2_recompiled_functions.h>\n";
        ss << "#include \"ps2_stubs.h\"\n";
        ss << "#include <ps2_recompiled_stubs.h>\n";
        ss << "#include \"ps2_syscalls.h\"\n\n";

        if (!cg.m_overlayName.empty())
        {
            emitOverlayTable(ss, entries, tableBase, tableEnd, slotCount);
            return ss.str();
        }

        ss << "extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x" << std::hex << tableBase << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x" << std::hex << tableEnd << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = " << std::dec << slotCount << "u;\n";
        ss << "PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[" << std::dec << (slotCount == 0u ? 1u : slotCount) << "u] = {};\n\n";

        ss << "namespace {\n";
        ss << "struct GeneratedFunctionTableInitializer {\n";
        ss << "    GeneratedFunctionTableInitializer() {\n";
        for (const auto &[address, name] : entries)
        {
            const uint32_t slot = (address - tableBase) >> 2;
            ss << "        g_ps2RecompiledFunctionTable[" << std::dec << slot << "] = " << name
               << "; // 0x" << std::hex << address << std::dec << "\n";
        }
        ss << "    }\n";
        ss << "};\n";
        ss << "static const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n";
        ss << "}\n";

        return ss.str();
    }
    void FunctionTableEmitter::emitOverlayTable(std::stringstream &ss,
                                                const std::vector<std::pair<uint32_t, std::string>> &entries,
                                                uint32_t tableBase,
                                                uint32_t tableEnd,
                                                uint32_t slotCount)
    {
        CodeGenerator &cg = m_codeGenerator;
        const std::string &name = cg.m_overlayName;

        // Fingerprint: 8-byte code words from the middle of functions spread across
        // the image. Overlays often share an identical library prefix, so sampling
        // many functions keeps them distinguishable, and code is never rewritten.
        auto readCode = [&](uint32_t address, uint64_t &value) -> bool
        {
            for (const auto &section : cg.m_sections)
            {
                if (!section.isCode || section.data == nullptr)
                    continue;
                if (address < section.address || address + 8u > section.address + section.size)
                    continue;
                std::memcpy(&value, section.data + (address - section.address), sizeof(value));
                return true;
            }
            return false;
        };

        std::vector<std::pair<uint32_t, uint64_t>> samples;
        constexpr size_t kMaxSamples = 16;
        const size_t stride = std::max<size_t>(1, entries.size() / kMaxSamples);
        for (size_t i = 0; i < entries.size() && samples.size() < kMaxSamples; i += stride)
        {
            // Skip the prologue, which is nearly identical across functions.
            const uint32_t address = (entries[i].first + 16u) & ~7u;
            uint64_t value = 0;
            if (readCode(address, value) && value != 0)
                samples.emplace_back(address, value);
        }
        if (samples.empty())
        {
            throw std::runtime_error("Overlay " + name + ": no code words available for fingerprint");
        }

        ss << "// Overlay function table for " << name << "\n";
        ss << "namespace {\n";
        ss << "PS2Runtime::RecompiledFunction s_overlayTable[" << std::dec << (slotCount == 0u ? 1u : slotCount) << "u] = {};\n";
        ss << "const uint32_t s_sampleAddresses[] = {";
        for (const auto &[address, value] : samples)
            ss << " 0x" << std::hex << address << "u,";
        ss << " };\n";
        ss << "const uint64_t s_sampleValues[] = {";
        for (const auto &[address, value] : samples)
            ss << " 0x" << std::hex << value << "ull,";
        ss << " };\n";
        ss << "const PS2OverlayTable s_overlay = { \"" << name << "\", 0x" << std::hex << tableBase << "u, 0x" << tableEnd
           << "u, " << std::dec << slotCount << "u, s_overlayTable, s_sampleAddresses, s_sampleValues, " << samples.size() << "u };\n\n";
        ss << "struct OverlayTableInitializer {\n";
        ss << "    OverlayTableInitializer() {\n";
        for (const auto &[address, fnName] : entries)
        {
            const uint32_t slot = (address - tableBase) >> 2;
            ss << "        s_overlayTable[" << std::dec << slot << "] = " << fnName
               << "; // 0x" << std::hex << address << std::dec << "\n";
        }
        ss << "        ps2RegisterOverlayTable(&s_overlay);\n";
        ss << "    }\n";
        ss << "};\n";
        ss << "static const OverlayTableInitializer g_overlayTableInitializer;\n";
        ss << "}\n";
    }
}
