#include "utils.hpp"

#include <Zydis/Disassembler.h>
#include <filesystem>
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
    std::uint64_t address{};
    std::span<const uint8_t> instrData;
    std::vector<TestCaseEntry> entries;
    bool illegalInstruction{};
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

static ZydisRegister getRootReg(ZydisMachineMode mode, ZydisRegister reg)
{
    const auto regCls = ZydisRegisterGetClass(reg);
    switch (regCls)
    {
        // General purpose registers
        case ZYDIS_REGCLASS_GPR8:
        case ZYDIS_REGCLASS_GPR16:
        case ZYDIS_REGCLASS_GPR32:
        case ZYDIS_REGCLASS_GPR64:
        case ZYDIS_REGCLASS_FLAGS:
            return ZydisRegisterGetLargestEnclosing(mode, reg);
    }
    return reg;
}

static sfl::static_vector<ZydisRegister, 5> getRegsRead(const ZydisDisassembledInstruction& instr)
{
    sfl::small_flat_set<ZydisRegister, 5> regs;
    for (std::size_t i = 0; i < instr.info.operand_count; ++i)
    {
        const auto& op = instr.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER)
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

    // Mark registers that are smaller than 32 bit also as read, the upper bits remain unaffected.
    for (std::size_t i = 0; i < instr.info.operand_count; ++i)
    {
        const auto& op = instr.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER)
        {
            const auto regCls = ZydisRegisterGetClass(op.reg.value);
            if (regCls == ZydisRegisterClass::ZYDIS_REGCLASS_GPR16 || regCls == ZydisRegisterClass::ZYDIS_REGCLASS_GPR8)
            {
                regs.insert(op.reg.value);
            }
        }
    }

    const auto remapReg = [](auto& oldReg) {
        if (oldReg == ZYDIS_REGISTER_AH)
            return ZYDIS_REGISTER_AX;
        if (oldReg == ZYDIS_REGISTER_BH)
            return ZYDIS_REGISTER_BX;
        if (oldReg == ZYDIS_REGISTER_CH)
            return ZYDIS_REGISTER_CX;
        if (oldReg == ZYDIS_REGISTER_DH)
            return ZYDIS_REGISTER_DX;
        return oldReg;
    };

    sfl::small_flat_map<ZydisRegister, ZydisRegister, 5> regMap;
    // Some registers may overlap, we have to turn them into a single register with largest size encountered.
    for (const auto& reg : regs)
    {
        const auto bigReg = getRootReg(instr.info.machine_mode, reg);
        auto newReg = remapReg(reg);
        if (auto it = regMap.find(bigReg); it != regMap.end())
        {
            if (ZydisRegisterGetWidth(instr.info.machine_mode, newReg)
                > ZydisRegisterGetWidth(instr.info.machine_mode, it->second))
            {
                // Pick bigger.
                it->second = newReg;
            }
        }
        else
            regMap[bigReg] = newReg;
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
    size_t numBitsZero = 0;

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
        case ZYDIS_MNEMONIC_FADD:
            firstBitAlwaysZero = regDestAndSrcSame;
            break;
        case ZYDIS_MNEMONIC_MOV:
            resultAlwaysZero = rightInputZero;
            break;
        case ZYDIS_MNEMONIC_LEA:
            // If mem is [rax+rax*1] then the first bit is always zero.
            firstBitAlwaysZero = ops[1].mem.base != ZYDIS_REGISTER_NONE && ops[1].mem.index == ops[1].mem.base
                && ops[1].mem.disp.value == 0;
            if (ops[1].mem.base == ZYDIS_REGISTER_NONE && ops[1].mem.index != ZYDIS_REGISTER_NONE && ops[1].mem.scale > 1
                && ops[1].mem.disp.value == 0)
            {
                // The first bits are always zero based on scale.
                // Turn multiply into shift
                const auto shift = static_cast<std::uint8_t>(std::log2(ops[1].mem.scale));
                numBitsZero = shift;
            }
            break;
    }

    // Generate test matrix for registers
    for (auto& regModified : regsModified)
    {
        const auto regSize = ZydisRegisterGetWidth(instr.info.machine_mode, regModified);

        auto maxBits = regSize;
        switch (instr.info.mnemonic)
        {
            case ZYDIS_MNEMONIC_SETB:
            case ZYDIS_MNEMONIC_SETBE:
            case ZYDIS_MNEMONIC_SETL:
            case ZYDIS_MNEMONIC_SETLE:
            case ZYDIS_MNEMONIC_SETNB:
            case ZYDIS_MNEMONIC_SETNBE:
            case ZYDIS_MNEMONIC_SETNL:
            case ZYDIS_MNEMONIC_SETNLE:
            case ZYDIS_MNEMONIC_SETNO:
            case ZYDIS_MNEMONIC_SETNP:
            case ZYDIS_MNEMONIC_SETNS:
            case ZYDIS_MNEMONIC_SETNZ:
            case ZYDIS_MNEMONIC_SETO:
            case ZYDIS_MNEMONIC_SETP:
            case ZYDIS_MNEMONIC_SETS:
            case ZYDIS_MNEMONIC_SETZ:
                maxBits = 1;
                break;
            case ZYDIS_MNEMONIC_LEA:
                maxBits = instr.info.address_width;
                break;
            case ZYDIS_MNEMONIC_BSWAP:
                resultAlwaysZero = regSize <= 16;
                break;
        }

        for (std::uint16_t bitPos = 0; bitPos < regSize; ++bitPos)
        {
            bool testZero = testRegZero;
            bool testOne = bitPos >= numBitsZero && !resultAlwaysZero && bitPos < maxBits;

            if (instr.info.mnemonic == ZYDIS_MNEMONIC_MOV && inputIsImmediate)
            {
                // We know the input value so we will expect those bits.
                testZero = (ops[1].imm.value.u & (1ULL << bitPos)) == 0;
                testOne = (ops[1].imm.value.u & (1ULL << bitPos)) != 0;
            }
            else if (instr.info.mnemonic == ZYDIS_MNEMONIC_OR && inputIsImmediate)
            {
                // If the input bit is not zero then the output bit will never be zero.
                testZero = (ops[1].imm.value.u & (1ULL << bitPos)) == 0;
            }
            else if (instr.info.mnemonic == ZYDIS_MNEMONIC_AND && inputIsImmediate)
            {
                // If the input bit is zero then the output bit will never be one.
                testOne = (ops[1].imm.value.u & (1ULL << bitPos)) != 0;
            }
            else if (instr.info.mnemonic == ZYDIS_MNEMONIC_BTR && inputIsImmediate)
            {
                // BTR is just reg[bit] = 0
                testOne = ((ops[1].imm.value.u % instr.info.operand_width) != bitPos);
            }

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
        const auto bigReg = getRootReg(instr.info.machine_mode, reg);
        regsReadBig.insert(bigReg);
    }

    // Cleanse the registers.
    for (const auto& reg : regsReadBig)
    {
        if (isRegFiltered(reg))
            continue;

        const auto bigRegSize = static_cast<size_t>(ZydisRegisterGetWidth(instr.info.machine_mode, reg) / 8);
        constexpr uint8_t ccBytes[] = {
            0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
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
        const auto bigReg = getRootReg(instr.info.machine_mode, reg);
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

    ctx.setRegValue(ZYDIS_REGISTER_EFLAGS, flags);

#if defined(_DEBU) && 0
    if (iteration >= kReportInputsThreshold)
    {
        std::string inputsStr;
        for (const auto& [reg, data] : regData)
        {
            inputsStr += std::format("{}=#{} ", ZydisRegisterGetString(reg), Utils::hexEncode(data));
        }

        Logging::println("Test: {} - Inputs: {}", instr.text, inputsStr);
    }
#endif
}

static void clearOutput(ZydisMachineMode mode, Execution::ScopedContext& ctx, const TestBitInfo& testBitInfo)
{
    uint8_t regBuf[256]{};

    if (!isRegFiltered(testBitInfo.reg))
    {
        const std::size_t regSize = ZydisRegisterGetWidth(mode, testBitInfo.reg) / 8;
        const auto regOffset = getRegOffset(testBitInfo.reg);

        for (std::size_t i = 0; i < regSize; ++i)
        {
            if (testBitInfo.expectedBitValue == 0)
                regBuf[i + regOffset] = 0xFF;
            else
                regBuf[i + regOffset] = 0;
        }

        const auto bigReg = getRootReg(mode, testBitInfo.reg);
        const std::size_t bigRegSize = ZydisRegisterGetWidth(mode, bigReg) / 8;

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
    ctx.setRegValue(ZYDIS_REGISTER_EFLAGS, flags);
}

static bool checkOutputs(
    ZydisMachineMode mode, Execution::ScopedContext& ctx, const ZydisDisassembledInstruction& instr,
    const TestBitInfo& testBitInfo, TestCaseEntry& testEntry)
{
    const auto bigReg = getRootReg(mode, testBitInfo.reg);

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
        const auto bigReg = getRootReg(instr.info.machine_mode, regModified);
        const auto bigSize = ZydisRegisterGetWidth(instr.info.machine_mode, bigReg);

        const auto regData = ctx.getRegBytes(bigReg);

        testEntry.outputRegs[bigReg] = RegTestData{ regData.begin(), regData.begin() + (bigSize / 8) };
    }

    if (getFlagsModified(instr) != 0)
    {
        testEntry.outputFlags = ctx.getRegValue<uint32_t>(ZYDIS_REGISTER_EFLAGS);
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

static bool isInputFromImmediate(const ZydisDisassembledInstruction& instr)
{
    for (std::size_t i = 0; i < instr.info.operand_count; ++i)
    {
        const auto& op = instr.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            return true;
    }
    return false;
}

static void testInstruction(ZydisMachineMode mode, InstrTestGroup& testCase)
{
    auto& instrData = testCase.instrData;

    ZydisDisassembledInstruction instr{};
    ZydisDisassembleIntel(mode, 0, instrData.data(), instrData.size(), &instr);

    const auto isInputImmediate = isInputFromImmediate(instr);
    const auto maxAttempts = isInputImmediate ? kAbortTestCaseThreshold / 3 : kAbortTestCaseThreshold;

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
        bool illegalInstr = false;

        std::size_t iteration = 0;
        // Repeat this until expected bit is set.
        while (!hasExpected && !illegalInstr)
        {
            // Ensure the output has the opposite value.
            clearOutput(mode, ctx, testBitInfo);

            // Assign inputs.
            advanceInputs(ctx, prng, inputGenerators, instr, testEntry, iteration);

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
                    case Execution::ExecutionStatus::IllegalInstruction:
                        illegalInstr = true;
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
                    hasExpected = checkOutputs(mode, ctx, instr, testBitInfo, testEntry);
                }
            }

            iteration++;

            if (iteration > maxAttempts)
            {
                // Probably impossible.
                Logging::println("Test probably impossible: {} ; {}", instr.text, getTestInfo(testBitInfo));
                break;
            }
        }

        if (illegalInstr)
        {
            Logging::println("Illegal instruction: {}", instr.text);
            testCase.illegalInstruction = true;
            break;
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
    InstrTestGroup testCase{};
    testCase.instrData = instrData;

    testInstruction(mode, testCase);

    // Remove duplicate entries.
    std::sort(testCase.entries.begin(), testCase.entries.end());

    auto last = std::unique(testCase.entries.begin(), testCase.entries.end());
    testCase.entries.erase(last, testCase.entries.end());

    return testCase;
}

static ZydisDisassembledInstruction disassembleInstruction(
    ZydisMachineMode mode, const std::span<const std::uint8_t> instrData, std::uint64_t address)
{
    ZydisDisassembledInstruction instr{};
    ZydisDisassembleIntel(mode, address, instrData.data(), instrData.size(), &instr);
    return instr;
}

static std::filesystem::path getPathForMnemonic(ZydisMnemonic mnemonic)
{
    std::filesystem::path outputPath = "testdata";

    if (!std::filesystem::exists(outputPath))
    {
        if (!std::filesystem::create_directory(outputPath))
        {
            std::print("Failed to create output directory\n");
            std::abort();
        }
    }

    const auto filePath = outputPath / (ZydisMnemonicGetString(mnemonic) + std::string(".txt"));
    return filePath;
}

static bool serializeTestEntries(ZydisMachineMode mode, ZydisMnemonic mnemonic, const std::vector<InstrTestGroup>& entries)
{
    const auto filePath = getPathForMnemonic(mnemonic);

    std::ofstream file(filePath);
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
        const auto instr = disassembleInstruction(mode, entry.instrData, entry.address);

        std::print(
            file, "instr:0x{:X};#{};{};{}\n", entry.address, Utils::hexEncode(entry.instrData), instr.text,
            entry.entries.size());
        for (const auto& entry : entry.entries)
        {
            std::print(file, " in:");
            auto numIn = 0;
            for (const auto& [reg, data] : entry.inputRegs)
            {
                std::print(
                    file, "{}{}:#{}", numIn > 0 ? "," : "", ZydisRegisterGetString(reg),
                    Utils::hexEncode({ data.data(), data.size() }));
                numIn++;
            }

            if (entry.inputFlags)
            {
                std::array<std::uint8_t, 4> flagsHex{};
                std::memcpy(flagsHex.data(), &entry.inputFlags.value(), 4);
                std::print(file, "{}flags:#{}", numIn > 0 ? "," : "", Utils::hexEncode(flagsHex));
            }

            std::print(file, "{}out:", numIn > 0 ? "|" : "");
            auto numOut = 0;
            for (const auto& [reg, data] : entry.outputRegs)
            {
                std::print(
                    file, "{}{}:#{}", numOut > 0 ? "," : "", ZydisRegisterGetString(reg),
                    Utils::hexEncode({ data.data(), data.size() }));
                numOut++;
            }

            if (entry.outputFlags)
            {
                std::array<std::uint8_t, 4> flagsHex{};
                std::memcpy(flagsHex.data(), &entry.outputFlags.value(), 4);
                std::print(file, "{}flags:#{}", numOut > 0 ? "," : "", Utils::hexEncode(flagsHex));
            }

            if (entry.exceptionType)
            {
                std::print(file, "|exception:{}", getExceptionString(*entry.exceptionType));
            }

            std::print(file, "\n");
        }
    }

    return true;
}

static void generateInstrTests(ZydisMachineMode mode, ZydisMnemonic mnemonic)
{
#ifndef _DEBUG
    const auto filePath = getPathForMnemonic(static_cast<ZydisMnemonic>(mnemonic));

    if (std::filesystem::exists(filePath))
    {
        std::print("Skipping \"{}\" as it already exists\n", ZydisMnemonicGetString(static_cast<ZydisMnemonic>(mnemonic)));
        return;
    }
#endif

    const auto filter = Generator::Filter{}.addMnemonics(mnemonic);

    Logging::startProgress(
        "Building \"{}\" instruction combinations", ZydisMnemonicGetString(static_cast<ZydisMnemonic>(mnemonic)));

    const auto instrs = Generator::buildInstructions(
        mode, filter, true, [](auto curVal, auto maxVal) { Logging::updateProgress(curVal, maxVal); });

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
        if (!testCase.entries.empty() && !testCase.illegalInstruction)
        {
            std::lock_guard lock(mtx);
            testGroups.push_back(std::move(testCase));
        }
        Logging::updateProgress(++curInstr, numInstrs);
    });

    Logging::endProgress();

    // Sort the groups by instruction operand width.
    std::sort(testGroups.begin(), testGroups.end(), [mode](const auto& a, const auto& b) {
        const auto instA = disassembleInstruction(mode, a.instrData, a.address);
        const auto instB = disassembleInstruction(mode, b.instrData, b.address);
        return instA.info.operand_width < instB.info.operand_width;
    });

    // Group the test cases by instruction.
    std::map<ZydisMnemonic, std::vector<InstrTestGroup>> testGroupsMap;
    for (auto& testGroup : testGroups)
    {
        const auto instr = disassembleInstruction(mode, testGroup.instrData, testGroup.address);

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
        serializeTestEntries(mode, mnemonic, { testGroups });
    }
}

int main()
{
    const ZydisMnemonic mnemonics[] = {
        ZYDIS_MNEMONIC_AAA,
        ZYDIS_MNEMONIC_AAD,
        ZYDIS_MNEMONIC_AADD,
        ZYDIS_MNEMONIC_AAM,
        ZYDIS_MNEMONIC_AAND,
        ZYDIS_MNEMONIC_AAS,
        ZYDIS_MNEMONIC_ADC,
        ZYDIS_MNEMONIC_ADCX,
        ZYDIS_MNEMONIC_ADD,
        ZYDIS_MNEMONIC_ADDPD,
        ZYDIS_MNEMONIC_ADDPS,
        ZYDIS_MNEMONIC_ADDSD,
        ZYDIS_MNEMONIC_ADDSS,
        ZYDIS_MNEMONIC_ADDSUBPD,
        ZYDIS_MNEMONIC_ADDSUBPS,
        ZYDIS_MNEMONIC_ADOX,
        ZYDIS_MNEMONIC_AESDEC,
        ZYDIS_MNEMONIC_AESDEC128KL,
        ZYDIS_MNEMONIC_AESDEC256KL,
        ZYDIS_MNEMONIC_AESDECLAST,
        ZYDIS_MNEMONIC_AESDECWIDE128KL,
        ZYDIS_MNEMONIC_AESDECWIDE256KL,
        ZYDIS_MNEMONIC_AESENC,
        ZYDIS_MNEMONIC_AESENC128KL,
        ZYDIS_MNEMONIC_AESENC256KL,
        ZYDIS_MNEMONIC_AESENCLAST,
        ZYDIS_MNEMONIC_AESENCWIDE128KL,
        ZYDIS_MNEMONIC_AESENCWIDE256KL,
        ZYDIS_MNEMONIC_AESIMC,
        ZYDIS_MNEMONIC_AESKEYGENASSIST,
        ZYDIS_MNEMONIC_AND,
        ZYDIS_MNEMONIC_AOR,
        ZYDIS_MNEMONIC_ARPL,
        ZYDIS_MNEMONIC_AXOR,
        ZYDIS_MNEMONIC_BLCFILL,
        ZYDIS_MNEMONIC_BLCI,
        ZYDIS_MNEMONIC_BLCIC,
        ZYDIS_MNEMONIC_BLCMSK,
        ZYDIS_MNEMONIC_BLCS,
        ZYDIS_MNEMONIC_BLENDPD,
        ZYDIS_MNEMONIC_BLENDPS,
        ZYDIS_MNEMONIC_BLENDVPD,
        ZYDIS_MNEMONIC_BLENDVPS,
        ZYDIS_MNEMONIC_BLSFILL,
        ZYDIS_MNEMONIC_BLSI,
        ZYDIS_MNEMONIC_BLSIC,
        ZYDIS_MNEMONIC_BLSMSK,
        ZYDIS_MNEMONIC_BLSR,
        ZYDIS_MNEMONIC_BNDCL,
        ZYDIS_MNEMONIC_BNDCN,
        ZYDIS_MNEMONIC_BNDCU,
        ZYDIS_MNEMONIC_BNDLDX,
        ZYDIS_MNEMONIC_BNDMK,
        ZYDIS_MNEMONIC_BNDMOV,
        ZYDIS_MNEMONIC_BNDSTX,
        ZYDIS_MNEMONIC_BOUND,
        ZYDIS_MNEMONIC_BSWAP,
        ZYDIS_MNEMONIC_BT,
        ZYDIS_MNEMONIC_BTC,
        ZYDIS_MNEMONIC_BTR,
        ZYDIS_MNEMONIC_BTS,
        ZYDIS_MNEMONIC_BZHI,
        ZYDIS_MNEMONIC_CBW,
        ZYDIS_MNEMONIC_CDQ,
        ZYDIS_MNEMONIC_CDQE,
        ZYDIS_MNEMONIC_CLAC,
        ZYDIS_MNEMONIC_CLC,
        ZYDIS_MNEMONIC_CLD,
        ZYDIS_MNEMONIC_CLDEMOTE,
        ZYDIS_MNEMONIC_CLEVICT0,
        ZYDIS_MNEMONIC_CLEVICT1,
        ZYDIS_MNEMONIC_CLFLUSH,
        ZYDIS_MNEMONIC_CLFLUSHOPT,
        ZYDIS_MNEMONIC_CLGI,
        ZYDIS_MNEMONIC_CLI,
        ZYDIS_MNEMONIC_CLRSSBSY,
        ZYDIS_MNEMONIC_CLTS,
        ZYDIS_MNEMONIC_CLUI,
        ZYDIS_MNEMONIC_CLWB,
        ZYDIS_MNEMONIC_CLZERO,
        ZYDIS_MNEMONIC_CMC,
        ZYDIS_MNEMONIC_CMOVB,
        ZYDIS_MNEMONIC_CMOVBE,
        ZYDIS_MNEMONIC_CMOVL,
        ZYDIS_MNEMONIC_CMOVLE,
        ZYDIS_MNEMONIC_CMOVNB,
        ZYDIS_MNEMONIC_CMOVNBE,
        ZYDIS_MNEMONIC_CMOVNL,
        ZYDIS_MNEMONIC_CMOVNLE,
        ZYDIS_MNEMONIC_CMOVNO,
        ZYDIS_MNEMONIC_CMOVNP,
        ZYDIS_MNEMONIC_CMOVNS,
        ZYDIS_MNEMONIC_CMOVNZ,
        ZYDIS_MNEMONIC_CMOVO,
        ZYDIS_MNEMONIC_CMOVP,
        ZYDIS_MNEMONIC_CMOVS,
        ZYDIS_MNEMONIC_CMOVZ,
        ZYDIS_MNEMONIC_CMP,
        ZYDIS_MNEMONIC_CMPPD,
        ZYDIS_MNEMONIC_CMPPS,
        ZYDIS_MNEMONIC_CMPSB,
        ZYDIS_MNEMONIC_CMPSD,
        ZYDIS_MNEMONIC_CMPSQ,
        ZYDIS_MNEMONIC_CMPSS,
        ZYDIS_MNEMONIC_CMPSW,
        ZYDIS_MNEMONIC_CMPXCHG,
        ZYDIS_MNEMONIC_CMPXCHG16B,
        ZYDIS_MNEMONIC_CMPXCHG8B,
        ZYDIS_MNEMONIC_COMISD,
        ZYDIS_MNEMONIC_COMISS,
        ZYDIS_MNEMONIC_CPUID,
        ZYDIS_MNEMONIC_CQO,
        ZYDIS_MNEMONIC_CVTDQ2PD,
        ZYDIS_MNEMONIC_CVTDQ2PS,
        ZYDIS_MNEMONIC_CVTPD2DQ,
        ZYDIS_MNEMONIC_CVTPD2PI,
        ZYDIS_MNEMONIC_CVTPD2PS,
        ZYDIS_MNEMONIC_CVTPI2PD,
        ZYDIS_MNEMONIC_CVTPI2PS,
        ZYDIS_MNEMONIC_CVTPS2DQ,
        ZYDIS_MNEMONIC_CVTPS2PD,
        ZYDIS_MNEMONIC_CVTPS2PI,
        ZYDIS_MNEMONIC_CVTSD2SI,
        ZYDIS_MNEMONIC_CVTSD2SS,
        ZYDIS_MNEMONIC_CVTSI2SD,
        ZYDIS_MNEMONIC_CVTSI2SS,
        ZYDIS_MNEMONIC_CVTSS2SD,
        ZYDIS_MNEMONIC_CVTSS2SI,
        ZYDIS_MNEMONIC_CVTTPD2DQ,
        ZYDIS_MNEMONIC_CVTTPD2PI,
        ZYDIS_MNEMONIC_CVTTPS2DQ,
        ZYDIS_MNEMONIC_CVTTPS2PI,
        ZYDIS_MNEMONIC_CVTTSD2SI,
        ZYDIS_MNEMONIC_CVTTSS2SI,
        ZYDIS_MNEMONIC_CWD,
        ZYDIS_MNEMONIC_CWDE,
        ZYDIS_MNEMONIC_DAA,
        ZYDIS_MNEMONIC_DAS,
        ZYDIS_MNEMONIC_DEC,
        ZYDIS_MNEMONIC_DELAY,
        ZYDIS_MNEMONIC_DIV,
        ZYDIS_MNEMONIC_DIVPD,
        ZYDIS_MNEMONIC_DIVPS,
        ZYDIS_MNEMONIC_DIVSD,
        ZYDIS_MNEMONIC_DIVSS,
        ZYDIS_MNEMONIC_DPPD,
        ZYDIS_MNEMONIC_DPPS,
        ZYDIS_MNEMONIC_EMMS,
        ZYDIS_MNEMONIC_ENCLS,
        ZYDIS_MNEMONIC_ENCLU,
        ZYDIS_MNEMONIC_ENCLV,
        ZYDIS_MNEMONIC_ENCODEKEY128,
        ZYDIS_MNEMONIC_ENCODEKEY256,
        ZYDIS_MNEMONIC_ENDBR32,
        ZYDIS_MNEMONIC_ENDBR64,
        ZYDIS_MNEMONIC_ENQCMD,
        ZYDIS_MNEMONIC_ENQCMDS,
        ZYDIS_MNEMONIC_ENTER,
        ZYDIS_MNEMONIC_ERETS,
        ZYDIS_MNEMONIC_ERETU,
        ZYDIS_MNEMONIC_EXTRACTPS,
        ZYDIS_MNEMONIC_EXTRQ,
        ZYDIS_MNEMONIC_F2XM1,
        ZYDIS_MNEMONIC_FABS,
        ZYDIS_MNEMONIC_FADD,
        ZYDIS_MNEMONIC_FADDP,
        ZYDIS_MNEMONIC_FBLD,
        ZYDIS_MNEMONIC_FBSTP,
        ZYDIS_MNEMONIC_FCHS,
        ZYDIS_MNEMONIC_FCMOVB,
        ZYDIS_MNEMONIC_FCMOVBE,
        ZYDIS_MNEMONIC_FCMOVE,
        ZYDIS_MNEMONIC_FCMOVNB,
        ZYDIS_MNEMONIC_FCMOVNBE,
        ZYDIS_MNEMONIC_FCMOVNE,
        ZYDIS_MNEMONIC_FCMOVNU,
        ZYDIS_MNEMONIC_FCMOVU,
        ZYDIS_MNEMONIC_FCOM,
        ZYDIS_MNEMONIC_FCOMI,
        ZYDIS_MNEMONIC_FCOMIP,
        ZYDIS_MNEMONIC_FCOMP,
        ZYDIS_MNEMONIC_FCOMPP,
        ZYDIS_MNEMONIC_FCOS,
        ZYDIS_MNEMONIC_FDECSTP,
        ZYDIS_MNEMONIC_FDISI8087_NOP,
        ZYDIS_MNEMONIC_FDIV,
        ZYDIS_MNEMONIC_FDIVP,
        ZYDIS_MNEMONIC_FDIVR,
        ZYDIS_MNEMONIC_FDIVRP,
        ZYDIS_MNEMONIC_FEMMS,
        ZYDIS_MNEMONIC_FENI8087_NOP,
        ZYDIS_MNEMONIC_FFREE,
        ZYDIS_MNEMONIC_FFREEP,
        ZYDIS_MNEMONIC_FIADD,
        ZYDIS_MNEMONIC_FICOM,
        ZYDIS_MNEMONIC_FICOMP,
        ZYDIS_MNEMONIC_FIDIV,
        ZYDIS_MNEMONIC_FIDIVR,
        ZYDIS_MNEMONIC_FILD,
        ZYDIS_MNEMONIC_FIMUL,
        ZYDIS_MNEMONIC_FINCSTP,
        ZYDIS_MNEMONIC_FIST,
        ZYDIS_MNEMONIC_FISTP,
        ZYDIS_MNEMONIC_FISTTP,
        ZYDIS_MNEMONIC_FISUB,
        ZYDIS_MNEMONIC_FISUBR,
        ZYDIS_MNEMONIC_FLD,
        ZYDIS_MNEMONIC_FLD1,
        ZYDIS_MNEMONIC_FLDCW,
        ZYDIS_MNEMONIC_FLDENV,
        ZYDIS_MNEMONIC_FLDL2E,
        ZYDIS_MNEMONIC_FLDL2T,
        ZYDIS_MNEMONIC_FLDLG2,
        ZYDIS_MNEMONIC_FLDLN2,
        ZYDIS_MNEMONIC_FLDPI,
        ZYDIS_MNEMONIC_FLDZ,
        ZYDIS_MNEMONIC_FMUL,
        ZYDIS_MNEMONIC_FMULP,
        ZYDIS_MNEMONIC_FNCLEX,
        ZYDIS_MNEMONIC_FNINIT,
        ZYDIS_MNEMONIC_FNOP,
        ZYDIS_MNEMONIC_FNSAVE,
        ZYDIS_MNEMONIC_FNSTCW,
        ZYDIS_MNEMONIC_FNSTENV,
        ZYDIS_MNEMONIC_FNSTSW,
        ZYDIS_MNEMONIC_FPATAN,
        ZYDIS_MNEMONIC_FPREM,
        ZYDIS_MNEMONIC_FPREM1,
        ZYDIS_MNEMONIC_FPTAN,
        ZYDIS_MNEMONIC_FRNDINT,
        ZYDIS_MNEMONIC_FRSTOR,
        ZYDIS_MNEMONIC_FSCALE,
        ZYDIS_MNEMONIC_FSETPM287_NOP,
        ZYDIS_MNEMONIC_FSIN,
        ZYDIS_MNEMONIC_FSINCOS,
        ZYDIS_MNEMONIC_FSQRT,
        ZYDIS_MNEMONIC_FST,
        ZYDIS_MNEMONIC_FSTP,
        ZYDIS_MNEMONIC_FSTPNCE,
        ZYDIS_MNEMONIC_FSUB,
        ZYDIS_MNEMONIC_FSUBP,
        ZYDIS_MNEMONIC_FSUBR,
        ZYDIS_MNEMONIC_FSUBRP,
        ZYDIS_MNEMONIC_FTST,
        ZYDIS_MNEMONIC_FUCOM,
        ZYDIS_MNEMONIC_FUCOMI,
        ZYDIS_MNEMONIC_FUCOMIP,
        ZYDIS_MNEMONIC_FUCOMP,
        ZYDIS_MNEMONIC_FUCOMPP,
        ZYDIS_MNEMONIC_FWAIT,
        ZYDIS_MNEMONIC_FXAM,
        ZYDIS_MNEMONIC_FXCH,
        ZYDIS_MNEMONIC_FXRSTOR,
        ZYDIS_MNEMONIC_FXRSTOR64,
        ZYDIS_MNEMONIC_FXSAVE,
        ZYDIS_MNEMONIC_FXSAVE64,
        ZYDIS_MNEMONIC_FXTRACT,
        ZYDIS_MNEMONIC_FYL2X,
        ZYDIS_MNEMONIC_FYL2XP1,
        ZYDIS_MNEMONIC_GETSEC,
        ZYDIS_MNEMONIC_GF2P8AFFINEINVQB,
        ZYDIS_MNEMONIC_GF2P8AFFINEQB,
        ZYDIS_MNEMONIC_GF2P8MULB,
        ZYDIS_MNEMONIC_HADDPD,
        ZYDIS_MNEMONIC_HADDPS,
        ZYDIS_MNEMONIC_HRESET,
        ZYDIS_MNEMONIC_HSUBPD,
        ZYDIS_MNEMONIC_HSUBPS,
        ZYDIS_MNEMONIC_IDIV,
        ZYDIS_MNEMONIC_IMUL,
        ZYDIS_MNEMONIC_INC,
        ZYDIS_MNEMONIC_INCSSPD,
        ZYDIS_MNEMONIC_INCSSPQ,
        ZYDIS_MNEMONIC_INSERTPS,
        ZYDIS_MNEMONIC_INSERTQ,
        ZYDIS_MNEMONIC_INT,
        ZYDIS_MNEMONIC_INT1,
        ZYDIS_MNEMONIC_INT3,
        ZYDIS_MNEMONIC_INTO,
        ZYDIS_MNEMONIC_INVD,
        ZYDIS_MNEMONIC_INVEPT,
        ZYDIS_MNEMONIC_INVLPG,
        ZYDIS_MNEMONIC_INVLPGA,
        ZYDIS_MNEMONIC_INVLPGB,
        ZYDIS_MNEMONIC_INVPCID,
        ZYDIS_MNEMONIC_INVVPID,
        ZYDIS_MNEMONIC_KADDB,
        ZYDIS_MNEMONIC_KADDD,
        ZYDIS_MNEMONIC_KADDQ,
        ZYDIS_MNEMONIC_KADDW,
        ZYDIS_MNEMONIC_KAND,
        ZYDIS_MNEMONIC_KANDB,
        ZYDIS_MNEMONIC_KANDD,
        ZYDIS_MNEMONIC_KANDN,
        ZYDIS_MNEMONIC_KANDNB,
        ZYDIS_MNEMONIC_KANDND,
        ZYDIS_MNEMONIC_KANDNQ,
        ZYDIS_MNEMONIC_KANDNR,
        ZYDIS_MNEMONIC_KANDNW,
        ZYDIS_MNEMONIC_KANDQ,
        ZYDIS_MNEMONIC_KANDW,
        ZYDIS_MNEMONIC_KCONCATH,
        ZYDIS_MNEMONIC_KCONCATL,
        ZYDIS_MNEMONIC_KEXTRACT,
        ZYDIS_MNEMONIC_KMERGE2L1H,
        ZYDIS_MNEMONIC_KMERGE2L1L,
        ZYDIS_MNEMONIC_KMOV,
        ZYDIS_MNEMONIC_KMOVB,
        ZYDIS_MNEMONIC_KMOVD,
        ZYDIS_MNEMONIC_KMOVQ,
        ZYDIS_MNEMONIC_KMOVW,
        ZYDIS_MNEMONIC_KNOT,
        ZYDIS_MNEMONIC_KNOTB,
        ZYDIS_MNEMONIC_KNOTD,
        ZYDIS_MNEMONIC_KNOTQ,
        ZYDIS_MNEMONIC_KNOTW,
        ZYDIS_MNEMONIC_KOR,
        ZYDIS_MNEMONIC_KORB,
        ZYDIS_MNEMONIC_KORD,
        ZYDIS_MNEMONIC_KORQ,
        ZYDIS_MNEMONIC_KORTEST,
        ZYDIS_MNEMONIC_KORTESTB,
        ZYDIS_MNEMONIC_KORTESTD,
        ZYDIS_MNEMONIC_KORTESTQ,
        ZYDIS_MNEMONIC_KORTESTW,
        ZYDIS_MNEMONIC_KORW,
        ZYDIS_MNEMONIC_KSHIFTLB,
        ZYDIS_MNEMONIC_KSHIFTLD,
        ZYDIS_MNEMONIC_KSHIFTLQ,
        ZYDIS_MNEMONIC_KSHIFTLW,
        ZYDIS_MNEMONIC_KSHIFTRB,
        ZYDIS_MNEMONIC_KSHIFTRD,
        ZYDIS_MNEMONIC_KSHIFTRQ,
        ZYDIS_MNEMONIC_KSHIFTRW,
        ZYDIS_MNEMONIC_KTESTB,
        ZYDIS_MNEMONIC_KTESTD,
        ZYDIS_MNEMONIC_KTESTQ,
        ZYDIS_MNEMONIC_KTESTW,
        ZYDIS_MNEMONIC_KUNPCKBW,
        ZYDIS_MNEMONIC_KUNPCKDQ,
        ZYDIS_MNEMONIC_KUNPCKWD,
        ZYDIS_MNEMONIC_KXNOR,
        ZYDIS_MNEMONIC_KXNORB,
        ZYDIS_MNEMONIC_KXNORD,
        ZYDIS_MNEMONIC_KXNORQ,
        ZYDIS_MNEMONIC_KXNORW,
        ZYDIS_MNEMONIC_KXOR,
        ZYDIS_MNEMONIC_KXORB,
        ZYDIS_MNEMONIC_KXORD,
        ZYDIS_MNEMONIC_KXORQ,
        ZYDIS_MNEMONIC_KXORW,
        ZYDIS_MNEMONIC_LAHF,
        ZYDIS_MNEMONIC_LAR,
        ZYDIS_MNEMONIC_LDDQU,
        ZYDIS_MNEMONIC_LDMXCSR,
        ZYDIS_MNEMONIC_LDS,
        ZYDIS_MNEMONIC_LDTILECFG,
        ZYDIS_MNEMONIC_LEA,
        ZYDIS_MNEMONIC_LES,
        ZYDIS_MNEMONIC_LFENCE,
        ZYDIS_MNEMONIC_LFS,
        ZYDIS_MNEMONIC_LGDT,
        ZYDIS_MNEMONIC_LGS,
        ZYDIS_MNEMONIC_LIDT,
        ZYDIS_MNEMONIC_LKGS,
        ZYDIS_MNEMONIC_LLDT,
        ZYDIS_MNEMONIC_LLWPCB,
        ZYDIS_MNEMONIC_LMSW,
        ZYDIS_MNEMONIC_LOADIWKEY,
        ZYDIS_MNEMONIC_LSL,
        ZYDIS_MNEMONIC_LSS,
        ZYDIS_MNEMONIC_LTR,
        ZYDIS_MNEMONIC_LWPINS,
        ZYDIS_MNEMONIC_LWPVAL,
        ZYDIS_MNEMONIC_LZCNT,
        ZYDIS_MNEMONIC_MASKMOVDQU,
        ZYDIS_MNEMONIC_MASKMOVQ,
        ZYDIS_MNEMONIC_MAXPD,
        ZYDIS_MNEMONIC_MAXPS,
        ZYDIS_MNEMONIC_MAXSD,
        ZYDIS_MNEMONIC_MAXSS,
        ZYDIS_MNEMONIC_MCOMMIT,
        ZYDIS_MNEMONIC_MFENCE,
        ZYDIS_MNEMONIC_MINPD,
        ZYDIS_MNEMONIC_MINPS,
        ZYDIS_MNEMONIC_MINSD,
        ZYDIS_MNEMONIC_MINSS,
        ZYDIS_MNEMONIC_MONITOR,
        ZYDIS_MNEMONIC_MONITORX,
        ZYDIS_MNEMONIC_MONTMUL,
        ZYDIS_MNEMONIC_MOV,
        ZYDIS_MNEMONIC_MOVAPD,
        ZYDIS_MNEMONIC_MOVAPS,
        ZYDIS_MNEMONIC_MOVBE,
        ZYDIS_MNEMONIC_MOVD,
        ZYDIS_MNEMONIC_MOVDDUP,
        ZYDIS_MNEMONIC_MOVDIR64B,
        ZYDIS_MNEMONIC_MOVDIRI,
        ZYDIS_MNEMONIC_MOVDQ2Q,
        ZYDIS_MNEMONIC_MOVDQA,
        ZYDIS_MNEMONIC_MOVDQU,
        ZYDIS_MNEMONIC_MOVHLPS,
        ZYDIS_MNEMONIC_MOVHPD,
        ZYDIS_MNEMONIC_MOVHPS,
        ZYDIS_MNEMONIC_MOVLHPS,
        ZYDIS_MNEMONIC_MOVLPD,
        ZYDIS_MNEMONIC_MOVLPS,
        ZYDIS_MNEMONIC_MOVMSKPD,
        ZYDIS_MNEMONIC_MOVMSKPS,
        ZYDIS_MNEMONIC_MOVNTDQ,
        ZYDIS_MNEMONIC_MOVNTDQA,
        ZYDIS_MNEMONIC_MOVNTI,
        ZYDIS_MNEMONIC_MOVNTPD,
        ZYDIS_MNEMONIC_MOVNTPS,
        ZYDIS_MNEMONIC_MOVNTQ,
        ZYDIS_MNEMONIC_MOVNTSD,
        ZYDIS_MNEMONIC_MOVNTSS,
        ZYDIS_MNEMONIC_MOVQ,
        ZYDIS_MNEMONIC_MOVQ2DQ,
        ZYDIS_MNEMONIC_MOVSB,
        ZYDIS_MNEMONIC_MOVSD,
        ZYDIS_MNEMONIC_MOVSHDUP,
        ZYDIS_MNEMONIC_MOVSLDUP,
        ZYDIS_MNEMONIC_MOVSQ,
        ZYDIS_MNEMONIC_MOVSS,
        ZYDIS_MNEMONIC_MOVSW,
        ZYDIS_MNEMONIC_MOVSX,
        ZYDIS_MNEMONIC_MOVSXD,
        ZYDIS_MNEMONIC_MOVUPD,
        ZYDIS_MNEMONIC_MOVUPS,
        ZYDIS_MNEMONIC_MOVZX,
        ZYDIS_MNEMONIC_MPSADBW,
        ZYDIS_MNEMONIC_MUL,
        ZYDIS_MNEMONIC_MULPD,
        ZYDIS_MNEMONIC_MULPS,
        ZYDIS_MNEMONIC_MULSD,
        ZYDIS_MNEMONIC_MULSS,
        ZYDIS_MNEMONIC_MULX,
        ZYDIS_MNEMONIC_MWAIT,
        ZYDIS_MNEMONIC_MWAITX,
        ZYDIS_MNEMONIC_NEG,
        ZYDIS_MNEMONIC_NOP,
        ZYDIS_MNEMONIC_NOT,
        ZYDIS_MNEMONIC_OR,
        ZYDIS_MNEMONIC_ORPD,
        ZYDIS_MNEMONIC_ORPS,
        ZYDIS_MNEMONIC_PABSB,
        ZYDIS_MNEMONIC_PABSD,
        ZYDIS_MNEMONIC_PABSW,
        ZYDIS_MNEMONIC_PACKSSDW,
        ZYDIS_MNEMONIC_PACKSSWB,
        ZYDIS_MNEMONIC_PACKUSDW,
        ZYDIS_MNEMONIC_PACKUSWB,
        ZYDIS_MNEMONIC_PADDB,
        ZYDIS_MNEMONIC_PADDD,
        ZYDIS_MNEMONIC_PADDQ,
        ZYDIS_MNEMONIC_PADDSB,
        ZYDIS_MNEMONIC_PADDSW,
        ZYDIS_MNEMONIC_PADDUSB,
        ZYDIS_MNEMONIC_PADDUSW,
        ZYDIS_MNEMONIC_PADDW,
        ZYDIS_MNEMONIC_PALIGNR,
        ZYDIS_MNEMONIC_PAND,
        ZYDIS_MNEMONIC_PANDN,
        ZYDIS_MNEMONIC_PAUSE,
        ZYDIS_MNEMONIC_PAVGB,
        ZYDIS_MNEMONIC_PAVGUSB,
        ZYDIS_MNEMONIC_PAVGW,
        ZYDIS_MNEMONIC_PBLENDVB,
        ZYDIS_MNEMONIC_PBLENDW,
        ZYDIS_MNEMONIC_PBNDKB,
        ZYDIS_MNEMONIC_PCLMULQDQ,
        ZYDIS_MNEMONIC_PCMPEQB,
        ZYDIS_MNEMONIC_PCMPEQD,
        ZYDIS_MNEMONIC_PCMPEQQ,
        ZYDIS_MNEMONIC_PCMPEQW,
        ZYDIS_MNEMONIC_PCMPESTRI,
        ZYDIS_MNEMONIC_PCMPESTRM,
        ZYDIS_MNEMONIC_PCMPGTB,
        ZYDIS_MNEMONIC_PCMPGTD,
        ZYDIS_MNEMONIC_PCMPGTQ,
        ZYDIS_MNEMONIC_PCMPGTW,
        ZYDIS_MNEMONIC_PCMPISTRI,
        ZYDIS_MNEMONIC_PCMPISTRM,
        ZYDIS_MNEMONIC_PCOMMIT,
        ZYDIS_MNEMONIC_PCONFIG,
        ZYDIS_MNEMONIC_PDEP,
        ZYDIS_MNEMONIC_PEXT,
        ZYDIS_MNEMONIC_PEXTRB,
        ZYDIS_MNEMONIC_PEXTRD,
        ZYDIS_MNEMONIC_PEXTRQ,
        ZYDIS_MNEMONIC_PEXTRW,
        ZYDIS_MNEMONIC_PF2ID,
        ZYDIS_MNEMONIC_PF2IW,
        ZYDIS_MNEMONIC_PFACC,
        ZYDIS_MNEMONIC_PFADD,
        ZYDIS_MNEMONIC_PFCMPEQ,
        ZYDIS_MNEMONIC_PFCMPGE,
        ZYDIS_MNEMONIC_PFCMPGT,
        ZYDIS_MNEMONIC_PFCPIT1,
        ZYDIS_MNEMONIC_PFMAX,
        ZYDIS_MNEMONIC_PFMIN,
        ZYDIS_MNEMONIC_PFMUL,
        ZYDIS_MNEMONIC_PFNACC,
        ZYDIS_MNEMONIC_PFPNACC,
        ZYDIS_MNEMONIC_PFRCP,
        ZYDIS_MNEMONIC_PFRCPIT2,
        ZYDIS_MNEMONIC_PFRSQIT1,
        ZYDIS_MNEMONIC_PFSQRT,
        ZYDIS_MNEMONIC_PFSUB,
        ZYDIS_MNEMONIC_PFSUBR,
        ZYDIS_MNEMONIC_PHADDD,
        ZYDIS_MNEMONIC_PHADDSW,
        ZYDIS_MNEMONIC_PHADDW,
        ZYDIS_MNEMONIC_PHMINPOSUW,
        ZYDIS_MNEMONIC_PHSUBD,
        ZYDIS_MNEMONIC_PHSUBSW,
        ZYDIS_MNEMONIC_PHSUBW,
        ZYDIS_MNEMONIC_PI2FD,
        ZYDIS_MNEMONIC_PI2FW,
        ZYDIS_MNEMONIC_PINSRB,
        ZYDIS_MNEMONIC_PINSRD,
        ZYDIS_MNEMONIC_PINSRQ,
        ZYDIS_MNEMONIC_PINSRW,
        ZYDIS_MNEMONIC_PMADDUBSW,
        ZYDIS_MNEMONIC_PMADDWD,
        ZYDIS_MNEMONIC_PMAXSB,
        ZYDIS_MNEMONIC_PMAXSD,
        ZYDIS_MNEMONIC_PMAXSW,
        ZYDIS_MNEMONIC_PMAXUB,
        ZYDIS_MNEMONIC_PMAXUD,
        ZYDIS_MNEMONIC_PMAXUW,
        ZYDIS_MNEMONIC_PMINSB,
        ZYDIS_MNEMONIC_PMINSD,
        ZYDIS_MNEMONIC_PMINSW,
        ZYDIS_MNEMONIC_PMINUB,
        ZYDIS_MNEMONIC_PMINUD,
        ZYDIS_MNEMONIC_PMINUW,
        ZYDIS_MNEMONIC_PMOVMSKB,
        ZYDIS_MNEMONIC_PMOVSXBD,
        ZYDIS_MNEMONIC_PMOVSXBQ,
        ZYDIS_MNEMONIC_PMOVSXBW,
        ZYDIS_MNEMONIC_PMOVSXDQ,
        ZYDIS_MNEMONIC_PMOVSXWD,
        ZYDIS_MNEMONIC_PMOVSXWQ,
        ZYDIS_MNEMONIC_PMOVZXBD,
        ZYDIS_MNEMONIC_PMOVZXBQ,
        ZYDIS_MNEMONIC_PMOVZXBW,
        ZYDIS_MNEMONIC_PMOVZXDQ,
        ZYDIS_MNEMONIC_PMOVZXWD,
        ZYDIS_MNEMONIC_PMOVZXWQ,
        ZYDIS_MNEMONIC_PMULDQ,
        ZYDIS_MNEMONIC_PMULHRSW,
        ZYDIS_MNEMONIC_PMULHRW,
        ZYDIS_MNEMONIC_PMULHUW,
        ZYDIS_MNEMONIC_PMULHW,
        ZYDIS_MNEMONIC_PMULLD,
        ZYDIS_MNEMONIC_PMULLW,
        ZYDIS_MNEMONIC_PMULUDQ,
        ZYDIS_MNEMONIC_POR,
        ZYDIS_MNEMONIC_PREFETCH,
        ZYDIS_MNEMONIC_PREFETCHIT0,
        ZYDIS_MNEMONIC_PREFETCHIT1,
        ZYDIS_MNEMONIC_PREFETCHNTA,
        ZYDIS_MNEMONIC_PREFETCHT0,
        ZYDIS_MNEMONIC_PREFETCHT1,
        ZYDIS_MNEMONIC_PREFETCHT2,
        ZYDIS_MNEMONIC_PREFETCHW,
        ZYDIS_MNEMONIC_PREFETCHWT1,
        ZYDIS_MNEMONIC_PSADBW,
        ZYDIS_MNEMONIC_PSHUFB,
        ZYDIS_MNEMONIC_PSHUFD,
        ZYDIS_MNEMONIC_PSHUFHW,
        ZYDIS_MNEMONIC_PSHUFLW,
        ZYDIS_MNEMONIC_PSHUFW,
        ZYDIS_MNEMONIC_PSIGNB,
        ZYDIS_MNEMONIC_PSIGND,
        ZYDIS_MNEMONIC_PSIGNW,
        ZYDIS_MNEMONIC_PSLLD,
        ZYDIS_MNEMONIC_PSLLDQ,
        ZYDIS_MNEMONIC_PSLLQ,
        ZYDIS_MNEMONIC_PSLLW,
        ZYDIS_MNEMONIC_PSMASH,
        ZYDIS_MNEMONIC_PSRAD,
        ZYDIS_MNEMONIC_PSRAW,
        ZYDIS_MNEMONIC_PSRLD,
        ZYDIS_MNEMONIC_PSRLDQ,
        ZYDIS_MNEMONIC_PSRLQ,
        ZYDIS_MNEMONIC_PSRLW,
        ZYDIS_MNEMONIC_PSUBB,
        ZYDIS_MNEMONIC_PSUBD,
        ZYDIS_MNEMONIC_PSUBQ,
        ZYDIS_MNEMONIC_PSUBSB,
        ZYDIS_MNEMONIC_PSUBSW,
        ZYDIS_MNEMONIC_PSUBUSB,
        ZYDIS_MNEMONIC_PSUBUSW,
        ZYDIS_MNEMONIC_PSUBW,
        ZYDIS_MNEMONIC_PSWAPD,
        ZYDIS_MNEMONIC_PTEST,
        ZYDIS_MNEMONIC_PTWRITE,
        ZYDIS_MNEMONIC_PUNPCKHBW,
        ZYDIS_MNEMONIC_PUNPCKHDQ,
        ZYDIS_MNEMONIC_PUNPCKHQDQ,
        ZYDIS_MNEMONIC_PUNPCKHWD,
        ZYDIS_MNEMONIC_PUNPCKLBW,
        ZYDIS_MNEMONIC_PUNPCKLDQ,
        ZYDIS_MNEMONIC_PUNPCKLQDQ,
        ZYDIS_MNEMONIC_PUNPCKLWD,
        ZYDIS_MNEMONIC_PVALIDATE,
        ZYDIS_MNEMONIC_PXOR,
        ZYDIS_MNEMONIC_RCL,
        ZYDIS_MNEMONIC_RCPPS,
        ZYDIS_MNEMONIC_RCPSS,
        ZYDIS_MNEMONIC_RCR,
        ZYDIS_MNEMONIC_RDFSBASE,
        ZYDIS_MNEMONIC_RDGSBASE,
        ZYDIS_MNEMONIC_RDMSR,
        ZYDIS_MNEMONIC_RDMSRLIST,
        ZYDIS_MNEMONIC_RDPID,
        ZYDIS_MNEMONIC_RDPKRU,
        ZYDIS_MNEMONIC_RDPMC,
        ZYDIS_MNEMONIC_RDPRU,
        ZYDIS_MNEMONIC_RDRAND,
        ZYDIS_MNEMONIC_RDSEED,
        ZYDIS_MNEMONIC_RDSSPD,
        ZYDIS_MNEMONIC_RDSSPQ,
        ZYDIS_MNEMONIC_RDTSC,
        ZYDIS_MNEMONIC_RDTSCP,
        ZYDIS_MNEMONIC_RMPADJUST,
        ZYDIS_MNEMONIC_RMPUPDATE,
        ZYDIS_MNEMONIC_ROL,
        ZYDIS_MNEMONIC_ROR,
        ZYDIS_MNEMONIC_RORX,
        ZYDIS_MNEMONIC_ROUNDPD,
        ZYDIS_MNEMONIC_ROUNDPS,
        ZYDIS_MNEMONIC_ROUNDSD,
        ZYDIS_MNEMONIC_ROUNDSS,
        ZYDIS_MNEMONIC_RSM,
        ZYDIS_MNEMONIC_RSQRTPS,
        ZYDIS_MNEMONIC_RSQRTSS,
        ZYDIS_MNEMONIC_RSTORSSP,
        ZYDIS_MNEMONIC_SAHF,
        ZYDIS_MNEMONIC_SALC,
        ZYDIS_MNEMONIC_SAR,
        ZYDIS_MNEMONIC_SARX,
        ZYDIS_MNEMONIC_SAVEPREVSSP,
        ZYDIS_MNEMONIC_SBB,
        ZYDIS_MNEMONIC_SCASB,
        ZYDIS_MNEMONIC_SCASD,
        ZYDIS_MNEMONIC_SCASQ,
        ZYDIS_MNEMONIC_SCASW,
        ZYDIS_MNEMONIC_SENDUIPI,
        ZYDIS_MNEMONIC_SERIALIZE,
        ZYDIS_MNEMONIC_SETB,
        ZYDIS_MNEMONIC_SETBE,
        ZYDIS_MNEMONIC_SETL,
        ZYDIS_MNEMONIC_SETLE,
        ZYDIS_MNEMONIC_SETNB,
        ZYDIS_MNEMONIC_SETNBE,
        ZYDIS_MNEMONIC_SETNL,
        ZYDIS_MNEMONIC_SETNLE,
        ZYDIS_MNEMONIC_SETNO,
        ZYDIS_MNEMONIC_SETNP,
        ZYDIS_MNEMONIC_SETNS,
        ZYDIS_MNEMONIC_SETNZ,
        ZYDIS_MNEMONIC_SETO,
        ZYDIS_MNEMONIC_SETP,
        ZYDIS_MNEMONIC_SETS,
        ZYDIS_MNEMONIC_SETSSBSY,
        ZYDIS_MNEMONIC_SETZ,
        ZYDIS_MNEMONIC_SFENCE,
        ZYDIS_MNEMONIC_SGDT,
        ZYDIS_MNEMONIC_SHA1MSG1,
        ZYDIS_MNEMONIC_SHA1MSG2,
        ZYDIS_MNEMONIC_SHA1NEXTE,
        ZYDIS_MNEMONIC_SHA1RNDS4,
        ZYDIS_MNEMONIC_SHA256MSG1,
        ZYDIS_MNEMONIC_SHA256MSG2,
        ZYDIS_MNEMONIC_SHA256RNDS2,
        ZYDIS_MNEMONIC_SHL,
        ZYDIS_MNEMONIC_SHLD,
        ZYDIS_MNEMONIC_SHLX,
        ZYDIS_MNEMONIC_SHR,
        ZYDIS_MNEMONIC_SHRD,
        ZYDIS_MNEMONIC_SHRX,
        ZYDIS_MNEMONIC_SHUFPD,
        ZYDIS_MNEMONIC_SHUFPS,
        ZYDIS_MNEMONIC_SIDT,
        ZYDIS_MNEMONIC_SKINIT,
        ZYDIS_MNEMONIC_SLDT,
        ZYDIS_MNEMONIC_SLWPCB,
        ZYDIS_MNEMONIC_SMSW,
        ZYDIS_MNEMONIC_SPFLT,
        ZYDIS_MNEMONIC_SQRTPD,
        ZYDIS_MNEMONIC_SQRTPS,
        ZYDIS_MNEMONIC_SQRTSD,
        ZYDIS_MNEMONIC_SQRTSS,
        ZYDIS_MNEMONIC_STAC,
        ZYDIS_MNEMONIC_STC,
        ZYDIS_MNEMONIC_STD,
        ZYDIS_MNEMONIC_STGI,
        ZYDIS_MNEMONIC_STI,
        ZYDIS_MNEMONIC_STMXCSR,
        ZYDIS_MNEMONIC_STR,
        ZYDIS_MNEMONIC_STTILECFG,
        ZYDIS_MNEMONIC_STUI,
        ZYDIS_MNEMONIC_SUB,
        ZYDIS_MNEMONIC_SUBPD,
        ZYDIS_MNEMONIC_SUBPS,
        ZYDIS_MNEMONIC_SUBSD,
        ZYDIS_MNEMONIC_SUBSS,
        ZYDIS_MNEMONIC_SWAPGS,
        ZYDIS_MNEMONIC_T1MSKC,
        ZYDIS_MNEMONIC_TDCALL,
        ZYDIS_MNEMONIC_TDPBF16PS,
        ZYDIS_MNEMONIC_TDPBSSD,
        ZYDIS_MNEMONIC_TDPBSUD,
        ZYDIS_MNEMONIC_TDPBUSD,
        ZYDIS_MNEMONIC_TDPBUUD,
        ZYDIS_MNEMONIC_TDPFP16PS,
        ZYDIS_MNEMONIC_TEST,
        ZYDIS_MNEMONIC_TESTUI,
        ZYDIS_MNEMONIC_TILELOADD,
        ZYDIS_MNEMONIC_TILELOADDT1,
        ZYDIS_MNEMONIC_TILERELEASE,
        ZYDIS_MNEMONIC_TILESTORED,
        ZYDIS_MNEMONIC_TILEZERO,
        ZYDIS_MNEMONIC_TLBSYNC,
        ZYDIS_MNEMONIC_TPAUSE,
        ZYDIS_MNEMONIC_TZCNT,
        ZYDIS_MNEMONIC_TZCNTI,
        ZYDIS_MNEMONIC_TZMSK,
        ZYDIS_MNEMONIC_UCOMISD,
        ZYDIS_MNEMONIC_UCOMISS,
        ZYDIS_MNEMONIC_UIRET,
        ZYDIS_MNEMONIC_UMONITOR,
        ZYDIS_MNEMONIC_UMWAIT,
        ZYDIS_MNEMONIC_UNPCKHPD,
        ZYDIS_MNEMONIC_UNPCKHPS,
        ZYDIS_MNEMONIC_UNPCKLPD,
        ZYDIS_MNEMONIC_UNPCKLPS,
        ZYDIS_MNEMONIC_V4FMADDPS,
        ZYDIS_MNEMONIC_V4FMADDSS,
        ZYDIS_MNEMONIC_V4FNMADDPS,
        ZYDIS_MNEMONIC_V4FNMADDSS,
        ZYDIS_MNEMONIC_VADDNPD,
        ZYDIS_MNEMONIC_VADDNPS,
        ZYDIS_MNEMONIC_VADDPD,
        ZYDIS_MNEMONIC_VADDPH,
        ZYDIS_MNEMONIC_VADDPS,
        ZYDIS_MNEMONIC_VADDSD,
        ZYDIS_MNEMONIC_VADDSETSPS,
        ZYDIS_MNEMONIC_VADDSH,
        ZYDIS_MNEMONIC_VADDSS,
        ZYDIS_MNEMONIC_VADDSUBPD,
        ZYDIS_MNEMONIC_VADDSUBPS,
        ZYDIS_MNEMONIC_VAESDEC,
        ZYDIS_MNEMONIC_VAESDECLAST,
        ZYDIS_MNEMONIC_VAESENC,
        ZYDIS_MNEMONIC_VAESENCLAST,
        ZYDIS_MNEMONIC_VAESIMC,
        ZYDIS_MNEMONIC_VAESKEYGENASSIST,
        ZYDIS_MNEMONIC_VALIGND,
        ZYDIS_MNEMONIC_VALIGNQ,
        ZYDIS_MNEMONIC_VANDNPD,
        ZYDIS_MNEMONIC_VANDNPS,
        ZYDIS_MNEMONIC_VANDPD,
        ZYDIS_MNEMONIC_VANDPS,
        ZYDIS_MNEMONIC_VBCSTNEBF162PS,
        ZYDIS_MNEMONIC_VBCSTNESH2PS,
        ZYDIS_MNEMONIC_VBLENDMPD,
        ZYDIS_MNEMONIC_VBLENDMPS,
        ZYDIS_MNEMONIC_VBLENDPD,
        ZYDIS_MNEMONIC_VBLENDPS,
        ZYDIS_MNEMONIC_VBLENDVPD,
        ZYDIS_MNEMONIC_VBLENDVPS,
        ZYDIS_MNEMONIC_VBROADCASTF128,
        ZYDIS_MNEMONIC_VBROADCASTF32X2,
        ZYDIS_MNEMONIC_VBROADCASTF32X4,
        ZYDIS_MNEMONIC_VBROADCASTF32X8,
        ZYDIS_MNEMONIC_VBROADCASTF64X2,
        ZYDIS_MNEMONIC_VBROADCASTF64X4,
        ZYDIS_MNEMONIC_VBROADCASTI128,
        ZYDIS_MNEMONIC_VBROADCASTI32X2,
        ZYDIS_MNEMONIC_VBROADCASTI32X4,
        ZYDIS_MNEMONIC_VBROADCASTI32X8,
        ZYDIS_MNEMONIC_VBROADCASTI64X2,
        ZYDIS_MNEMONIC_VBROADCASTI64X4,
        ZYDIS_MNEMONIC_VBROADCASTSD,
        ZYDIS_MNEMONIC_VBROADCASTSS,
        ZYDIS_MNEMONIC_VCMPPD,
        ZYDIS_MNEMONIC_VCMPPH,
        ZYDIS_MNEMONIC_VCMPPS,
        ZYDIS_MNEMONIC_VCMPSD,
        ZYDIS_MNEMONIC_VCMPSH,
        ZYDIS_MNEMONIC_VCMPSS,
        ZYDIS_MNEMONIC_VCOMISD,
        ZYDIS_MNEMONIC_VCOMISH,
        ZYDIS_MNEMONIC_VCOMISS,
        ZYDIS_MNEMONIC_VCOMPRESSPD,
        ZYDIS_MNEMONIC_VCOMPRESSPS,
        ZYDIS_MNEMONIC_VCVTDQ2PD,
        ZYDIS_MNEMONIC_VCVTDQ2PH,
        ZYDIS_MNEMONIC_VCVTDQ2PS,
        ZYDIS_MNEMONIC_VCVTFXPNTDQ2PS,
        ZYDIS_MNEMONIC_VCVTFXPNTPD2DQ,
        ZYDIS_MNEMONIC_VCVTFXPNTPD2UDQ,
        ZYDIS_MNEMONIC_VCVTFXPNTPS2DQ,
        ZYDIS_MNEMONIC_VCVTFXPNTPS2UDQ,
        ZYDIS_MNEMONIC_VCVTFXPNTUDQ2PS,
        ZYDIS_MNEMONIC_VCVTNE2PS2BF16,
        ZYDIS_MNEMONIC_VCVTNEEBF162PS,
        ZYDIS_MNEMONIC_VCVTNEEPH2PS,
        ZYDIS_MNEMONIC_VCVTNEOBF162PS,
        ZYDIS_MNEMONIC_VCVTNEOPH2PS,
        ZYDIS_MNEMONIC_VCVTNEPS2BF16,
        ZYDIS_MNEMONIC_VCVTPD2DQ,
        ZYDIS_MNEMONIC_VCVTPD2PH,
        ZYDIS_MNEMONIC_VCVTPD2PS,
        ZYDIS_MNEMONIC_VCVTPD2QQ,
        ZYDIS_MNEMONIC_VCVTPD2UDQ,
        ZYDIS_MNEMONIC_VCVTPD2UQQ,
        ZYDIS_MNEMONIC_VCVTPH2DQ,
        ZYDIS_MNEMONIC_VCVTPH2PD,
        ZYDIS_MNEMONIC_VCVTPH2PS,
        ZYDIS_MNEMONIC_VCVTPH2PSX,
        ZYDIS_MNEMONIC_VCVTPH2QQ,
        ZYDIS_MNEMONIC_VCVTPH2UDQ,
        ZYDIS_MNEMONIC_VCVTPH2UQQ,
        ZYDIS_MNEMONIC_VCVTPH2UW,
        ZYDIS_MNEMONIC_VCVTPH2W,
        ZYDIS_MNEMONIC_VCVTPS2DQ,
        ZYDIS_MNEMONIC_VCVTPS2PD,
        ZYDIS_MNEMONIC_VCVTPS2PH,
        ZYDIS_MNEMONIC_VCVTPS2PHX,
        ZYDIS_MNEMONIC_VCVTPS2QQ,
        ZYDIS_MNEMONIC_VCVTPS2UDQ,
        ZYDIS_MNEMONIC_VCVTPS2UQQ,
        ZYDIS_MNEMONIC_VCVTQQ2PD,
        ZYDIS_MNEMONIC_VCVTQQ2PH,
        ZYDIS_MNEMONIC_VCVTQQ2PS,
        ZYDIS_MNEMONIC_VCVTSD2SH,
        ZYDIS_MNEMONIC_VCVTSD2SI,
        ZYDIS_MNEMONIC_VCVTSD2SS,
        ZYDIS_MNEMONIC_VCVTSD2USI,
        ZYDIS_MNEMONIC_VCVTSH2SD,
        ZYDIS_MNEMONIC_VCVTSH2SI,
        ZYDIS_MNEMONIC_VCVTSH2SS,
        ZYDIS_MNEMONIC_VCVTSH2USI,
        ZYDIS_MNEMONIC_VCVTSI2SD,
        ZYDIS_MNEMONIC_VCVTSI2SH,
        ZYDIS_MNEMONIC_VCVTSI2SS,
        ZYDIS_MNEMONIC_VCVTSS2SD,
        ZYDIS_MNEMONIC_VCVTSS2SH,
        ZYDIS_MNEMONIC_VCVTSS2SI,
        ZYDIS_MNEMONIC_VCVTSS2USI,
        ZYDIS_MNEMONIC_VCVTTPD2DQ,
        ZYDIS_MNEMONIC_VCVTTPD2QQ,
        ZYDIS_MNEMONIC_VCVTTPD2UDQ,
        ZYDIS_MNEMONIC_VCVTTPD2UQQ,
        ZYDIS_MNEMONIC_VCVTTPH2DQ,
        ZYDIS_MNEMONIC_VCVTTPH2QQ,
        ZYDIS_MNEMONIC_VCVTTPH2UDQ,
        ZYDIS_MNEMONIC_VCVTTPH2UQQ,
        ZYDIS_MNEMONIC_VCVTTPH2UW,
        ZYDIS_MNEMONIC_VCVTTPH2W,
        ZYDIS_MNEMONIC_VCVTTPS2DQ,
        ZYDIS_MNEMONIC_VCVTTPS2QQ,
        ZYDIS_MNEMONIC_VCVTTPS2UDQ,
        ZYDIS_MNEMONIC_VCVTTPS2UQQ,
        ZYDIS_MNEMONIC_VCVTTSD2SI,
        ZYDIS_MNEMONIC_VCVTTSD2USI,
        ZYDIS_MNEMONIC_VCVTTSH2SI,
        ZYDIS_MNEMONIC_VCVTTSH2USI,
        ZYDIS_MNEMONIC_VCVTTSS2SI,
        ZYDIS_MNEMONIC_VCVTTSS2USI,
        ZYDIS_MNEMONIC_VCVTUDQ2PD,
        ZYDIS_MNEMONIC_VCVTUDQ2PH,
        ZYDIS_MNEMONIC_VCVTUDQ2PS,
        ZYDIS_MNEMONIC_VCVTUQQ2PD,
        ZYDIS_MNEMONIC_VCVTUQQ2PH,
        ZYDIS_MNEMONIC_VCVTUQQ2PS,
        ZYDIS_MNEMONIC_VCVTUSI2SD,
        ZYDIS_MNEMONIC_VCVTUSI2SH,
        ZYDIS_MNEMONIC_VCVTUSI2SS,
        ZYDIS_MNEMONIC_VCVTUW2PH,
        ZYDIS_MNEMONIC_VCVTW2PH,
        ZYDIS_MNEMONIC_VDBPSADBW,
        ZYDIS_MNEMONIC_VDIVPD,
        ZYDIS_MNEMONIC_VDIVPH,
        ZYDIS_MNEMONIC_VDIVPS,
        ZYDIS_MNEMONIC_VDIVSD,
        ZYDIS_MNEMONIC_VDIVSH,
        ZYDIS_MNEMONIC_VDIVSS,
        ZYDIS_MNEMONIC_VDPBF16PS,
        ZYDIS_MNEMONIC_VDPPD,
        ZYDIS_MNEMONIC_VDPPS,
        ZYDIS_MNEMONIC_VERR,
        ZYDIS_MNEMONIC_VERW,
        ZYDIS_MNEMONIC_VEXP223PS,
        ZYDIS_MNEMONIC_VEXP2PD,
        ZYDIS_MNEMONIC_VEXP2PS,
        ZYDIS_MNEMONIC_VEXPANDPD,
        ZYDIS_MNEMONIC_VEXPANDPS,
        ZYDIS_MNEMONIC_VEXTRACTF128,
        ZYDIS_MNEMONIC_VEXTRACTF32X4,
        ZYDIS_MNEMONIC_VEXTRACTF32X8,
        ZYDIS_MNEMONIC_VEXTRACTF64X2,
        ZYDIS_MNEMONIC_VEXTRACTF64X4,
        ZYDIS_MNEMONIC_VEXTRACTI128,
        ZYDIS_MNEMONIC_VEXTRACTI32X4,
        ZYDIS_MNEMONIC_VEXTRACTI32X8,
        ZYDIS_MNEMONIC_VEXTRACTI64X2,
        ZYDIS_MNEMONIC_VEXTRACTI64X4,
        ZYDIS_MNEMONIC_VEXTRACTPS,
        ZYDIS_MNEMONIC_VFCMADDCPH,
        ZYDIS_MNEMONIC_VFCMADDCSH,
        ZYDIS_MNEMONIC_VFCMULCPH,
        ZYDIS_MNEMONIC_VFCMULCSH,
        ZYDIS_MNEMONIC_VFIXUPIMMPD,
        ZYDIS_MNEMONIC_VFIXUPIMMPS,
        ZYDIS_MNEMONIC_VFIXUPIMMSD,
        ZYDIS_MNEMONIC_VFIXUPIMMSS,
        ZYDIS_MNEMONIC_VFIXUPNANPD,
        ZYDIS_MNEMONIC_VFIXUPNANPS,
        ZYDIS_MNEMONIC_VFMADD132PD,
        ZYDIS_MNEMONIC_VFMADD132PH,
        ZYDIS_MNEMONIC_VFMADD132PS,
        ZYDIS_MNEMONIC_VFMADD132SD,
        ZYDIS_MNEMONIC_VFMADD132SH,
        ZYDIS_MNEMONIC_VFMADD132SS,
        ZYDIS_MNEMONIC_VFMADD213PD,
        ZYDIS_MNEMONIC_VFMADD213PH,
        ZYDIS_MNEMONIC_VFMADD213PS,
        ZYDIS_MNEMONIC_VFMADD213SD,
        ZYDIS_MNEMONIC_VFMADD213SH,
        ZYDIS_MNEMONIC_VFMADD213SS,
        ZYDIS_MNEMONIC_VFMADD231PD,
        ZYDIS_MNEMONIC_VFMADD231PH,
        ZYDIS_MNEMONIC_VFMADD231PS,
        ZYDIS_MNEMONIC_VFMADD231SD,
        ZYDIS_MNEMONIC_VFMADD231SH,
        ZYDIS_MNEMONIC_VFMADD231SS,
        ZYDIS_MNEMONIC_VFMADD233PS,
        ZYDIS_MNEMONIC_VFMADDCPH,
        ZYDIS_MNEMONIC_VFMADDCSH,
        ZYDIS_MNEMONIC_VFMADDPD,
        ZYDIS_MNEMONIC_VFMADDPS,
        ZYDIS_MNEMONIC_VFMADDSD,
        ZYDIS_MNEMONIC_VFMADDSS,
        ZYDIS_MNEMONIC_VFMADDSUB132PD,
        ZYDIS_MNEMONIC_VFMADDSUB132PH,
        ZYDIS_MNEMONIC_VFMADDSUB132PS,
        ZYDIS_MNEMONIC_VFMADDSUB213PD,
        ZYDIS_MNEMONIC_VFMADDSUB213PH,
        ZYDIS_MNEMONIC_VFMADDSUB213PS,
        ZYDIS_MNEMONIC_VFMADDSUB231PD,
        ZYDIS_MNEMONIC_VFMADDSUB231PH,
        ZYDIS_MNEMONIC_VFMADDSUB231PS,
        ZYDIS_MNEMONIC_VFMADDSUBPD,
        ZYDIS_MNEMONIC_VFMADDSUBPS,
        ZYDIS_MNEMONIC_VFMSUB132PD,
        ZYDIS_MNEMONIC_VFMSUB132PH,
        ZYDIS_MNEMONIC_VFMSUB132PS,
        ZYDIS_MNEMONIC_VFMSUB132SD,
        ZYDIS_MNEMONIC_VFMSUB132SH,
        ZYDIS_MNEMONIC_VFMSUB132SS,
        ZYDIS_MNEMONIC_VFMSUB213PD,
        ZYDIS_MNEMONIC_VFMSUB213PH,
        ZYDIS_MNEMONIC_VFMSUB213PS,
        ZYDIS_MNEMONIC_VFMSUB213SD,
        ZYDIS_MNEMONIC_VFMSUB213SH,
        ZYDIS_MNEMONIC_VFMSUB213SS,
        ZYDIS_MNEMONIC_VFMSUB231PD,
        ZYDIS_MNEMONIC_VFMSUB231PH,
        ZYDIS_MNEMONIC_VFMSUB231PS,
        ZYDIS_MNEMONIC_VFMSUB231SD,
        ZYDIS_MNEMONIC_VFMSUB231SH,
        ZYDIS_MNEMONIC_VFMSUB231SS,
        ZYDIS_MNEMONIC_VFMSUBADD132PD,
        ZYDIS_MNEMONIC_VFMSUBADD132PH,
        ZYDIS_MNEMONIC_VFMSUBADD132PS,
        ZYDIS_MNEMONIC_VFMSUBADD213PD,
        ZYDIS_MNEMONIC_VFMSUBADD213PH,
        ZYDIS_MNEMONIC_VFMSUBADD213PS,
        ZYDIS_MNEMONIC_VFMSUBADD231PD,
        ZYDIS_MNEMONIC_VFMSUBADD231PH,
        ZYDIS_MNEMONIC_VFMSUBADD231PS,
        ZYDIS_MNEMONIC_VFMSUBADDPD,
        ZYDIS_MNEMONIC_VFMSUBADDPS,
        ZYDIS_MNEMONIC_VFMSUBPD,
        ZYDIS_MNEMONIC_VFMSUBPS,
        ZYDIS_MNEMONIC_VFMSUBSD,
        ZYDIS_MNEMONIC_VFMSUBSS,
        ZYDIS_MNEMONIC_VFMULCPH,
        ZYDIS_MNEMONIC_VFMULCSH,
        ZYDIS_MNEMONIC_VFNMADD132PD,
        ZYDIS_MNEMONIC_VFNMADD132PH,
        ZYDIS_MNEMONIC_VFNMADD132PS,
        ZYDIS_MNEMONIC_VFNMADD132SD,
        ZYDIS_MNEMONIC_VFNMADD132SH,
        ZYDIS_MNEMONIC_VFNMADD132SS,
        ZYDIS_MNEMONIC_VFNMADD213PD,
        ZYDIS_MNEMONIC_VFNMADD213PH,
        ZYDIS_MNEMONIC_VFNMADD213PS,
        ZYDIS_MNEMONIC_VFNMADD213SD,
        ZYDIS_MNEMONIC_VFNMADD213SH,
        ZYDIS_MNEMONIC_VFNMADD213SS,
        ZYDIS_MNEMONIC_VFNMADD231PD,
        ZYDIS_MNEMONIC_VFNMADD231PH,
        ZYDIS_MNEMONIC_VFNMADD231PS,
        ZYDIS_MNEMONIC_VFNMADD231SD,
        ZYDIS_MNEMONIC_VFNMADD231SH,
        ZYDIS_MNEMONIC_VFNMADD231SS,
        ZYDIS_MNEMONIC_VFNMADDPD,
        ZYDIS_MNEMONIC_VFNMADDPS,
        ZYDIS_MNEMONIC_VFNMADDSD,
        ZYDIS_MNEMONIC_VFNMADDSS,
        ZYDIS_MNEMONIC_VFNMSUB132PD,
        ZYDIS_MNEMONIC_VFNMSUB132PH,
        ZYDIS_MNEMONIC_VFNMSUB132PS,
        ZYDIS_MNEMONIC_VFNMSUB132SD,
        ZYDIS_MNEMONIC_VFNMSUB132SH,
        ZYDIS_MNEMONIC_VFNMSUB132SS,
        ZYDIS_MNEMONIC_VFNMSUB213PD,
        ZYDIS_MNEMONIC_VFNMSUB213PH,
        ZYDIS_MNEMONIC_VFNMSUB213PS,
        ZYDIS_MNEMONIC_VFNMSUB213SD,
        ZYDIS_MNEMONIC_VFNMSUB213SH,
        ZYDIS_MNEMONIC_VFNMSUB213SS,
        ZYDIS_MNEMONIC_VFNMSUB231PD,
        ZYDIS_MNEMONIC_VFNMSUB231PH,
        ZYDIS_MNEMONIC_VFNMSUB231PS,
        ZYDIS_MNEMONIC_VFNMSUB231SD,
        ZYDIS_MNEMONIC_VFNMSUB231SH,
        ZYDIS_MNEMONIC_VFNMSUB231SS,
        ZYDIS_MNEMONIC_VFNMSUBPD,
        ZYDIS_MNEMONIC_VFNMSUBPS,
        ZYDIS_MNEMONIC_VFNMSUBSD,
        ZYDIS_MNEMONIC_VFNMSUBSS,
        ZYDIS_MNEMONIC_VFPCLASSPD,
        ZYDIS_MNEMONIC_VFPCLASSPH,
        ZYDIS_MNEMONIC_VFPCLASSPS,
        ZYDIS_MNEMONIC_VFPCLASSSD,
        ZYDIS_MNEMONIC_VFPCLASSSH,
        ZYDIS_MNEMONIC_VFPCLASSSS,
        ZYDIS_MNEMONIC_VFRCZPD,
        ZYDIS_MNEMONIC_VFRCZPS,
        ZYDIS_MNEMONIC_VFRCZSD,
        ZYDIS_MNEMONIC_VFRCZSS,
        ZYDIS_MNEMONIC_VGATHERDPD,
        ZYDIS_MNEMONIC_VGATHERDPS,
        ZYDIS_MNEMONIC_VGATHERPF0DPD,
        ZYDIS_MNEMONIC_VGATHERPF0DPS,
        ZYDIS_MNEMONIC_VGATHERPF0HINTDPD,
        ZYDIS_MNEMONIC_VGATHERPF0HINTDPS,
        ZYDIS_MNEMONIC_VGATHERPF0QPD,
        ZYDIS_MNEMONIC_VGATHERPF0QPS,
        ZYDIS_MNEMONIC_VGATHERPF1DPD,
        ZYDIS_MNEMONIC_VGATHERPF1DPS,
        ZYDIS_MNEMONIC_VGATHERPF1QPD,
        ZYDIS_MNEMONIC_VGATHERPF1QPS,
        ZYDIS_MNEMONIC_VGATHERQPD,
        ZYDIS_MNEMONIC_VGATHERQPS,
        ZYDIS_MNEMONIC_VGETEXPPD,
        ZYDIS_MNEMONIC_VGETEXPPH,
        ZYDIS_MNEMONIC_VGETEXPPS,
        ZYDIS_MNEMONIC_VGETEXPSD,
        ZYDIS_MNEMONIC_VGETEXPSH,
        ZYDIS_MNEMONIC_VGETEXPSS,
        ZYDIS_MNEMONIC_VGETMANTPD,
        ZYDIS_MNEMONIC_VGETMANTPH,
        ZYDIS_MNEMONIC_VGETMANTPS,
        ZYDIS_MNEMONIC_VGETMANTSD,
        ZYDIS_MNEMONIC_VGETMANTSH,
        ZYDIS_MNEMONIC_VGETMANTSS,
        ZYDIS_MNEMONIC_VGF2P8AFFINEINVQB,
        ZYDIS_MNEMONIC_VGF2P8AFFINEQB,
        ZYDIS_MNEMONIC_VGF2P8MULB,
        ZYDIS_MNEMONIC_VGMAXABSPS,
        ZYDIS_MNEMONIC_VGMAXPD,
        ZYDIS_MNEMONIC_VGMAXPS,
        ZYDIS_MNEMONIC_VGMINPD,
        ZYDIS_MNEMONIC_VGMINPS,
        ZYDIS_MNEMONIC_VHADDPD,
        ZYDIS_MNEMONIC_VHADDPS,
        ZYDIS_MNEMONIC_VHSUBPD,
        ZYDIS_MNEMONIC_VHSUBPS,
        ZYDIS_MNEMONIC_VINSERTF128,
        ZYDIS_MNEMONIC_VINSERTF32X4,
        ZYDIS_MNEMONIC_VINSERTF32X8,
        ZYDIS_MNEMONIC_VINSERTF64X2,
        ZYDIS_MNEMONIC_VINSERTF64X4,
        ZYDIS_MNEMONIC_VINSERTI128,
        ZYDIS_MNEMONIC_VINSERTI32X4,
        ZYDIS_MNEMONIC_VINSERTI32X8,
        ZYDIS_MNEMONIC_VINSERTI64X2,
        ZYDIS_MNEMONIC_VINSERTI64X4,
        ZYDIS_MNEMONIC_VINSERTPS,
        ZYDIS_MNEMONIC_VLDDQU,
        ZYDIS_MNEMONIC_VLDMXCSR,
        ZYDIS_MNEMONIC_VLOADUNPACKHD,
        ZYDIS_MNEMONIC_VLOADUNPACKHPD,
        ZYDIS_MNEMONIC_VLOADUNPACKHPS,
        ZYDIS_MNEMONIC_VLOADUNPACKHQ,
        ZYDIS_MNEMONIC_VLOADUNPACKLD,
        ZYDIS_MNEMONIC_VLOADUNPACKLPD,
        ZYDIS_MNEMONIC_VLOADUNPACKLPS,
        ZYDIS_MNEMONIC_VLOADUNPACKLQ,
        ZYDIS_MNEMONIC_VLOG2PS,
        ZYDIS_MNEMONIC_VMASKMOVDQU,
        ZYDIS_MNEMONIC_VMASKMOVPD,
        ZYDIS_MNEMONIC_VMASKMOVPS,
        ZYDIS_MNEMONIC_VMAXPD,
        ZYDIS_MNEMONIC_VMAXPH,
        ZYDIS_MNEMONIC_VMAXPS,
        ZYDIS_MNEMONIC_VMAXSD,
        ZYDIS_MNEMONIC_VMAXSH,
        ZYDIS_MNEMONIC_VMAXSS,
        ZYDIS_MNEMONIC_VMCALL,
        ZYDIS_MNEMONIC_VMCLEAR,
        ZYDIS_MNEMONIC_VMFUNC,
        ZYDIS_MNEMONIC_VMINPD,
        ZYDIS_MNEMONIC_VMINPH,
        ZYDIS_MNEMONIC_VMINPS,
        ZYDIS_MNEMONIC_VMINSD,
        ZYDIS_MNEMONIC_VMINSH,
        ZYDIS_MNEMONIC_VMINSS,
        ZYDIS_MNEMONIC_VMLAUNCH,
        ZYDIS_MNEMONIC_VMLOAD,
        ZYDIS_MNEMONIC_VMMCALL,
        ZYDIS_MNEMONIC_VMOVAPD,
        ZYDIS_MNEMONIC_VMOVAPS,
        ZYDIS_MNEMONIC_VMOVD,
        ZYDIS_MNEMONIC_VMOVDDUP,
        ZYDIS_MNEMONIC_VMOVDQA,
        ZYDIS_MNEMONIC_VMOVDQA32,
        ZYDIS_MNEMONIC_VMOVDQA64,
        ZYDIS_MNEMONIC_VMOVDQU,
        ZYDIS_MNEMONIC_VMOVDQU16,
        ZYDIS_MNEMONIC_VMOVDQU32,
        ZYDIS_MNEMONIC_VMOVDQU64,
        ZYDIS_MNEMONIC_VMOVDQU8,
        ZYDIS_MNEMONIC_VMOVHLPS,
        ZYDIS_MNEMONIC_VMOVHPD,
        ZYDIS_MNEMONIC_VMOVHPS,
        ZYDIS_MNEMONIC_VMOVLHPS,
        ZYDIS_MNEMONIC_VMOVLPD,
        ZYDIS_MNEMONIC_VMOVLPS,
        ZYDIS_MNEMONIC_VMOVMSKPD,
        ZYDIS_MNEMONIC_VMOVMSKPS,
        ZYDIS_MNEMONIC_VMOVNRAPD,
        ZYDIS_MNEMONIC_VMOVNRAPS,
        ZYDIS_MNEMONIC_VMOVNRNGOAPD,
        ZYDIS_MNEMONIC_VMOVNRNGOAPS,
        ZYDIS_MNEMONIC_VMOVNTDQ,
        ZYDIS_MNEMONIC_VMOVNTDQA,
        ZYDIS_MNEMONIC_VMOVNTPD,
        ZYDIS_MNEMONIC_VMOVNTPS,
        ZYDIS_MNEMONIC_VMOVQ,
        ZYDIS_MNEMONIC_VMOVSD,
        ZYDIS_MNEMONIC_VMOVSH,
        ZYDIS_MNEMONIC_VMOVSHDUP,
        ZYDIS_MNEMONIC_VMOVSLDUP,
        ZYDIS_MNEMONIC_VMOVSS,
        ZYDIS_MNEMONIC_VMOVUPD,
        ZYDIS_MNEMONIC_VMOVUPS,
        ZYDIS_MNEMONIC_VMOVW,
        ZYDIS_MNEMONIC_VMPSADBW,
        ZYDIS_MNEMONIC_VMPTRLD,
        ZYDIS_MNEMONIC_VMPTRST,
        ZYDIS_MNEMONIC_VMREAD,
        ZYDIS_MNEMONIC_VMRESUME,
        ZYDIS_MNEMONIC_VMRUN,
        ZYDIS_MNEMONIC_VMSAVE,
        ZYDIS_MNEMONIC_VMULPD,
        ZYDIS_MNEMONIC_VMULPH,
        ZYDIS_MNEMONIC_VMULPS,
        ZYDIS_MNEMONIC_VMULSD,
        ZYDIS_MNEMONIC_VMULSH,
        ZYDIS_MNEMONIC_VMULSS,
        ZYDIS_MNEMONIC_VMWRITE,
        ZYDIS_MNEMONIC_VMXOFF,
        ZYDIS_MNEMONIC_VMXON,
        ZYDIS_MNEMONIC_VORPD,
        ZYDIS_MNEMONIC_VORPS,
        ZYDIS_MNEMONIC_VP2INTERSECTD,
        ZYDIS_MNEMONIC_VP2INTERSECTQ,
        ZYDIS_MNEMONIC_VP4DPWSSD,
        ZYDIS_MNEMONIC_VP4DPWSSDS,
        ZYDIS_MNEMONIC_VPABSB,
        ZYDIS_MNEMONIC_VPABSD,
        ZYDIS_MNEMONIC_VPABSQ,
        ZYDIS_MNEMONIC_VPABSW,
        ZYDIS_MNEMONIC_VPACKSSDW,
        ZYDIS_MNEMONIC_VPACKSSWB,
        ZYDIS_MNEMONIC_VPACKSTOREHD,
        ZYDIS_MNEMONIC_VPACKSTOREHPD,
        ZYDIS_MNEMONIC_VPACKSTOREHPS,
        ZYDIS_MNEMONIC_VPACKSTOREHQ,
        ZYDIS_MNEMONIC_VPACKSTORELD,
        ZYDIS_MNEMONIC_VPACKSTORELPD,
        ZYDIS_MNEMONIC_VPACKSTORELPS,
        ZYDIS_MNEMONIC_VPACKSTORELQ,
        ZYDIS_MNEMONIC_VPACKUSDW,
        ZYDIS_MNEMONIC_VPACKUSWB,
        ZYDIS_MNEMONIC_VPADCD,
        ZYDIS_MNEMONIC_VPADDB,
        ZYDIS_MNEMONIC_VPADDD,
        ZYDIS_MNEMONIC_VPADDQ,
        ZYDIS_MNEMONIC_VPADDSB,
        ZYDIS_MNEMONIC_VPADDSETCD,
        ZYDIS_MNEMONIC_VPADDSETSD,
        ZYDIS_MNEMONIC_VPADDSW,
        ZYDIS_MNEMONIC_VPADDUSB,
        ZYDIS_MNEMONIC_VPADDUSW,
        ZYDIS_MNEMONIC_VPADDW,
        ZYDIS_MNEMONIC_VPALIGNR,
        ZYDIS_MNEMONIC_VPAND,
        ZYDIS_MNEMONIC_VPANDD,
        ZYDIS_MNEMONIC_VPANDN,
        ZYDIS_MNEMONIC_VPANDND,
        ZYDIS_MNEMONIC_VPANDNQ,
        ZYDIS_MNEMONIC_VPANDQ,
        ZYDIS_MNEMONIC_VPAVGB,
        ZYDIS_MNEMONIC_VPAVGW,
        ZYDIS_MNEMONIC_VPBLENDD,
        ZYDIS_MNEMONIC_VPBLENDMB,
        ZYDIS_MNEMONIC_VPBLENDMD,
        ZYDIS_MNEMONIC_VPBLENDMQ,
        ZYDIS_MNEMONIC_VPBLENDMW,
        ZYDIS_MNEMONIC_VPBLENDVB,
        ZYDIS_MNEMONIC_VPBLENDW,
        ZYDIS_MNEMONIC_VPBROADCASTB,
        ZYDIS_MNEMONIC_VPBROADCASTD,
        ZYDIS_MNEMONIC_VPBROADCASTMB2Q,
        ZYDIS_MNEMONIC_VPBROADCASTMW2D,
        ZYDIS_MNEMONIC_VPBROADCASTQ,
        ZYDIS_MNEMONIC_VPBROADCASTW,
        ZYDIS_MNEMONIC_VPCLMULQDQ,
        ZYDIS_MNEMONIC_VPCMOV,
        ZYDIS_MNEMONIC_VPCMPB,
        ZYDIS_MNEMONIC_VPCMPD,
        ZYDIS_MNEMONIC_VPCMPEQB,
        ZYDIS_MNEMONIC_VPCMPEQD,
        ZYDIS_MNEMONIC_VPCMPEQQ,
        ZYDIS_MNEMONIC_VPCMPEQW,
        ZYDIS_MNEMONIC_VPCMPESTRI,
        ZYDIS_MNEMONIC_VPCMPESTRM,
        ZYDIS_MNEMONIC_VPCMPGTB,
        ZYDIS_MNEMONIC_VPCMPGTD,
        ZYDIS_MNEMONIC_VPCMPGTQ,
        ZYDIS_MNEMONIC_VPCMPGTW,
        ZYDIS_MNEMONIC_VPCMPISTRI,
        ZYDIS_MNEMONIC_VPCMPISTRM,
        ZYDIS_MNEMONIC_VPCMPLTD,
        ZYDIS_MNEMONIC_VPCMPQ,
        ZYDIS_MNEMONIC_VPCMPUB,
        ZYDIS_MNEMONIC_VPCMPUD,
        ZYDIS_MNEMONIC_VPCMPUQ,
        ZYDIS_MNEMONIC_VPCMPUW,
        ZYDIS_MNEMONIC_VPCMPW,
        ZYDIS_MNEMONIC_VPCOMB,
        ZYDIS_MNEMONIC_VPCOMD,
        ZYDIS_MNEMONIC_VPCOMPRESSB,
        ZYDIS_MNEMONIC_VPCOMPRESSD,
        ZYDIS_MNEMONIC_VPCOMPRESSQ,
        ZYDIS_MNEMONIC_VPCOMPRESSW,
        ZYDIS_MNEMONIC_VPCOMQ,
        ZYDIS_MNEMONIC_VPCOMUB,
        ZYDIS_MNEMONIC_VPCOMUD,
        ZYDIS_MNEMONIC_VPCOMUQ,
        ZYDIS_MNEMONIC_VPCOMUW,
        ZYDIS_MNEMONIC_VPCOMW,
        ZYDIS_MNEMONIC_VPCONFLICTD,
        ZYDIS_MNEMONIC_VPCONFLICTQ,
        ZYDIS_MNEMONIC_VPDPBSSD,
        ZYDIS_MNEMONIC_VPDPBSSDS,
        ZYDIS_MNEMONIC_VPDPBSUD,
        ZYDIS_MNEMONIC_VPDPBSUDS,
        ZYDIS_MNEMONIC_VPDPBUSD,
        ZYDIS_MNEMONIC_VPDPBUSDS,
        ZYDIS_MNEMONIC_VPDPBUUD,
        ZYDIS_MNEMONIC_VPDPBUUDS,
        ZYDIS_MNEMONIC_VPDPWSSD,
        ZYDIS_MNEMONIC_VPDPWSSDS,
        ZYDIS_MNEMONIC_VPDPWSUD,
        ZYDIS_MNEMONIC_VPDPWSUDS,
        ZYDIS_MNEMONIC_VPDPWUSD,
        ZYDIS_MNEMONIC_VPDPWUSDS,
        ZYDIS_MNEMONIC_VPDPWUUD,
        ZYDIS_MNEMONIC_VPDPWUUDS,
        ZYDIS_MNEMONIC_VPERM2F128,
        ZYDIS_MNEMONIC_VPERM2I128,
        ZYDIS_MNEMONIC_VPERMB,
        ZYDIS_MNEMONIC_VPERMD,
        ZYDIS_MNEMONIC_VPERMF32X4,
        ZYDIS_MNEMONIC_VPERMI2B,
        ZYDIS_MNEMONIC_VPERMI2D,
        ZYDIS_MNEMONIC_VPERMI2PD,
        ZYDIS_MNEMONIC_VPERMI2PS,
        ZYDIS_MNEMONIC_VPERMI2Q,
        ZYDIS_MNEMONIC_VPERMI2W,
        ZYDIS_MNEMONIC_VPERMIL2PD,
        ZYDIS_MNEMONIC_VPERMIL2PS,
        ZYDIS_MNEMONIC_VPERMILPD,
        ZYDIS_MNEMONIC_VPERMILPS,
        ZYDIS_MNEMONIC_VPERMPD,
        ZYDIS_MNEMONIC_VPERMPS,
        ZYDIS_MNEMONIC_VPERMQ,
        ZYDIS_MNEMONIC_VPERMT2B,
        ZYDIS_MNEMONIC_VPERMT2D,
        ZYDIS_MNEMONIC_VPERMT2PD,
        ZYDIS_MNEMONIC_VPERMT2PS,
        ZYDIS_MNEMONIC_VPERMT2Q,
        ZYDIS_MNEMONIC_VPERMT2W,
        ZYDIS_MNEMONIC_VPERMW,
        ZYDIS_MNEMONIC_VPEXPANDB,
        ZYDIS_MNEMONIC_VPEXPANDD,
        ZYDIS_MNEMONIC_VPEXPANDQ,
        ZYDIS_MNEMONIC_VPEXPANDW,
        ZYDIS_MNEMONIC_VPEXTRB,
        ZYDIS_MNEMONIC_VPEXTRD,
        ZYDIS_MNEMONIC_VPEXTRQ,
        ZYDIS_MNEMONIC_VPEXTRW,
        ZYDIS_MNEMONIC_VPGATHERDD,
        ZYDIS_MNEMONIC_VPGATHERDQ,
        ZYDIS_MNEMONIC_VPGATHERQD,
        ZYDIS_MNEMONIC_VPGATHERQQ,
        ZYDIS_MNEMONIC_VPHADDBD,
        ZYDIS_MNEMONIC_VPHADDBQ,
        ZYDIS_MNEMONIC_VPHADDBW,
        ZYDIS_MNEMONIC_VPHADDD,
        ZYDIS_MNEMONIC_VPHADDDQ,
        ZYDIS_MNEMONIC_VPHADDSW,
        ZYDIS_MNEMONIC_VPHADDUBD,
        ZYDIS_MNEMONIC_VPHADDUBQ,
        ZYDIS_MNEMONIC_VPHADDUBW,
        ZYDIS_MNEMONIC_VPHADDUDQ,
        ZYDIS_MNEMONIC_VPHADDUWD,
        ZYDIS_MNEMONIC_VPHADDUWQ,
        ZYDIS_MNEMONIC_VPHADDW,
        ZYDIS_MNEMONIC_VPHADDWD,
        ZYDIS_MNEMONIC_VPHADDWQ,
        ZYDIS_MNEMONIC_VPHMINPOSUW,
        ZYDIS_MNEMONIC_VPHSUBBW,
        ZYDIS_MNEMONIC_VPHSUBD,
        ZYDIS_MNEMONIC_VPHSUBDQ,
        ZYDIS_MNEMONIC_VPHSUBSW,
        ZYDIS_MNEMONIC_VPHSUBW,
        ZYDIS_MNEMONIC_VPHSUBWD,
        ZYDIS_MNEMONIC_VPINSRB,
        ZYDIS_MNEMONIC_VPINSRD,
        ZYDIS_MNEMONIC_VPINSRQ,
        ZYDIS_MNEMONIC_VPINSRW,
        ZYDIS_MNEMONIC_VPLZCNTD,
        ZYDIS_MNEMONIC_VPLZCNTQ,
        ZYDIS_MNEMONIC_VPMACSDD,
        ZYDIS_MNEMONIC_VPMACSDQH,
        ZYDIS_MNEMONIC_VPMACSDQL,
        ZYDIS_MNEMONIC_VPMACSSDD,
        ZYDIS_MNEMONIC_VPMACSSDQH,
        ZYDIS_MNEMONIC_VPMACSSDQL,
        ZYDIS_MNEMONIC_VPMACSSWD,
        ZYDIS_MNEMONIC_VPMACSSWW,
        ZYDIS_MNEMONIC_VPMACSWD,
        ZYDIS_MNEMONIC_VPMACSWW,
        ZYDIS_MNEMONIC_VPMADCSSWD,
        ZYDIS_MNEMONIC_VPMADCSWD,
        ZYDIS_MNEMONIC_VPMADD231D,
        ZYDIS_MNEMONIC_VPMADD233D,
        ZYDIS_MNEMONIC_VPMADD52HUQ,
        ZYDIS_MNEMONIC_VPMADD52LUQ,
        ZYDIS_MNEMONIC_VPMADDUBSW,
        ZYDIS_MNEMONIC_VPMADDWD,
        ZYDIS_MNEMONIC_VPMASKMOVD,
        ZYDIS_MNEMONIC_VPMASKMOVQ,
        ZYDIS_MNEMONIC_VPMAXSB,
        ZYDIS_MNEMONIC_VPMAXSD,
        ZYDIS_MNEMONIC_VPMAXSQ,
        ZYDIS_MNEMONIC_VPMAXSW,
        ZYDIS_MNEMONIC_VPMAXUB,
        ZYDIS_MNEMONIC_VPMAXUD,
        ZYDIS_MNEMONIC_VPMAXUQ,
        ZYDIS_MNEMONIC_VPMAXUW,
        ZYDIS_MNEMONIC_VPMINSB,
        ZYDIS_MNEMONIC_VPMINSD,
        ZYDIS_MNEMONIC_VPMINSQ,
        ZYDIS_MNEMONIC_VPMINSW,
        ZYDIS_MNEMONIC_VPMINUB,
        ZYDIS_MNEMONIC_VPMINUD,
        ZYDIS_MNEMONIC_VPMINUQ,
        ZYDIS_MNEMONIC_VPMINUW,
        ZYDIS_MNEMONIC_VPMOVB2M,
        ZYDIS_MNEMONIC_VPMOVD2M,
        ZYDIS_MNEMONIC_VPMOVDB,
        ZYDIS_MNEMONIC_VPMOVDW,
        ZYDIS_MNEMONIC_VPMOVM2B,
        ZYDIS_MNEMONIC_VPMOVM2D,
        ZYDIS_MNEMONIC_VPMOVM2Q,
        ZYDIS_MNEMONIC_VPMOVM2W,
        ZYDIS_MNEMONIC_VPMOVMSKB,
        ZYDIS_MNEMONIC_VPMOVQ2M,
        ZYDIS_MNEMONIC_VPMOVQB,
        ZYDIS_MNEMONIC_VPMOVQD,
        ZYDIS_MNEMONIC_VPMOVQW,
        ZYDIS_MNEMONIC_VPMOVSDB,
        ZYDIS_MNEMONIC_VPMOVSDW,
        ZYDIS_MNEMONIC_VPMOVSQB,
        ZYDIS_MNEMONIC_VPMOVSQD,
        ZYDIS_MNEMONIC_VPMOVSQW,
        ZYDIS_MNEMONIC_VPMOVSWB,
        ZYDIS_MNEMONIC_VPMOVSXBD,
        ZYDIS_MNEMONIC_VPMOVSXBQ,
        ZYDIS_MNEMONIC_VPMOVSXBW,
        ZYDIS_MNEMONIC_VPMOVSXDQ,
        ZYDIS_MNEMONIC_VPMOVSXWD,
        ZYDIS_MNEMONIC_VPMOVSXWQ,
        ZYDIS_MNEMONIC_VPMOVUSDB,
        ZYDIS_MNEMONIC_VPMOVUSDW,
        ZYDIS_MNEMONIC_VPMOVUSQB,
        ZYDIS_MNEMONIC_VPMOVUSQD,
        ZYDIS_MNEMONIC_VPMOVUSQW,
        ZYDIS_MNEMONIC_VPMOVUSWB,
        ZYDIS_MNEMONIC_VPMOVW2M,
        ZYDIS_MNEMONIC_VPMOVWB,
        ZYDIS_MNEMONIC_VPMOVZXBD,
        ZYDIS_MNEMONIC_VPMOVZXBQ,
        ZYDIS_MNEMONIC_VPMOVZXBW,
        ZYDIS_MNEMONIC_VPMOVZXDQ,
        ZYDIS_MNEMONIC_VPMOVZXWD,
        ZYDIS_MNEMONIC_VPMOVZXWQ,
        ZYDIS_MNEMONIC_VPMULDQ,
        ZYDIS_MNEMONIC_VPMULHD,
        ZYDIS_MNEMONIC_VPMULHRSW,
        ZYDIS_MNEMONIC_VPMULHUD,
        ZYDIS_MNEMONIC_VPMULHUW,
        ZYDIS_MNEMONIC_VPMULHW,
        ZYDIS_MNEMONIC_VPMULLD,
        ZYDIS_MNEMONIC_VPMULLQ,
        ZYDIS_MNEMONIC_VPMULLW,
        ZYDIS_MNEMONIC_VPMULTISHIFTQB,
        ZYDIS_MNEMONIC_VPMULUDQ,
        ZYDIS_MNEMONIC_VPOPCNTB,
        ZYDIS_MNEMONIC_VPOPCNTD,
        ZYDIS_MNEMONIC_VPOPCNTQ,
        ZYDIS_MNEMONIC_VPOPCNTW,
        ZYDIS_MNEMONIC_VPOR,
        ZYDIS_MNEMONIC_VPORD,
        ZYDIS_MNEMONIC_VPORQ,
        ZYDIS_MNEMONIC_VPPERM,
        ZYDIS_MNEMONIC_VPREFETCH0,
        ZYDIS_MNEMONIC_VPREFETCH1,
        ZYDIS_MNEMONIC_VPREFETCH2,
        ZYDIS_MNEMONIC_VPREFETCHE0,
        ZYDIS_MNEMONIC_VPREFETCHE1,
        ZYDIS_MNEMONIC_VPREFETCHE2,
        ZYDIS_MNEMONIC_VPREFETCHENTA,
        ZYDIS_MNEMONIC_VPREFETCHNTA,
        ZYDIS_MNEMONIC_VPROLD,
        ZYDIS_MNEMONIC_VPROLQ,
        ZYDIS_MNEMONIC_VPROLVD,
        ZYDIS_MNEMONIC_VPROLVQ,
        ZYDIS_MNEMONIC_VPRORD,
        ZYDIS_MNEMONIC_VPRORQ,
        ZYDIS_MNEMONIC_VPRORVD,
        ZYDIS_MNEMONIC_VPRORVQ,
        ZYDIS_MNEMONIC_VPROTB,
        ZYDIS_MNEMONIC_VPROTD,
        ZYDIS_MNEMONIC_VPROTQ,
        ZYDIS_MNEMONIC_VPROTW,
        ZYDIS_MNEMONIC_VPSADBW,
        ZYDIS_MNEMONIC_VPSBBD,
        ZYDIS_MNEMONIC_VPSBBRD,
        ZYDIS_MNEMONIC_VPSCATTERDD,
        ZYDIS_MNEMONIC_VPSCATTERDQ,
        ZYDIS_MNEMONIC_VPSCATTERQD,
        ZYDIS_MNEMONIC_VPSCATTERQQ,
        ZYDIS_MNEMONIC_VPSHAB,
        ZYDIS_MNEMONIC_VPSHAD,
        ZYDIS_MNEMONIC_VPSHAQ,
        ZYDIS_MNEMONIC_VPSHAW,
        ZYDIS_MNEMONIC_VPSHLB,
        ZYDIS_MNEMONIC_VPSHLD,
        ZYDIS_MNEMONIC_VPSHLDD,
        ZYDIS_MNEMONIC_VPSHLDQ,
        ZYDIS_MNEMONIC_VPSHLDVD,
        ZYDIS_MNEMONIC_VPSHLDVQ,
        ZYDIS_MNEMONIC_VPSHLDVW,
        ZYDIS_MNEMONIC_VPSHLDW,
        ZYDIS_MNEMONIC_VPSHLQ,
        ZYDIS_MNEMONIC_VPSHLW,
        ZYDIS_MNEMONIC_VPSHRDD,
        ZYDIS_MNEMONIC_VPSHRDQ,
        ZYDIS_MNEMONIC_VPSHRDVD,
        ZYDIS_MNEMONIC_VPSHRDVQ,
        ZYDIS_MNEMONIC_VPSHRDVW,
        ZYDIS_MNEMONIC_VPSHRDW,
        ZYDIS_MNEMONIC_VPSHUFB,
        ZYDIS_MNEMONIC_VPSHUFBITQMB,
        ZYDIS_MNEMONIC_VPSHUFD,
        ZYDIS_MNEMONIC_VPSHUFHW,
        ZYDIS_MNEMONIC_VPSHUFLW,
        ZYDIS_MNEMONIC_VPSIGNB,
        ZYDIS_MNEMONIC_VPSIGND,
        ZYDIS_MNEMONIC_VPSIGNW,
        ZYDIS_MNEMONIC_VPSLLD,
        ZYDIS_MNEMONIC_VPSLLDQ,
        ZYDIS_MNEMONIC_VPSLLQ,
        ZYDIS_MNEMONIC_VPSLLVD,
        ZYDIS_MNEMONIC_VPSLLVQ,
        ZYDIS_MNEMONIC_VPSLLVW,
        ZYDIS_MNEMONIC_VPSLLW,
        ZYDIS_MNEMONIC_VPSRAD,
        ZYDIS_MNEMONIC_VPSRAQ,
        ZYDIS_MNEMONIC_VPSRAVD,
        ZYDIS_MNEMONIC_VPSRAVQ,
        ZYDIS_MNEMONIC_VPSRAVW,
        ZYDIS_MNEMONIC_VPSRAW,
        ZYDIS_MNEMONIC_VPSRLD,
        ZYDIS_MNEMONIC_VPSRLDQ,
        ZYDIS_MNEMONIC_VPSRLQ,
        ZYDIS_MNEMONIC_VPSRLVD,
        ZYDIS_MNEMONIC_VPSRLVQ,
        ZYDIS_MNEMONIC_VPSRLVW,
        ZYDIS_MNEMONIC_VPSRLW,
        ZYDIS_MNEMONIC_VPSUBB,
        ZYDIS_MNEMONIC_VPSUBD,
        ZYDIS_MNEMONIC_VPSUBQ,
        ZYDIS_MNEMONIC_VPSUBRD,
        ZYDIS_MNEMONIC_VPSUBRSETBD,
        ZYDIS_MNEMONIC_VPSUBSB,
        ZYDIS_MNEMONIC_VPSUBSETBD,
        ZYDIS_MNEMONIC_VPSUBSW,
        ZYDIS_MNEMONIC_VPSUBUSB,
        ZYDIS_MNEMONIC_VPSUBUSW,
        ZYDIS_MNEMONIC_VPSUBW,
        ZYDIS_MNEMONIC_VPTERNLOGD,
        ZYDIS_MNEMONIC_VPTERNLOGQ,
        ZYDIS_MNEMONIC_VPTEST,
        ZYDIS_MNEMONIC_VPTESTMB,
        ZYDIS_MNEMONIC_VPTESTMD,
        ZYDIS_MNEMONIC_VPTESTMQ,
        ZYDIS_MNEMONIC_VPTESTMW,
        ZYDIS_MNEMONIC_VPTESTNMB,
        ZYDIS_MNEMONIC_VPTESTNMD,
        ZYDIS_MNEMONIC_VPTESTNMQ,
        ZYDIS_MNEMONIC_VPTESTNMW,
        ZYDIS_MNEMONIC_VPUNPCKHBW,
        ZYDIS_MNEMONIC_VPUNPCKHDQ,
        ZYDIS_MNEMONIC_VPUNPCKHQDQ,
        ZYDIS_MNEMONIC_VPUNPCKHWD,
        ZYDIS_MNEMONIC_VPUNPCKLBW,
        ZYDIS_MNEMONIC_VPUNPCKLDQ,
        ZYDIS_MNEMONIC_VPUNPCKLQDQ,
        ZYDIS_MNEMONIC_VPUNPCKLWD,
        ZYDIS_MNEMONIC_VPXOR,
        ZYDIS_MNEMONIC_VPXORD,
        ZYDIS_MNEMONIC_VPXORQ,
        ZYDIS_MNEMONIC_VRANGEPD,
        ZYDIS_MNEMONIC_VRANGEPS,
        ZYDIS_MNEMONIC_VRANGESD,
        ZYDIS_MNEMONIC_VRANGESS,
        ZYDIS_MNEMONIC_VRCP14PD,
        ZYDIS_MNEMONIC_VRCP14PS,
        ZYDIS_MNEMONIC_VRCP14SD,
        ZYDIS_MNEMONIC_VRCP14SS,
        ZYDIS_MNEMONIC_VRCP23PS,
        ZYDIS_MNEMONIC_VRCP28PD,
        ZYDIS_MNEMONIC_VRCP28PS,
        ZYDIS_MNEMONIC_VRCP28SD,
        ZYDIS_MNEMONIC_VRCP28SS,
        ZYDIS_MNEMONIC_VRCPPH,
        ZYDIS_MNEMONIC_VRCPPS,
        ZYDIS_MNEMONIC_VRCPSH,
        ZYDIS_MNEMONIC_VRCPSS,
        ZYDIS_MNEMONIC_VREDUCEPD,
        ZYDIS_MNEMONIC_VREDUCEPH,
        ZYDIS_MNEMONIC_VREDUCEPS,
        ZYDIS_MNEMONIC_VREDUCESD,
        ZYDIS_MNEMONIC_VREDUCESH,
        ZYDIS_MNEMONIC_VREDUCESS,
        ZYDIS_MNEMONIC_VRNDFXPNTPD,
        ZYDIS_MNEMONIC_VRNDFXPNTPS,
        ZYDIS_MNEMONIC_VRNDSCALEPD,
        ZYDIS_MNEMONIC_VRNDSCALEPH,
        ZYDIS_MNEMONIC_VRNDSCALEPS,
        ZYDIS_MNEMONIC_VRNDSCALESD,
        ZYDIS_MNEMONIC_VRNDSCALESH,
        ZYDIS_MNEMONIC_VRNDSCALESS,
        ZYDIS_MNEMONIC_VROUNDPD,
        ZYDIS_MNEMONIC_VROUNDPS,
        ZYDIS_MNEMONIC_VROUNDSD,
        ZYDIS_MNEMONIC_VROUNDSS,
        ZYDIS_MNEMONIC_VRSQRT14PD,
        ZYDIS_MNEMONIC_VRSQRT14PS,
        ZYDIS_MNEMONIC_VRSQRT14SD,
        ZYDIS_MNEMONIC_VRSQRT14SS,
        ZYDIS_MNEMONIC_VRSQRT23PS,
        ZYDIS_MNEMONIC_VRSQRT28PD,
        ZYDIS_MNEMONIC_VRSQRT28PS,
        ZYDIS_MNEMONIC_VRSQRT28SD,
        ZYDIS_MNEMONIC_VRSQRT28SS,
        ZYDIS_MNEMONIC_VRSQRTPH,
        ZYDIS_MNEMONIC_VRSQRTPS,
        ZYDIS_MNEMONIC_VRSQRTSH,
        ZYDIS_MNEMONIC_VRSQRTSS,
        ZYDIS_MNEMONIC_VSCALEFPD,
        ZYDIS_MNEMONIC_VSCALEFPH,
        ZYDIS_MNEMONIC_VSCALEFPS,
        ZYDIS_MNEMONIC_VSCALEFSD,
        ZYDIS_MNEMONIC_VSCALEFSH,
        ZYDIS_MNEMONIC_VSCALEFSS,
        ZYDIS_MNEMONIC_VSCALEPS,
        ZYDIS_MNEMONIC_VSCATTERDPD,
        ZYDIS_MNEMONIC_VSCATTERDPS,
        ZYDIS_MNEMONIC_VSCATTERPF0DPD,
        ZYDIS_MNEMONIC_VSCATTERPF0DPS,
        ZYDIS_MNEMONIC_VSCATTERPF0HINTDPD,
        ZYDIS_MNEMONIC_VSCATTERPF0HINTDPS,
        ZYDIS_MNEMONIC_VSCATTERPF0QPD,
        ZYDIS_MNEMONIC_VSCATTERPF0QPS,
        ZYDIS_MNEMONIC_VSCATTERPF1DPD,
        ZYDIS_MNEMONIC_VSCATTERPF1DPS,
        ZYDIS_MNEMONIC_VSCATTERPF1QPD,
        ZYDIS_MNEMONIC_VSCATTERPF1QPS,
        ZYDIS_MNEMONIC_VSCATTERQPD,
        ZYDIS_MNEMONIC_VSCATTERQPS,
        ZYDIS_MNEMONIC_VSHA512MSG1,
        ZYDIS_MNEMONIC_VSHA512MSG2,
        ZYDIS_MNEMONIC_VSHA512RNDS2,
        ZYDIS_MNEMONIC_VSHUFF32X4,
        ZYDIS_MNEMONIC_VSHUFF64X2,
        ZYDIS_MNEMONIC_VSHUFI32X4,
        ZYDIS_MNEMONIC_VSHUFI64X2,
        ZYDIS_MNEMONIC_VSHUFPD,
        ZYDIS_MNEMONIC_VSHUFPS,
        ZYDIS_MNEMONIC_VSM3MSG1,
        ZYDIS_MNEMONIC_VSM3MSG2,
        ZYDIS_MNEMONIC_VSM3RNDS2,
        ZYDIS_MNEMONIC_VSM4KEY4,
        ZYDIS_MNEMONIC_VSM4RNDS4,
        ZYDIS_MNEMONIC_VSQRTPD,
        ZYDIS_MNEMONIC_VSQRTPH,
        ZYDIS_MNEMONIC_VSQRTPS,
        ZYDIS_MNEMONIC_VSQRTSD,
        ZYDIS_MNEMONIC_VSQRTSH,
        ZYDIS_MNEMONIC_VSQRTSS,
        ZYDIS_MNEMONIC_VSTMXCSR,
        ZYDIS_MNEMONIC_VSUBPD,
        ZYDIS_MNEMONIC_VSUBPH,
        ZYDIS_MNEMONIC_VSUBPS,
        ZYDIS_MNEMONIC_VSUBRPD,
        ZYDIS_MNEMONIC_VSUBRPS,
        ZYDIS_MNEMONIC_VSUBSD,
        ZYDIS_MNEMONIC_VSUBSH,
        ZYDIS_MNEMONIC_VSUBSS,
        ZYDIS_MNEMONIC_VTESTPD,
        ZYDIS_MNEMONIC_VTESTPS,
        ZYDIS_MNEMONIC_VUCOMISD,
        ZYDIS_MNEMONIC_VUCOMISH,
        ZYDIS_MNEMONIC_VUCOMISS,
        ZYDIS_MNEMONIC_VUNPCKHPD,
        ZYDIS_MNEMONIC_VUNPCKHPS,
        ZYDIS_MNEMONIC_VUNPCKLPD,
        ZYDIS_MNEMONIC_VUNPCKLPS,
        ZYDIS_MNEMONIC_VXORPD,
        ZYDIS_MNEMONIC_VXORPS,
        ZYDIS_MNEMONIC_VZEROALL,
        ZYDIS_MNEMONIC_VZEROUPPER,
        ZYDIS_MNEMONIC_WBINVD,
        ZYDIS_MNEMONIC_WRFSBASE,
        ZYDIS_MNEMONIC_WRGSBASE,
        ZYDIS_MNEMONIC_WRMSR,
        ZYDIS_MNEMONIC_WRMSRLIST,
        ZYDIS_MNEMONIC_WRMSRNS,
        ZYDIS_MNEMONIC_WRPKRU,
        ZYDIS_MNEMONIC_WRSSD,
        ZYDIS_MNEMONIC_WRSSQ,
        ZYDIS_MNEMONIC_WRUSSD,
        ZYDIS_MNEMONIC_WRUSSQ,
        ZYDIS_MNEMONIC_XABORT,
        ZYDIS_MNEMONIC_XADD,
        ZYDIS_MNEMONIC_XBEGIN,
        ZYDIS_MNEMONIC_XCHG,
        ZYDIS_MNEMONIC_XCRYPT_CBC,
        ZYDIS_MNEMONIC_XCRYPT_CFB,
        ZYDIS_MNEMONIC_XCRYPT_CTR,
        ZYDIS_MNEMONIC_XCRYPT_ECB,
        ZYDIS_MNEMONIC_XCRYPT_OFB,
        ZYDIS_MNEMONIC_XEND,
        ZYDIS_MNEMONIC_XLAT,
        ZYDIS_MNEMONIC_XOR,
        ZYDIS_MNEMONIC_XORPD,
        ZYDIS_MNEMONIC_XORPS,
        ZYDIS_MNEMONIC_XRESLDTRK,
        ZYDIS_MNEMONIC_XSETBV,
        ZYDIS_MNEMONIC_XSHA1,
        ZYDIS_MNEMONIC_XSHA256,
        ZYDIS_MNEMONIC_XSTORE,
        ZYDIS_MNEMONIC_XSUSLDTRK,
        ZYDIS_MNEMONIC_BSF,
        ZYDIS_MNEMONIC_BSR,
        ZYDIS_MNEMONIC_ANDN,
        ZYDIS_MNEMONIC_ANDNPD,
        ZYDIS_MNEMONIC_ANDNPS,
        ZYDIS_MNEMONIC_ANDPD,
        ZYDIS_MNEMONIC_ANDPS,
        ZYDIS_MNEMONIC_BEXTR,
        // ZYDIS_MNEMONIC_CRC32,
        // ZYDIS_MNEMONIC_STOSB,
        // ZYDIS_MNEMONIC_STOSD,
        // ZYDIS_MNEMONIC_STOSQ,
        // ZYDIS_MNEMONIC_STOSW,
        // ZYDIS_MNEMONIC_SYSCALL,
        // ZYDIS_MNEMONIC_SYSENTER,
        // ZYDIS_MNEMONIC_SYSEXIT,
        // ZYDIS_MNEMONIC_SYSRET,
        // ZYDIS_MNEMONIC_SEAMCALL,
        // ZYDIS_MNEMONIC_SEAMOPS,
        // ZYDIS_MNEMONIC_SEAMRET,
        // ZYDIS_MNEMONIC_HLT,
        // ZYDIS_MNEMONIC_LEAVE,
        // ZYDIS_MNEMONIC_LODSB,
        // ZYDIS_MNEMONIC_LODSD,
        // ZYDIS_MNEMONIC_LODSQ,
        // ZYDIS_MNEMONIC_LODSW,
        // ZYDIS_MNEMONIC_LOOP,
        // ZYDIS_MNEMONIC_LOOPE,
        // ZYDIS_MNEMONIC_LOOPNE,
        // ZYDIS_MNEMONIC_XGETBV,
        // ZYDIS_MNEMONIC_POP,
        // ZYDIS_MNEMONIC_POPA,
        // ZYDIS_MNEMONIC_POPAD,
        // ZYDIS_MNEMONIC_POPCNT,
        // ZYDIS_MNEMONIC_POPF,
        // ZYDIS_MNEMONIC_POPFD,
        // ZYDIS_MNEMONIC_POPFQ,
        // ZYDIS_MNEMONIC_CALL,
        // ZYDIS_MNEMONIC_PUSH,
        // ZYDIS_MNEMONIC_PUSHA,
        // ZYDIS_MNEMONIC_PUSHAD,
        // ZYDIS_MNEMONIC_PUSHF,
        // ZYDIS_MNEMONIC_PUSHFD,
        // ZYDIS_MNEMONIC_PUSHFQ,
        // ZYDIS_MNEMONIC_RET,
        // ZYDIS_MNEMONIC_OUT,
        // ZYDIS_MNEMONIC_OUTSB,
        // ZYDIS_MNEMONIC_OUTSD,
        // ZYDIS_MNEMONIC_OUTSW,
        // ZYDIS_MNEMONIC_XRSTOR,
        // ZYDIS_MNEMONIC_XRSTOR64,
        // ZYDIS_MNEMONIC_XRSTORS,
        // ZYDIS_MNEMONIC_XRSTORS64,
        // ZYDIS_MNEMONIC_XSAVE,
        // ZYDIS_MNEMONIC_XSAVE64,
        // ZYDIS_MNEMONIC_XSAVEC,
        // ZYDIS_MNEMONIC_XSAVEC64,
        // ZYDIS_MNEMONIC_XSAVEOPT,
        // ZYDIS_MNEMONIC_XSAVEOPT64,
        // ZYDIS_MNEMONIC_XSAVES,
        // ZYDIS_MNEMONIC_XSAVES64,
        // ZYDIS_MNEMONIC_UD0,
        // ZYDIS_MNEMONIC_UD1,
        // ZYDIS_MNEMONIC_UD2,
        // ZYDIS_MNEMONIC_IN,
        // ZYDIS_MNEMONIC_INSB,
        // ZYDIS_MNEMONIC_INSD,
        // ZYDIS_MNEMONIC_INSW,
        // ZYDIS_MNEMONIC_IRET,
        // ZYDIS_MNEMONIC_IRETD,
        // ZYDIS_MNEMONIC_IRETQ,
        // ZYDIS_MNEMONIC_JB,
        // ZYDIS_MNEMONIC_JBE,
        // ZYDIS_MNEMONIC_JCXZ,
        // ZYDIS_MNEMONIC_JECXZ,
        // ZYDIS_MNEMONIC_JKNZD,
        // ZYDIS_MNEMONIC_JKZD,
        // ZYDIS_MNEMONIC_JL,
        // ZYDIS_MNEMONIC_JLE,
        // ZYDIS_MNEMONIC_JMP,
        // ZYDIS_MNEMONIC_JNB,
        // ZYDIS_MNEMONIC_JNBE,
        // ZYDIS_MNEMONIC_JNL,
        // ZYDIS_MNEMONIC_JNLE,
        // ZYDIS_MNEMONIC_JNO,
        // ZYDIS_MNEMONIC_JNP,
        // ZYDIS_MNEMONIC_JNS,
        // ZYDIS_MNEMONIC_JNZ,
        // ZYDIS_MNEMONIC_JO,
        // ZYDIS_MNEMONIC_JP,
        // ZYDIS_MNEMONIC_JRCXZ,
        // ZYDIS_MNEMONIC_JS,
        // ZYDIS_MNEMONIC_JZ,
        ZYDIS_MNEMONIC_XTEST,
    };

    const auto mode = ZydisMachineMode::ZYDIS_MACHINE_MODE_LONG_64;

#ifdef _DEBUG
    generateInstrTests(mode, ZYDIS_MNEMONIC_SHL);
#else
    for (auto mnemonic : mnemonics)
    {
        generateInstrTests(mode, mnemonic);
    }
#endif

    return EXIT_SUCCESS;
}
