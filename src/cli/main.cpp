#include "utils.hpp"

#include <Zydis/Disassembler.h>
#include <fstream>
#include <map>
#include <random>
#include <ranges>
#include <set>
#include <sfl/small_flat_map.hpp>
#include <sfl/small_flat_set.hpp>
#include <sfl/small_vector.hpp>
#include <sfl/static_vector.hpp>
#include <sfl/vector.hpp>
#include <x86Tester/execution.hpp>
#include <x86Tester/generator.hpp>
#include <x86Tester/inputgenerator.hpp>
#include <x86Tester/logging.hpp>

using namespace x86Tester;

static constexpr auto kAbortTestCaseThreshold = 100'000;
static constexpr auto kReportInputsThreshold = kAbortTestCaseThreshold * 80 / 100;

enum class ExceptionType
{
    None,
    // #DE
    DivideError,
    IntegerOverflow,
};

struct TestBitInfo
{
    ExceptionType exceptionType;
    ZydisRegister reg;
    std::uint16_t bitPos;
    std::uint8_t expectedBitValue;
};

using RegTestData = sfl::small_vector<std::uint8_t, 8>;

struct TestCaseEntry
{
    sfl::small_flat_map<ZydisRegister, RegTestData, 2> inputRegs;
    std::optional<std::uint32_t> inputFlags;
    sfl::small_flat_map<ZydisRegister, RegTestData, 2> outputRegs;
    std::optional<std::uint32_t> outputFlags;
    std::optional<ExceptionType> exceptionType;

    bool operator==(const TestCaseEntry& other) const
    {
        return inputRegs == other.inputRegs && inputFlags == other.inputFlags && outputRegs == other.outputRegs
            && outputFlags == other.outputFlags && exceptionType == other.exceptionType;
    }

    bool operator<(const TestCaseEntry& other) const
    {
        return std::tie(inputRegs, inputFlags, outputRegs, outputFlags, exceptionType)
            < std::tie(other.inputRegs, other.inputFlags, other.outputRegs, other.outputFlags, other.exceptionType);
    }
};

struct InstrTestGroup
{
    std::uint64_t address;
    std::span<const uint8_t> instrData;
    std::vector<TestCaseEntry> entries;
};

static bool isRegFiltered(ZydisRegister reg)
{
    switch (reg)
    {
        case ZYDIS_REGISTER_NONE:
        case ZYDIS_REGISTER_EIP:
        case ZYDIS_REGISTER_RIP:
        case ZYDIS_REGISTER_FLAGS:
        case ZYDIS_REGISTER_EFLAGS:
        case ZYDIS_REGISTER_RFLAGS:
            return true;
    }
    return false;
}

static sfl::small_vector<ExceptionType, 5> getExceptions(const ZydisDisassembledInstruction& instr)
{
    sfl::small_vector<ExceptionType, 5> res;

    switch (instr.info.mnemonic)
    {
        case ZYDIS_MNEMONIC_DIV:
            // #DE
            res.push_back(ExceptionType::DivideError);
            res.push_back(ExceptionType::IntegerOverflow);
            break;
    }

    return res;
}

static sfl::static_vector<ZydisRegister, 5> sortRegs(const sfl::small_flat_set<ZydisRegister, 5>& regs)
{
    sfl::static_vector<ZydisRegister, 5> res(regs.begin(), regs.end());
    std::sort(res.begin(), res.end(), [](auto a, auto b) {
        return ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, a) > ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, b);
    });
    return res;
}

static sfl::static_vector<ZydisRegister, 5> getRegsModified(const ZydisDisassembledInstruction& instr)
{
    sfl::small_flat_set<ZydisRegister, 5> regs;
    for (std::size_t i = 0; i < instr.info.operand_count; ++i)
    {
        const auto& op = instr.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0)
        {
            if (!isRegFiltered(op.reg.value))
                regs.insert(op.reg.value);
        }
    }
    return sortRegs(regs);
}

static sfl::static_vector<ZydisRegister, 5> getRegsRead(const ZydisDisassembledInstruction& instr)
{
    sfl::small_flat_set<ZydisRegister, 5> regs;
    for (std::size_t i = 0; i < instr.info.operand_count; ++i)
    {
        const auto& op = instr.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && (op.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0)
        {
            regs.insert(op.reg.value);
        }
        else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            if (op.mem.base != ZYDIS_REGISTER_NONE && !isRegFiltered(op.mem.base))
                regs.insert(op.mem.base);
            if (op.mem.index != ZYDIS_REGISTER_NONE && !isRegFiltered(op.mem.index))
                regs.insert(op.mem.index);
        }
    }

    sfl::small_flat_map<ZydisRegister, ZydisRegister, 5> regMap;
    // Some registers may overlap, we have to turn them into a single register with largest size encountered.
    for (const auto& reg : regs)
    {
        const auto bigReg = ZydisRegisterGetLargestEnclosing(instr.info.machine_mode, reg);
        if (auto it = regMap.find(bigReg); it != regMap.end())
        {
            if (ZydisRegisterGetWidth(instr.info.machine_mode, reg)
                > ZydisRegisterGetWidth(instr.info.machine_mode, it->second))
            {
                // Pick bigger.
                it->second = reg;
            }
        }
        else
            regMap[bigReg] = reg;
    }

    regs.clear();
    for (const auto& [bigReg, reg] : regMap)
    {
        regs.insert(reg);
    }

    return sortRegs(regs);
}

static sfl::static_vector<ZydisRegister, 5> getRegsUsed(const ZydisDisassembledInstruction& instr)
{
    sfl::small_flat_set<ZydisRegister, 5> regs;
    for (std::size_t i = 0; i < instr.info.operand_count; ++i)
    {
        const auto& op = instr.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER)
        {
            if (!isRegFiltered(op.reg.value))
                regs.insert(op.reg.value);
        }
        else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            if (op.mem.base != ZYDIS_REGISTER_NONE && !isRegFiltered(op.mem.base))
                regs.insert(op.mem.base);
            if (op.mem.index != ZYDIS_REGISTER_NONE && !isRegFiltered(op.mem.index))
                regs.insert(op.mem.index);
        }
    }
    return sortRegs(regs);
}

static std::uint32_t getFlagsModified(const ZydisDisassembledInstruction& instr)
{
    std::uint32_t flags = 0;
    flags |= instr.info.cpu_flags->modified;
    return flags;
}

static std::uint32_t getFlagsSet0(const ZydisDisassembledInstruction& instr)
{
    std::uint32_t flags = 0;
    flags |= instr.info.cpu_flags->set_0;
    return flags;
}

static std::uint32_t getFlagsSet1(const ZydisDisassembledInstruction& instr)
{
    std::uint32_t flags = 0;
    flags |= instr.info.cpu_flags->set_1;
    return flags;
}

static std::uint32_t getFlagsRead(const ZydisDisassembledInstruction& instr)
{
    std::uint32_t flags = 0;
    flags |= instr.info.cpu_flags->tested;
    return flags;
}

static std::vector<TestBitInfo> generateTestMatrix(const ZydisDisassembledInstruction& instr)
{
    const auto regsRead = getRegsRead(instr);
    const auto regsModified = getRegsModified(instr);
    const auto flagsModified = getFlagsModified(instr);
    const auto flagsSet1 = getFlagsSet1(instr);
    const auto flagsSet0 = getFlagsSet0(instr);

    std::vector<TestBitInfo> matrix;

    bool regDestAndSrcSame = false;
    const auto& ops = instr.operands;
    if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
    {
        if (ops[0].reg.value == ops[1].reg.value)
            regDestAndSrcSame = true;
    }

    bool testRegZero = true;
    bool testRegOne = true;
    bool rightInputZero = false;
    bool resultAlwaysZero = false;
    bool firstBitAlwaysZero = false;
    bool inputIsImmediate = false;

    if (ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
    {
        inputIsImmediate = true;
        if (ops[1].imm.value.s == 0)
        {
            rightInputZero = true;
        }
    }

    // Enhanced semantic checks for specific instructions
    switch (instr.info.mnemonic)
    {
        case ZYDIS_MNEMONIC_SUB:
        case ZYDIS_MNEMONIC_CMP:
        case ZYDIS_MNEMONIC_XOR:
            resultAlwaysZero = regDestAndSrcSame;
            break;
        case ZYDIS_MNEMONIC_AND:
        case ZYDIS_MNEMONIC_TEST:
            resultAlwaysZero = rightInputZero;
            break;
        case ZYDIS_MNEMONIC_ADD:
            firstBitAlwaysZero = regDestAndSrcSame;
            break;
    }

    // Generate test matrix for registers
    for (auto& regModified : regsModified)
    {
        const auto regSize = ZydisRegisterGetWidth(instr.info.machine_mode, regModified);
        for (std::uint16_t bitPos = 0; bitPos < regSize; ++bitPos)
        {
            bool testZero = testRegZero;
            bool testOne = !resultAlwaysZero;

            // Expect 0 if possible.
            if (testZero)
            {
                matrix.push_back({ ExceptionType::None, regModified, bitPos, 0 });
            }

            if (bitPos == 0 && firstBitAlwaysZero)
                testOne = false;

            // Expect 1 if possible.
            if (testOne)
            {
                matrix.push_back({ ExceptionType::None, regModified, bitPos, 1 });
            }
        }
    }

    // Generate test matrix for flags
    for (std::size_t i = 0; i < 32; ++i)
    {
        const auto flag = 1U << i;

        if (!inputIsImmediate)
        {
            if ((flagsModified & flag) != 0)
            {
                bool testFlagZero = true;
                bool testFlagOne = true;

                // Additional checks for specific flags based on instruction type.
                if (flag == ZYDIS_CPUFLAG_ZF)
                {
                    testFlagZero = !resultAlwaysZero;
                }
                if (flag == ZYDIS_CPUFLAG_CF)
                {
                    testFlagOne = !resultAlwaysZero && !rightInputZero;
                }
                if (flag == ZYDIS_CPUFLAG_OF)
                {
                    testFlagOne = !regDestAndSrcSame && !rightInputZero;
                }
                if (flag == ZYDIS_CPUFLAG_PF)
                {
                    testFlagZero = !resultAlwaysZero;
                }
                if (flag == ZYDIS_CPUFLAG_AF)
                {
                    testFlagOne = !resultAlwaysZero && !rightInputZero;
                }
                if (flag == ZYDIS_CPUFLAG_SF)
                {
                    testFlagOne = !resultAlwaysZero;
                }

                // Expect 0 if possible.
                if (testFlagZero)
                {
                    matrix.push_back({ ExceptionType::None, ZYDIS_REGISTER_FLAGS, static_cast<std::uint16_t>(i), 0 });
                }

                // Expect 1 if possible.
                if (testFlagOne)
                {
                    matrix.push_back({ ExceptionType::None, ZYDIS_REGISTER_FLAGS, static_cast<std::uint16_t>(i), 1 });
                }
            }
        }

        if ((flagsSet0 & flag) != 0)
        {
            matrix.push_back({ ExceptionType::None, ZYDIS_REGISTER_FLAGS, static_cast<std::uint16_t>(i), 0 });
        }

        if ((flagsSet1 & flag) != 0)
        {
            matrix.push_back({ ExceptionType::None, ZYDIS_REGISTER_FLAGS, static_cast<std::uint16_t>(i), 1 });
        }
    }

    // Generate test matrix for exceptions.
    const auto exceptions = getExceptions(instr);
    for (const auto& exception : exceptions)
    {
        matrix.push_back({ exception, ZYDIS_REGISTER_NONE, 0, 0 });
    }

    return matrix;
}

static std::size_t getRegOffset(ZydisRegister reg)
{
    switch (reg)
    {
        case ZYDIS_REGISTER_AH:
        case ZYDIS_REGISTER_BH:
        case ZYDIS_REGISTER_CH:
        case ZYDIS_REGISTER_DH:
            return 1;
    }
    return 0;
}

static void advanceInputs(
    Execution::ScopedContext& ctx, std::mt19937_64& prng, std::vector<Generator::InputGenerator>& inputGens,
    const ZydisDisassembledInstruction& instr, TestCaseEntry& testEntry, std::size_t iteration)
{
    const auto regsRead = getRegsRead(instr);
    const auto flagsRead = getFlagsRead(instr);

    sfl::small_flat_set<ZydisRegister, 5> regsReadBig;
    for (const auto& reg : regsRead)
    {
        const auto bigReg = ZydisRegisterGetLargestEnclosing(instr.info.machine_mode, reg);
        regsReadBig.insert(bigReg);
    }

    // Cleanse the registers.
    for (const auto& reg : regsReadBig)
    {
        if (isRegFiltered(reg))
            continue;

        const auto bigRegSize = static_cast<size_t>(ZydisRegisterGetWidth(instr.info.machine_mode, reg) / 8);
        const uint8_t ccBytes[] = {
            0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
        };
        ctx.setRegBytes(reg, std::span<const std::uint8_t>{ ccBytes, bigRegSize });
    }

#ifdef _DEBUG
    sfl::static_vector<std::pair<ZydisRegister, std::vector<std::uint8_t>>, 5> regData;
#endif

    // Randomize read registers.
    size_t regIndex = 0;
    for (const auto& reg : regsRead)
    {
        if (isRegFiltered(reg))
            continue;

        std::uint8_t regBuf[256]{};
        const std::size_t usedRegBitSize = ZydisRegisterGetWidth(instr.info.machine_mode, reg);
        const std::size_t usedRegByteSize = usedRegBitSize / 8;
        const auto bigReg = ZydisRegisterGetLargestEnclosing(instr.info.machine_mode, reg);
        const std::size_t bigRegBitSize = ZydisRegisterGetWidth(instr.info.machine_mode, bigReg);
        const std::size_t bigRegByteSize = bigRegBitSize / 8;

        // In case inputs are ah, al we need to re-use the existing data in the root register.
        auto currentBytes = ctx.getRegBytes(bigReg);
        std::memcpy(regBuf, currentBytes.data(), currentBytes.size());
        const auto regOffset = getRegOffset(reg);

        auto& inputGen = inputGens[regIndex];

        const auto inputData = inputGen.current();
        std::memcpy(regBuf + regOffset, inputData.data(), usedRegByteSize);

        ctx.setRegBytes(bigReg, std::span<const std::uint8_t>{ regBuf, bigRegByteSize });

        testEntry.inputRegs[bigReg] = RegTestData{ regBuf, regBuf + bigRegByteSize };

        regIndex++;

#ifdef _DEBUG
        if (iteration >= kReportInputsThreshold)
        {
            regData.emplace_back(reg, std::vector<std::uint8_t>{ regBuf + regOffset, regBuf + regOffset + usedRegByteSize });
        }
#endif
    }

    for (size_t inputIdx = 0; inputIdx < regIndex; ++inputIdx)
    {
        if (inputGens[inputIdx].advance())
        {
            if ((iteration + 1) % 3 == 0)
                break;
        }
    }

    // Randomize read flags.
    std::uint32_t flags = 0;
    if (flagsRead != 0)
    {
        for (std::size_t i = 0; i < 32; ++i)
        {
            if ((flagsRead & (1 << i)) != 0)
            {
                flags |= (prng() % 2) << i;
            }
        }

        testEntry.inputFlags = flags;
    }

    // Ensure we never have TF set.
    flags &= ~ZYDIS_CPUFLAG_TF;

    ctx.setRegValue(ZYDIS_REGISTER_RFLAGS, flags);

#ifdef _DEBUG
    if (iteration >= kReportInputsThreshold)
    {
        std::string inputsStr;
        for (const auto& [reg, data] : regData)
        {
            inputsStr += std::format("{}=#{} ", ZydisRegisterGetString(reg), Utils::toHexString(data));
        }

        Logging::println("Test: {} - Inputs: {}", instr.text, inputsStr);
    }
#endif
}

static void clearOutput(Execution::ScopedContext& ctx, const TestBitInfo& testBitInfo)
{
    uint8_t regBuf[256]{};

    if (!isRegFiltered(testBitInfo.reg))
    {
        // FIXME: Pass the correct mode.
        const std::size_t regSize = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, testBitInfo.reg) / 8;
        const auto regOffset = getRegOffset(testBitInfo.reg);

        for (std::size_t i = 0; i < regSize; ++i)
        {
            if (testBitInfo.expectedBitValue == 0)
                regBuf[i + regOffset] = 0xFF;
            else
                regBuf[i + regOffset] = 0;
        }

        const auto bigReg = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, testBitInfo.reg);
        const std::size_t bigRegSize = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, bigReg) / 8;

        ctx.setRegBytes(bigReg, std::span<const std::uint8_t>{ regBuf, bigRegSize });
    }

    // Clear flags.
    std::uint32_t flags = 0;
    if (testBitInfo.expectedBitValue == 0)
    {
        flags |= ZYDIS_CPUFLAG_CF | ZYDIS_CPUFLAG_PF | ZYDIS_CPUFLAG_AF | ZYDIS_CPUFLAG_ZF | ZYDIS_CPUFLAG_SF
            | ZYDIS_CPUFLAG_OF;
    }
    else
    {
        flags = 0;
    }
    ctx.setRegValue(ZYDIS_REGISTER_RFLAGS, flags);
}

static bool checkOutputs(
    Execution::ScopedContext& ctx, const ZydisDisassembledInstruction& instr, const TestBitInfo& testBitInfo,
    TestCaseEntry& testEntry)
{
    const auto bigReg = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, testBitInfo.reg);

    const auto regData = ctx.getRegBytes(bigReg);
    const auto regOffset = getRegOffset(testBitInfo.reg);

    const auto bitValue = (regData[regOffset + (testBitInfo.bitPos / 8)] >> (testBitInfo.bitPos % 8)) & 1;

    if (bitValue != testBitInfo.expectedBitValue)
    {
        return false;
    }

    // Capture output.
    auto regsModified = getRegsModified(instr);
    auto flagsModified = getFlagsModified(instr);

    for (auto regModified : regsModified)
    {
        const auto bigReg = ZydisRegisterGetLargestEnclosing(instr.info.machine_mode, regModified);
        const auto bigSize = ZydisRegisterGetWidth(instr.info.machine_mode, bigReg);

        const auto regData = ctx.getRegBytes(bigReg);

        testEntry.outputRegs[bigReg] = RegTestData{ regData.begin(), regData.begin() + (bigSize / 8) };
    }

    if (getFlagsModified(instr) != 0)
    {
        testEntry.outputFlags = ctx.getRegValue<uint32_t>(ZYDIS_REGISTER_RFLAGS);
        // Remove certain flags that are forced.
        *testEntry.outputFlags &= ~ZYDIS_CPUFLAG_IF;
    }

    return true;
}

static std::string getTestInfo(const TestBitInfo& info)
{
    std::string res;
    res = std::format("{}[{}] = 0b{}", ZydisRegisterGetString(info.reg), info.bitPos, info.expectedBitValue);
    return res;
}

static std::vector<Generator::InputGenerator> setupInputGenerators(
    std::mt19937_64& prng, const ZydisDisassembledInstruction& instr)
{
    std::vector<Generator::InputGenerator> generators;

    const auto regsRead = getRegsRead(instr);

    // Generate input generators for registers.
    for (const auto& reg : regsRead)
    {
        if (isRegFiltered(reg))
            continue;

        const auto regSize = ZydisRegisterGetWidth(instr.info.machine_mode, reg);
        generators.emplace_back(regSize, prng);
    }

    return generators;
}

static void testInstruction(ZydisMachineMode mode, InstrTestGroup& testCase)
{
    auto& instrData = testCase.instrData;

    ZydisDisassembledInstruction instr{};
    ZydisDisassembleIntel(mode, 0, instrData.data(), instrData.size(), &instr);

    const auto regsRead = getRegsRead(instr);
    const auto flagsRead = getFlagsRead(instr);

    // TODO: Create a matrix to test all possible bits of registers and flags.
    const auto testMatrix = generateTestMatrix(instr);

    const auto timeStart = std::chrono::high_resolution_clock::now();

    auto ctx = Execution::ScopedContext(mode, instrData);
    if (!ctx)
    {
        Logging::println("Failed to prepare context");
        return;
    }

    testCase.address = ctx.getCodeAddress();

    const auto seed = static_cast<std::size_t>(instr.info.mnemonic);
    std::mt19937_64 prng(seed);

    for (const TestBitInfo& testBitInfo : testMatrix)
    {
        TestCaseEntry testEntry{};

        auto inputGenerators = setupInputGenerators(prng, instr);

        bool hasExpected = false;
        std::size_t iteration = 0;
        // Repeat this until expected bit is set.
        while (!hasExpected)
        {
            // Ensure the output has the opposite value.
            clearOutput(ctx, testBitInfo);

            // Assign inputs.
            // if (iteration % 2 == 0)
            advanceInputs(ctx, prng, inputGenerators, instr, testEntry, iteration);
            // else
            // randomizeInputs(ctx, prng, instr, testEntry, iteration);

            if (!ctx.execute())
            {
                Logging::println("Failed to execute instruction");
                return;
            }

            ExceptionType exceptionType = ExceptionType::None;
            if (auto status = ctx.getExecutionStatus(); status != Execution::ExecutionStatus::Success)
            {
                switch (status)
                {
                    case Execution::ExecutionStatus::ExceptionIntDivideError:
                        exceptionType = ExceptionType::DivideError;
                        break;
                    case Execution::ExecutionStatus::ExceptionIntOverflow:
                        exceptionType = ExceptionType::IntegerOverflow;
                        break;
                }
                if (exceptionType != testBitInfo.exceptionType)
                {
                    // Unexpected exception, ignore.
                    hasExpected = false;
                }
                else
                {
                    testEntry.exceptionType = exceptionType;
                    hasExpected = true;
                }
            }
            else
            {
                // If we expect an exception we don't care about the output.
                if (testBitInfo.exceptionType == ExceptionType::None)
                {
                    hasExpected = checkOutputs(ctx, instr, testBitInfo, testEntry);
                }
            }

            iteration++;

            if (iteration > kAbortTestCaseThreshold)
            {
                // Probably impossible.
                Logging::println("Test probably impossible: {} ; {}", instr.text, getTestInfo(testBitInfo));
                break;
            }
        }

        if (hasExpected)
        {
            testCase.entries.push_back(std::move(testEntry));
        }
    }

    const auto timeEnd = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(timeEnd - timeStart).count();

    // std::print("Completed testing of instruction: {}, matrix size: {}, elapsed in: {} ms.\n", instr.text,
    // testMatrix.size(), elapsed);
}

static InstrTestGroup generateInstructionTestData(ZydisMachineMode mode, const std::span<const uint8_t> instrData)
{
    InstrTestGroup testCase;
    testCase.instrData = instrData;

    testInstruction(mode, testCase);

    // Remove duplicate entries.
    std::sort(testCase.entries.begin(), testCase.entries.end());

    auto last = std::unique(testCase.entries.begin(), testCase.entries.end());
    testCase.entries.erase(last, testCase.entries.end());

    return testCase;
}

static ZydisDisassembledInstruction disassembleInstruction(const std::span<const std::uint8_t> instrData, std::uint64_t address)
{
    ZydisDisassembledInstruction instr{};
    ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, address, instrData.data(), instrData.size(), &instr);
    return instr;
}

static bool serializeTestEntries(ZydisMnemonic mnemonic, const std::vector<InstrTestGroup>& entries)
{
    std::ofstream file(ZydisMnemonicGetString(mnemonic) + std::string(".txt"));
    if (!file)
    {
        std::print("Failed to open file for writing\n");
        return false;
    }

    const auto getExceptionString = [](ExceptionType exception) -> std::string {
        switch (exception)
        {
            case ExceptionType::None:
                return "NONE";
            case ExceptionType::DivideError:
                return "INT_DIVIDE_ERROR";
            case ExceptionType::IntegerOverflow:
                return "INT_OVERFLOW";
        }
        return "<ERROR>";
    };

    for (const auto& entry : entries)
    {
        const auto instr = disassembleInstruction(entry.instrData, entry.address);

        std::print(
            file, "instr:0x{:X};#{};{};{}\n", entry.address, Utils::toHexString(entry.instrData), instr.text,
            entry.entries.size());
        for (const auto& entry : entry.entries)
        {
            std::print(file, " in:");
            auto numIn = 0;
            for (const auto& [reg, data] : entry.inputRegs)
            {
                std::print(
                    file, "{}{};#{}", numIn > 0 ? ";" : "", ZydisRegisterGetString(reg),
                    Utils::toHexString({ data.data(), data.size() }));
                numIn++;
            }
            if (entry.inputFlags)
                std::print(file, "{}{};0x{:08X}", numIn > 0 ? ";" : "", "flags", *entry.inputFlags);

            std::print(file, "{}out:", numIn > 0 ? "|" : "");
            auto numOut = 0;
            for (const auto& [reg, data] : entry.outputRegs)
            {
                std::print(
                    file, "{}{};#{}", numOut > 0 ? ";" : "", ZydisRegisterGetString(reg),
                    Utils::toHexString({ data.data(), data.size() }));
                numOut++;
            }
            if (entry.outputFlags)
                std::print(file, "{}{};0x{:08X}", numOut > 0 ? ";" : "", "flags", *entry.outputFlags);

            if (entry.exceptionType)
            {
                std::print(file, "|{};{}", "exception", getExceptionString(*entry.exceptionType));
            }

            std::print(file, "\n");
        }
    }

    return true;
}

int main()
{
    const auto mode = ZydisMachineMode::ZYDIS_MACHINE_MODE_LONG_64;

    const auto filter = Generator::Filter{}.addMnemonics(ZydisMnemonic::ZYDIS_MNEMONIC_TEST);

    Logging::startProgress("Building instructions");

    const auto instrs = Generator::buildInstructions(
        ZYDIS_MACHINE_MODE_LONG_64, filter, true, [](auto curVal, auto maxVal) { Logging::updateProgress(curVal, maxVal); });

    Logging::endProgress();

    const auto numInstrs = instrs.entryOffsets.size();
    Logging::println("Total instructions: {}", numInstrs);

    Logging::startProgress("Generating tests");

    std::vector<InstrTestGroup> testGroups;
    std::mutex mtx;
    std::atomic<size_t> curInstr = 0;

    instrs.forEachParallel([&](auto&& instrData) {
        //
        InstrTestGroup testCase = generateInstructionTestData(mode, instrData);
        if (!testCase.entries.empty())
        {
            std::lock_guard lock(mtx);
            testGroups.push_back(std::move(testCase));
        }
        Logging::updateProgress(++curInstr, numInstrs);
    });

    Logging::endProgress();

    // Sort the groups by instruction string.
    std::sort(testGroups.begin(), testGroups.end(), [](const auto& a, const auto& b) {
        const auto instA = disassembleInstruction(a.instrData, a.address);
        const auto instB = disassembleInstruction(b.instrData, b.address);
        return std::strcmp(instA.text, instB.text) < 0;
    });

    // Group the test cases by instruction.
    std::map<ZydisMnemonic, std::vector<InstrTestGroup>> testGroupsMap;
    for (auto& testGroup : testGroups)
    {
        const auto instr = disassembleInstruction(testGroup.instrData, testGroup.address);

        auto it = testGroupsMap.find(instr.info.mnemonic);
        if (it != testGroupsMap.end())
        {
            it->second.push_back(std::move(testGroup));
            continue;
        }
        else
        {
            testGroupsMap.insert({ instr.info.mnemonic, { std::move(testGroup) } });
        }
    }

    // Report results.
    size_t totalTestEntries = 0;
    for (const auto& [mnemonic, testGroups] : testGroupsMap)
    {
        for (auto& testGroup : testGroups)
        {
            totalTestEntries += testGroup.entries.size();
        }
    }
    Logging::println("Total test cases: {}", totalTestEntries);

    // Save to file.
    for (const auto& [mnemonic, testGroups] : testGroupsMap)
    {
        serializeTestEntries(mnemonic, { testGroups });
    }

    return EXIT_SUCCESS;
}