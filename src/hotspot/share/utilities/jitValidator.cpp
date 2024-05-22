#include "utilities/jitValidator.hpp"

#include "code/codeCache.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "utilities/Zydis.h"
#include "utilities/debug.hpp"
#include "utilities/exceptions.hpp"

void JitValidator::run() {
  JitValidator jitValidator;
  jitValidator.analyze();
}

JitValidator::JitValidator()
    : _decoder(), _formatter(), _address_locator(), _log_level(OFF) {
  ZydisDecoderInit(&_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&_formatter, ZYDIS_FORMATTER_STYLE_ATT);

  for (int i = 0; i < mt_number_of_types; i++) {
    _address_count[i] = 0;
  }
  _unrecognizable_address_count = 0;
  _nmethod_count = 0;

  if (strcmp(JitValidatorLogLevel, "off") == 0) {
    _log_level = OFF;
  } else if (strcmp(JitValidatorLogLevel, "summary") == 0) {
    _log_level = SUMMARY;
  } else if (strcmp(JitValidatorLogLevel, "detail") == 0) {
    _log_level = DETAIL;
  } else {
    assert(false, "Unknown JitValidatorLogLevel");
  }
}

void JitValidator::do_code_blob(CodeBlob *cb) {
  if (cb->is_nmethod()) {
    _nmethod_count++;
    analyze_nmethod((nmethod *) cb);
  }
}

void JitValidator::analyze() {
  JIT_VALIDATOR_DETAIL_LOG("Starting JitValidator\n");
  MutexLockerEx mu(CodeCache_lock, Mutex::_no_safepoint_check_flag);
  CodeCache::blobs_do(this);
  print_statistics();
}

void JitValidator::analyze_nmethod(nmethod *nm) {
  JIT_VALIDATOR_DETAIL_LOG("Analyzing %s\n", nm->method()->name_and_sig_as_C_string());

  ZyanU64 runtimeAddress = p2i(nm->code_begin());
  ZyanUSize offset = 0;
  const ZyanUSize length = nm->code_size();
  ZydisDecodedInstruction instruction;
  ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

  while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&_decoder, nm->code_begin() + offset, length - offset,
                                             &instruction, operands))) {
    // Check each operand
    for (ZyanU8 i = 0; i < instruction.operand_count_visible; i++) {
      OperandValue result = {OperandValue::INVALID, 0};
      ZydisDecodedOperand *operand = &operands[i];
      switch (operand->type) {
        case ZYDIS_OPERAND_TYPE_MEMORY:
          result = get_mem_operand(&instruction, operand, runtimeAddress);
          break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
          result = get_imm_operand(&instruction, operand, runtimeAddress);
          break;
        default:
          break;
      }

      // Only positive value can be an address
      if (result.type == OperandValue::UNSIGNED || result.type == OperandValue::SIGNED && result.value.s > 0) {
        // Filter out small value
        if (result.value.u >= (1 << 20)) {
          if (_log_level == DETAIL) {
            print_instruction(&instruction, operands, runtimeAddress);
          }

          handle_value(result);
        }
      }
    }

    offset += instruction.length;
    runtimeAddress += instruction.length;
  }
}

void JitValidator::print_instruction(const ZydisDecodedInstruction *instruction,
                                     const ZydisDecodedOperand operands[],
                                     ZyanU64 runtimeAddress) {
  // Print current instruction pointer.
  tty->print("%016" PRIX64 "  ", runtimeAddress);

  // Format & print the binary instruction structure in human-readable format
  char buffer[256];
  ZydisFormatterFormatInstruction(&_formatter, instruction, operands,
                                  instruction->operand_count_visible, buffer, sizeof(buffer),
                                  runtimeAddress, ZYAN_NULL);
  tty->print_raw(buffer);
}

JitValidator::OperandValue JitValidator::get_imm_operand(const ZydisDecodedInstruction *instruction,
                                                         const ZydisDecodedOperand *operand,
                                                         ZyanU64 runtimeAddress) {
  OperandValue result = {OperandValue::INVALID, 0};
  const ZydisDecodedOperandImm *imm = &operand->imm;
  if (imm->is_relative) {
    result = calculate_absolute_address(instruction, operand, runtimeAddress);
  } else if (imm->is_signed) {
    result.value.s = imm->value.s;
    result.type = OperandValue::SIGNED;
  } else {
    result.value.u = imm->value.u;
    result.type = OperandValue::UNSIGNED;
  }
  return result;
}

JitValidator::OperandValue JitValidator::get_mem_operand(const ZydisDecodedInstruction *instruction,
                                                         const ZydisDecodedOperand *operand,
                                                         ZyanU64 runtimeAddress) {
  OperandValue result = {OperandValue::INVALID, 0};
  const ZydisDecodedOperandMem *mem = &operand->mem;
  // 1. Check RIP-relative address
  ZydisRegister base = mem->base;
  if (base == ZYDIS_REGISTER_RIP || base == ZYDIS_REGISTER_EIP || base == ZYDIS_REGISTER_IP) {
    result = calculate_absolute_address(instruction, operand, runtimeAddress);
  } else if (mem->disp.has_displacement) {
    // 2. check displacement
    // If memory operand does not contain base and index, it must be an absolute address.
    if (mem->base == ZYDIS_REGISTER_NONE && mem->index == ZYDIS_REGISTER_NONE) {
      result = calculate_absolute_address(instruction, operand, runtimeAddress);
    } else {
      result.value.s = mem->disp.value;
      result.type = OperandValue::SIGNED;
    }
  }
  return result;
}

JitValidator::OperandValue JitValidator::calculate_absolute_address(const ZydisDecodedInstruction *instruction,
                                                                    const ZydisDecodedOperand *operand,
                                                                    ZyanU64 runtimeAddress) {
  OperandValue result = {OperandValue::UNSIGNED, 0};
  assert(ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(instruction, operand, runtimeAddress, &result.value.u)),
         "Failed to calculate absolute address value");
  return result;
}

void JitValidator::handle_value(OperandValue value) {
  assert(value.type == OperandValue::UNSIGNED || value.type == OperandValue::SIGNED && value.value.s > 0,
         "Handling non-positive value");

  address p = (address) value.value.u;
  MEMFLAGS flag;
  if (_address_locator.get_flag(p, flag)) {
    JIT_VALIDATOR_DETAIL_LOG(" : %p has memory flag \"%s\"\n", p, NMTUtil::flag_to_name(flag));
    _address_count[flag]++;
    // TODO: validate address
  } else {
    JIT_VALIDATOR_DETAIL_LOG(" : Failed to get memory flag of %p\n", p);
    _unrecognizable_address_count++;
  }
}

void JitValidator::print_statistics() {
  if (_log_level == OFF) {
    return;
  }
  long long sum = 0;
  tty->print("\n=====================================================\n");
  tty->print("JIT validator statistics:\n");
  for (int i = 0; i < mt_number_of_types; i++) {
    if (_address_count[i] != 0) {
      tty->print("%s : %d\n", NMTUtil::flag_to_name(NMTUtil::index_to_flag(i)), _address_count[i]);
      sum += _address_count[i];
    }
  }
  tty->print("Unrecognizable address : %d\n", _unrecognizable_address_count);
  sum += _unrecognizable_address_count;
  tty->print("Total address: %lld\n", sum);
  tty->print("Total nmethod: %d\n", _nmethod_count);
  tty->print("=====================================================\n");
}