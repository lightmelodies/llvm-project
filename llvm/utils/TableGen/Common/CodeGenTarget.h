//===- CodeGenTarget.h - Target Class Wrapper -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines wrappers for the Target class and related global
// functionality.  This makes it easier to access the data and provides a single
// place that needs to check it for validity.  All of these classes abort
// on error conditions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_UTILS_TABLEGEN_COMMON_CODEGENTARGET_H
#define LLVM_UTILS_TABLEGEN_COMMON_CODEGENTARGET_H

#include "Basic/CodeGenIntrinsics.h"
#include "Basic/SDNodeProperties.h"
#include "CodeGenHwModes.h"
#include "CodeGenInstruction.h"
#include "InfoByHwMode.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGenTypes/MachineValueType.h"
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {

class RecordKeeper;
class Record;
class CodeGenRegBank;
class CodeGenRegister;
class CodeGenRegisterClass;
class CodeGenSchedModels;
class CodeGenSubRegIndex;

/// getValueType - Return the MVT::SimpleValueType that the specified TableGen
/// record corresponds to.
MVT::SimpleValueType getValueType(const Record *Rec);

StringRef getEnumName(MVT::SimpleValueType T);

/// getQualifiedName - Return the name of the specified record, with a
/// namespace qualifier if the record contains one.
std::string getQualifiedName(const Record *R);

/// CodeGenTarget - This class corresponds to the Target class in the .td files.
///
class CodeGenTarget {
  const RecordKeeper &Records;
  const Record *TargetRec;

  mutable DenseMap<const Record *, std::unique_ptr<CodeGenInstruction>>
      InstructionMap;
  mutable std::unique_ptr<CodeGenRegBank> RegBank;
  mutable ArrayRef<const Record *> RegAltNameIndices;
  mutable SmallVector<ValueTypeByHwMode, 8> LegalValueTypes;
  CodeGenHwModes CGH;
  ArrayRef<const Record *> MacroFusions;
  mutable bool HasVariableLengthEncodings = false;

  void ReadInstructions() const;
  void ReadLegalValueTypes() const;

  mutable std::unique_ptr<CodeGenSchedModels> SchedModels;

  mutable StringRef InstNamespace;
  mutable std::vector<const CodeGenInstruction *> InstrsByEnum;
  mutable CodeGenIntrinsicMap Intrinsics;

  mutable unsigned NumPseudoInstructions = 0;

public:
  CodeGenTarget(const RecordKeeper &Records);
  ~CodeGenTarget();

  const Record *getTargetRecord() const { return TargetRec; }
  StringRef getName() const;

  /// getInstNamespace - Return the target-specific instruction namespace.
  ///
  StringRef getInstNamespace() const;

  /// getRegNamespace - Return the target-specific register namespace.
  StringRef getRegNamespace() const;

  /// getInstructionSet - Return the InstructionSet object.
  ///
  const Record *getInstructionSet() const;

  /// getAllowRegisterRenaming - Return the AllowRegisterRenaming flag value for
  /// this target.
  ///
  bool getAllowRegisterRenaming() const;

  /// getAsmParser - Return the AssemblyParser definition for this target.
  ///
  const Record *getAsmParser() const;

  /// getAsmParserVariant - Return the AssemblyParserVariant definition for
  /// this target.
  ///
  const Record *getAsmParserVariant(unsigned i) const;

  /// getAsmParserVariantCount - Return the AssemblyParserVariant definition
  /// available for this target.
  ///
  unsigned getAsmParserVariantCount() const;

  /// getAsmWriter - Return the AssemblyWriter definition for this target.
  ///
  const Record *getAsmWriter() const;

  /// getRegBank - Return the register bank description.
  CodeGenRegBank &getRegBank() const;

  /// getRegisterByName - If there is a register with the specific AsmName,
  /// return it.
  const CodeGenRegister *getRegisterByName(StringRef Name) const;

  ArrayRef<const Record *> getRegAltNameIndices() const {
    if (RegAltNameIndices.empty())
      RegAltNameIndices = Records.getAllDerivedDefinitions("RegAltNameIndex");
    return RegAltNameIndices;
  }

  const CodeGenRegisterClass &getRegisterClass(const Record *R) const;

  /// getRegisterVTs - Find the union of all possible SimpleValueTypes for the
  /// specified physical register.
  std::vector<ValueTypeByHwMode> getRegisterVTs(const Record *R) const;

  ArrayRef<ValueTypeByHwMode> getLegalValueTypes() const {
    if (LegalValueTypes.empty())
      ReadLegalValueTypes();
    return LegalValueTypes;
  }

  CodeGenSchedModels &getSchedModels() const;

  const CodeGenHwModes &getHwModes() const { return CGH; }

  bool hasMacroFusion() const { return !MacroFusions.empty(); }

  ArrayRef<const Record *> getMacroFusions() const { return MacroFusions; }

private:
  DenseMap<const Record *, std::unique_ptr<CodeGenInstruction>> &
  getInstructionMap() const {
    if (InstructionMap.empty())
      ReadInstructions();
    return InstructionMap;
  }

public:
  CodeGenInstruction &getInstruction(const Record *InstRec) const {
    auto I = getInstructionMap().find(InstRec);
    assert(I != InstructionMap.end() && "Not an instruction");
    return *I->second;
  }

  /// Returns the number of predefined instructions.
  static unsigned getNumFixedInstructions();

  /// Return all of the instructions defined by the target, ordered by their
  /// enum value.
  /// The following order of instructions is also guaranteed:
  /// - fixed / generic instructions as declared in TargetOpcodes.def, in order;
  /// - pseudo instructions in lexicographical order sorted by name;
  /// - other instructions in lexicographical order sorted by name.
  ArrayRef<const CodeGenInstruction *> getInstructionsByEnumValue() const {
    if (InstrsByEnum.empty())
      ComputeInstrsByEnum();
    return InstrsByEnum;
  }

  // Functions that return various slices of `getInstructionsByEnumValue`.
  ArrayRef<const CodeGenInstruction *>
  getGenericInstructionsByEnumValue() const {
    return getInstructionsByEnumValue().take_front(getNumFixedInstructions());
  }

  ArrayRef<const CodeGenInstruction *>
  getTargetInstructionsByEnumValue() const {
    return getInstructionsByEnumValue().drop_front(getNumFixedInstructions());
  }

  ArrayRef<const CodeGenInstruction *>
  getTargetPseudoInstructionsByEnumValue() const {
    return getTargetInstructionsByEnumValue().take_front(NumPseudoInstructions);
  }

  ArrayRef<const CodeGenInstruction *>
  getTargetNonPseudoInstructionsByEnumValue() const {
    return getTargetInstructionsByEnumValue().drop_front(NumPseudoInstructions);
  }

  /// Return the integer enum value corresponding to this instruction record.
  unsigned getInstrIntValue(const Record *R) const {
    if (InstrsByEnum.empty())
      ComputeInstrsByEnum();
    return getInstruction(R).EnumVal;
  }

  /// Return whether instructions have variable length encodings on this target.
  bool hasVariableLengthEncodings() const { return HasVariableLengthEncodings; }

  /// isLittleEndianEncoding - are instruction bit patterns defined as  [0..n]?
  ///
  bool isLittleEndianEncoding() const;

  /// reverseBitsForLittleEndianEncoding - For little-endian instruction bit
  /// encodings, reverse the bit order of all instructions.
  void reverseBitsForLittleEndianEncoding();

  /// guessInstructionProperties - should we just guess unset instruction
  /// properties?
  bool guessInstructionProperties() const;

  const CodeGenIntrinsic &getIntrinsic(const Record *Def) const {
    return Intrinsics[Def];
  }

private:
  void ComputeInstrsByEnum() const;
};

/// ComplexPattern - ComplexPattern info, corresponding to the ComplexPattern
/// tablegen class in TargetSelectionDAG.td
class ComplexPattern {
  const Record *Ty;
  unsigned NumOperands;
  std::string SelectFunc;
  std::vector<const Record *> RootNodes;
  unsigned Properties; // Node properties
  unsigned Complexity;
  bool WantsRoot;
  bool WantsParent;

public:
  ComplexPattern(const Record *R);

  const Record *getValueType() const { return Ty; }
  unsigned getNumOperands() const { return NumOperands; }
  const std::string &getSelectFunc() const { return SelectFunc; }
  ArrayRef<const Record *> getRootNodes() const { return RootNodes; }
  bool hasProperty(enum SDNP Prop) const { return Properties & (1 << Prop); }
  unsigned getComplexity() const { return Complexity; }
  bool wantsRoot() const { return WantsRoot; }
  bool wantsParent() const { return WantsParent; }
};

} // namespace llvm

#endif // LLVM_UTILS_TABLEGEN_COMMON_CODEGENTARGET_H
