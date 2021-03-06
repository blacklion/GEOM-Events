//===--- CGBlocks.cpp - Emit LLVM Code for declarations -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit blocks.
//
//===----------------------------------------------------------------------===//

#include "CGDebugInfo.h"
#include "CodeGenFunction.h"
#include "CGObjCRuntime.h"
#include "CodeGenModule.h"
#include "CGBlocks.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/Module.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Target/TargetData.h"
#include <algorithm>

using namespace clang;
using namespace CodeGen;

CGBlockInfo::CGBlockInfo(const BlockExpr *blockExpr, const char *N)
  : Name(N), CXXThisIndex(0), CanBeGlobal(false), NeedsCopyDispose(false),
    HasCXXObject(false), UsesStret(false), StructureType(0), Block(blockExpr) {
    
  // Skip asm prefix, if any.
  if (Name && Name[0] == '\01')
    ++Name;
}

// Anchor the vtable to this translation unit.
CodeGenModule::ByrefHelpers::~ByrefHelpers() {}

/// Build the given block as a global block.
static llvm::Constant *buildGlobalBlock(CodeGenModule &CGM,
                                        const CGBlockInfo &blockInfo,
                                        llvm::Constant *blockFn);

/// Build the helper function to copy a block.
static llvm::Constant *buildCopyHelper(CodeGenModule &CGM,
                                       const CGBlockInfo &blockInfo) {
  return CodeGenFunction(CGM).GenerateCopyHelperFunction(blockInfo);
}

/// Build the helper function to dipose of a block.
static llvm::Constant *buildDisposeHelper(CodeGenModule &CGM,
                                          const CGBlockInfo &blockInfo) {
  return CodeGenFunction(CGM).GenerateDestroyHelperFunction(blockInfo);
}

/// Build the block descriptor constant for a block.
static llvm::Constant *buildBlockDescriptor(CodeGenModule &CGM,
                                            const CGBlockInfo &blockInfo) {
  ASTContext &C = CGM.getContext();

  const llvm::Type *ulong = CGM.getTypes().ConvertType(C.UnsignedLongTy);
  const llvm::Type *i8p = CGM.getTypes().ConvertType(C.VoidPtrTy);

  llvm::SmallVector<llvm::Constant*, 6> elements;

  // reserved
  elements.push_back(llvm::ConstantInt::get(ulong, 0));

  // Size
  // FIXME: What is the right way to say this doesn't fit?  We should give
  // a user diagnostic in that case.  Better fix would be to change the
  // API to size_t.
  elements.push_back(llvm::ConstantInt::get(ulong,
                                            blockInfo.BlockSize.getQuantity()));

  // Optional copy/dispose helpers.
  if (blockInfo.NeedsCopyDispose) {
    // copy_func_helper_decl
    elements.push_back(buildCopyHelper(CGM, blockInfo));

    // destroy_func_decl
    elements.push_back(buildDisposeHelper(CGM, blockInfo));
  }

  // Signature.  Mandatory ObjC-style method descriptor @encode sequence.
  std::string typeAtEncoding =
    CGM.getContext().getObjCEncodingForBlock(blockInfo.getBlockExpr());
  elements.push_back(llvm::ConstantExpr::getBitCast(
                          CGM.GetAddrOfConstantCString(typeAtEncoding), i8p));
  
  // GC layout.
  if (C.getLangOptions().ObjC1)
    elements.push_back(CGM.getObjCRuntime().BuildGCBlockLayout(CGM, blockInfo));
  else
    elements.push_back(llvm::Constant::getNullValue(i8p));

  llvm::Constant *init = llvm::ConstantStruct::getAnon(elements);

  llvm::GlobalVariable *global =
    new llvm::GlobalVariable(CGM.getModule(), init->getType(), true,
                             llvm::GlobalValue::InternalLinkage,
                             init, "__block_descriptor_tmp");

  return llvm::ConstantExpr::getBitCast(global, CGM.getBlockDescriptorType());
}

/*
  Purely notional variadic template describing the layout of a block.

  template <class _ResultType, class... _ParamTypes, class... _CaptureTypes>
  struct Block_literal {
    /// Initialized to one of:
    ///   extern void *_NSConcreteStackBlock[];
    ///   extern void *_NSConcreteGlobalBlock[];
    ///
    /// In theory, we could start one off malloc'ed by setting
    /// BLOCK_NEEDS_FREE, giving it a refcount of 1, and using
    /// this isa:
    ///   extern void *_NSConcreteMallocBlock[];
    struct objc_class *isa;

    /// These are the flags (with corresponding bit number) that the
    /// compiler is actually supposed to know about.
    ///  25. BLOCK_HAS_COPY_DISPOSE - indicates that the block
    ///   descriptor provides copy and dispose helper functions
    ///  26. BLOCK_HAS_CXX_OBJ - indicates that there's a captured
    ///   object with a nontrivial destructor or copy constructor
    ///  28. BLOCK_IS_GLOBAL - indicates that the block is allocated
    ///   as global memory
    ///  29. BLOCK_USE_STRET - indicates that the block function
    ///   uses stret, which objc_msgSend needs to know about
    ///  30. BLOCK_HAS_SIGNATURE - indicates that the block has an
    ///   @encoded signature string
    /// And we're not supposed to manipulate these:
    ///  24. BLOCK_NEEDS_FREE - indicates that the block has been moved
    ///   to malloc'ed memory
    ///  27. BLOCK_IS_GC - indicates that the block has been moved to
    ///   to GC-allocated memory
    /// Additionally, the bottom 16 bits are a reference count which
    /// should be zero on the stack.
    int flags;

    /// Reserved;  should be zero-initialized.
    int reserved;

    /// Function pointer generated from block literal.
    _ResultType (*invoke)(Block_literal *, _ParamTypes...);

    /// Block description metadata generated from block literal.
    struct Block_descriptor *block_descriptor;

    /// Captured values follow.
    _CapturesTypes captures...;
  };
 */

/// The number of fields in a block header.
const unsigned BlockHeaderSize = 5;

namespace {
  /// A chunk of data that we actually have to capture in the block.
  struct BlockLayoutChunk {
    CharUnits Alignment;
    CharUnits Size;
    const BlockDecl::Capture *Capture; // null for 'this'
    llvm::Type *Type;

    BlockLayoutChunk(CharUnits align, CharUnits size,
                     const BlockDecl::Capture *capture,
                     llvm::Type *type)
      : Alignment(align), Size(size), Capture(capture), Type(type) {}

    /// Tell the block info that this chunk has the given field index.
    void setIndex(CGBlockInfo &info, unsigned index) {
      if (!Capture)
        info.CXXThisIndex = index;
      else
        info.Captures[Capture->getVariable()]
          = CGBlockInfo::Capture::makeIndex(index);
    }
  };

  /// Order by descending alignment.
  bool operator<(const BlockLayoutChunk &left, const BlockLayoutChunk &right) {
    return left.Alignment > right.Alignment;
  }
}

/// Determines if the given type is safe for constant capture in C++.
static bool isSafeForCXXConstantCapture(QualType type) {
  const RecordType *recordType =
    type->getBaseElementTypeUnsafe()->getAs<RecordType>();

  // Only records can be unsafe.
  if (!recordType) return true;

  const CXXRecordDecl *record = cast<CXXRecordDecl>(recordType->getDecl());

  // Maintain semantics for classes with non-trivial dtors or copy ctors.
  if (!record->hasTrivialDestructor()) return false;
  if (!record->hasTrivialCopyConstructor()) return false;

  // Otherwise, we just have to make sure there aren't any mutable
  // fields that might have changed since initialization.
  return !record->hasMutableFields();
}

/// It is illegal to modify a const object after initialization.
/// Therefore, if a const object has a constant initializer, we don't
/// actually need to keep storage for it in the block; we'll just
/// rematerialize it at the start of the block function.  This is
/// acceptable because we make no promises about address stability of
/// captured variables.
static llvm::Constant *tryCaptureAsConstant(CodeGenModule &CGM,
                                            const VarDecl *var) {
  QualType type = var->getType();

  // We can only do this if the variable is const.
  if (!type.isConstQualified()) return 0;

  // Furthermore, in C++ we have to worry about mutable fields:
  // C++ [dcl.type.cv]p4:
  //   Except that any class member declared mutable can be
  //   modified, any attempt to modify a const object during its
  //   lifetime results in undefined behavior.
  if (CGM.getLangOptions().CPlusPlus && !isSafeForCXXConstantCapture(type))
    return 0;

  // If the variable doesn't have any initializer (shouldn't this be
  // invalid?), it's not clear what we should do.  Maybe capture as
  // zero?
  const Expr *init = var->getInit();
  if (!init) return 0;

  return CGM.EmitConstantExpr(init, var->getType());
}

/// Get the low bit of a nonzero character count.  This is the
/// alignment of the nth byte if the 0th byte is universally aligned.
static CharUnits getLowBit(CharUnits v) {
  return CharUnits::fromQuantity(v.getQuantity() & (~v.getQuantity() + 1));
}

static void initializeForBlockHeader(CodeGenModule &CGM, CGBlockInfo &info,
                             llvm::SmallVectorImpl<llvm::Type*> &elementTypes) {
  ASTContext &C = CGM.getContext();

  // The header is basically a 'struct { void *; int; int; void *; void *; }'.
  CharUnits ptrSize, ptrAlign, intSize, intAlign;
  llvm::tie(ptrSize, ptrAlign) = C.getTypeInfoInChars(C.VoidPtrTy);
  llvm::tie(intSize, intAlign) = C.getTypeInfoInChars(C.IntTy);

  // Are there crazy embedded platforms where this isn't true?
  assert(intSize <= ptrSize && "layout assumptions horribly violated");

  CharUnits headerSize = ptrSize;
  if (2 * intSize < ptrAlign) headerSize += ptrSize;
  else headerSize += 2 * intSize;
  headerSize += 2 * ptrSize;

  info.BlockAlign = ptrAlign;
  info.BlockSize = headerSize;

  assert(elementTypes.empty());
  llvm::Type *i8p = CGM.getTypes().ConvertType(C.VoidPtrTy);
  llvm::Type *intTy = CGM.getTypes().ConvertType(C.IntTy);
  elementTypes.push_back(i8p);
  elementTypes.push_back(intTy);
  elementTypes.push_back(intTy);
  elementTypes.push_back(i8p);
  elementTypes.push_back(CGM.getBlockDescriptorType());

  assert(elementTypes.size() == BlockHeaderSize);
}

/// Compute the layout of the given block.  Attempts to lay the block
/// out with minimal space requirements.
static void computeBlockInfo(CodeGenModule &CGM, CGBlockInfo &info) {
  ASTContext &C = CGM.getContext();
  const BlockDecl *block = info.getBlockDecl();

  llvm::SmallVector<llvm::Type*, 8> elementTypes;
  initializeForBlockHeader(CGM, info, elementTypes);

  if (!block->hasCaptures()) {
    info.StructureType =
      llvm::StructType::get(CGM.getLLVMContext(), elementTypes, true);
    info.CanBeGlobal = true;
    return;
  }

  // Collect the layout chunks.
  llvm::SmallVector<BlockLayoutChunk, 16> layout;
  layout.reserve(block->capturesCXXThis() +
                 (block->capture_end() - block->capture_begin()));

  CharUnits maxFieldAlign;

  // First, 'this'.
  if (block->capturesCXXThis()) {
    const DeclContext *DC = block->getDeclContext();
    for (; isa<BlockDecl>(DC); DC = cast<BlockDecl>(DC)->getDeclContext())
      ;
    QualType thisType;
    if (const CXXRecordDecl *RD = dyn_cast<CXXRecordDecl>(DC))
      thisType = C.getPointerType(C.getRecordType(RD));
    else
      thisType = cast<CXXMethodDecl>(DC)->getThisType(C);

    llvm::Type *llvmType = CGM.getTypes().ConvertType(thisType);
    std::pair<CharUnits,CharUnits> tinfo
      = CGM.getContext().getTypeInfoInChars(thisType);
    maxFieldAlign = std::max(maxFieldAlign, tinfo.second);

    layout.push_back(BlockLayoutChunk(tinfo.second, tinfo.first, 0, llvmType));
  }

  // Next, all the block captures.
  for (BlockDecl::capture_const_iterator ci = block->capture_begin(),
         ce = block->capture_end(); ci != ce; ++ci) {
    const VarDecl *variable = ci->getVariable();

    if (ci->isByRef()) {
      // We have to copy/dispose of the __block reference.
      info.NeedsCopyDispose = true;

      // Just use void* instead of a pointer to the byref type.
      QualType byRefPtrTy = C.VoidPtrTy;

      llvm::Type *llvmType = CGM.getTypes().ConvertType(byRefPtrTy);
      std::pair<CharUnits,CharUnits> tinfo
        = CGM.getContext().getTypeInfoInChars(byRefPtrTy);
      maxFieldAlign = std::max(maxFieldAlign, tinfo.second);

      layout.push_back(BlockLayoutChunk(tinfo.second, tinfo.first,
                                        &*ci, llvmType));
      continue;
    }

    // Otherwise, build a layout chunk with the size and alignment of
    // the declaration.
    if (llvm::Constant *constant = tryCaptureAsConstant(CGM, variable)) {
      info.Captures[variable] = CGBlockInfo::Capture::makeConstant(constant);
      continue;
    }

    // If we have a lifetime qualifier, honor it for capture purposes.
    // That includes *not* copying it if it's __unsafe_unretained.
    if (Qualifiers::ObjCLifetime lifetime 
          = variable->getType().getObjCLifetime()) {
      switch (lifetime) {
      case Qualifiers::OCL_None: llvm_unreachable("impossible");
      case Qualifiers::OCL_ExplicitNone:
      case Qualifiers::OCL_Autoreleasing:
        break;

      case Qualifiers::OCL_Strong:
      case Qualifiers::OCL_Weak:
        info.NeedsCopyDispose = true;
      }

    // Block pointers require copy/dispose.  So do Objective-C pointers.
    } else if (variable->getType()->isObjCRetainableType()) {
      info.NeedsCopyDispose = true;

    // So do types that require non-trivial copy construction.
    } else if (ci->hasCopyExpr()) {
      info.NeedsCopyDispose = true;
      info.HasCXXObject = true;

    // And so do types with destructors.
    } else if (CGM.getLangOptions().CPlusPlus) {
      if (const CXXRecordDecl *record =
            variable->getType()->getAsCXXRecordDecl()) {
        if (!record->hasTrivialDestructor()) {
          info.HasCXXObject = true;
          info.NeedsCopyDispose = true;
        }
      }
    }

    CharUnits size = C.getTypeSizeInChars(variable->getType());
    CharUnits align = C.getDeclAlign(variable);
    maxFieldAlign = std::max(maxFieldAlign, align);

    llvm::Type *llvmType =
      CGM.getTypes().ConvertTypeForMem(variable->getType());

    layout.push_back(BlockLayoutChunk(align, size, &*ci, llvmType));
  }

  // If that was everything, we're done here.
  if (layout.empty()) {
    info.StructureType =
      llvm::StructType::get(CGM.getLLVMContext(), elementTypes, true);
    info.CanBeGlobal = true;
    return;
  }

  // Sort the layout by alignment.  We have to use a stable sort here
  // to get reproducible results.  There should probably be an
  // llvm::array_pod_stable_sort.
  std::stable_sort(layout.begin(), layout.end());

  CharUnits &blockSize = info.BlockSize;
  info.BlockAlign = std::max(maxFieldAlign, info.BlockAlign);

  // Assuming that the first byte in the header is maximally aligned,
  // get the alignment of the first byte following the header.
  CharUnits endAlign = getLowBit(blockSize);

  // If the end of the header isn't satisfactorily aligned for the
  // maximum thing, look for things that are okay with the header-end
  // alignment, and keep appending them until we get something that's
  // aligned right.  This algorithm is only guaranteed optimal if
  // that condition is satisfied at some point; otherwise we can get
  // things like:
  //   header                 // next byte has alignment 4
  //   something_with_size_5; // next byte has alignment 1
  //   something_with_alignment_8;
  // which has 7 bytes of padding, as opposed to the naive solution
  // which might have less (?).
  if (endAlign < maxFieldAlign) {
    llvm::SmallVectorImpl<BlockLayoutChunk>::iterator
      li = layout.begin() + 1, le = layout.end();

    // Look for something that the header end is already
    // satisfactorily aligned for.
    for (; li != le && endAlign < li->Alignment; ++li)
      ;

    // If we found something that's naturally aligned for the end of
    // the header, keep adding things...
    if (li != le) {
      llvm::SmallVectorImpl<BlockLayoutChunk>::iterator first = li;
      for (; li != le; ++li) {
        assert(endAlign >= li->Alignment);

        li->setIndex(info, elementTypes.size());
        elementTypes.push_back(li->Type);
        blockSize += li->Size;
        endAlign = getLowBit(blockSize);

        // ...until we get to the alignment of the maximum field.
        if (endAlign >= maxFieldAlign)
          break;
      }

      // Don't re-append everything we just appended.
      layout.erase(first, li);
    }
  }

  // At this point, we just have to add padding if the end align still
  // isn't aligned right.
  if (endAlign < maxFieldAlign) {
    CharUnits padding = maxFieldAlign - endAlign;

    elementTypes.push_back(llvm::ArrayType::get(CGM.Int8Ty,
                                                padding.getQuantity()));
    blockSize += padding;

    endAlign = getLowBit(blockSize);
    assert(endAlign >= maxFieldAlign);
  }

  // Slam everything else on now.  This works because they have
  // strictly decreasing alignment and we expect that size is always a
  // multiple of alignment.
  for (llvm::SmallVectorImpl<BlockLayoutChunk>::iterator
         li = layout.begin(), le = layout.end(); li != le; ++li) {
    assert(endAlign >= li->Alignment);
    li->setIndex(info, elementTypes.size());
    elementTypes.push_back(li->Type);
    blockSize += li->Size;
    endAlign = getLowBit(blockSize);
  }

  info.StructureType =
    llvm::StructType::get(CGM.getLLVMContext(), elementTypes, true);
}

/// Emit a block literal expression in the current function.
llvm::Value *CodeGenFunction::EmitBlockLiteral(const BlockExpr *blockExpr) {
  std::string Name = CurFn->getName();
  CGBlockInfo blockInfo(blockExpr, Name.c_str());

  // Compute information about the layout, etc., of this block.
  computeBlockInfo(CGM, blockInfo);

  // Using that metadata, generate the actual block function.
  llvm::Constant *blockFn
    = CodeGenFunction(CGM).GenerateBlockFunction(CurGD, blockInfo,
                                                 CurFuncDecl, LocalDeclMap);
  blockFn = llvm::ConstantExpr::getBitCast(blockFn, VoidPtrTy);

  // If there is nothing to capture, we can emit this as a global block.
  if (blockInfo.CanBeGlobal)
    return buildGlobalBlock(CGM, blockInfo, blockFn);

  // Otherwise, we have to emit this as a local block.

  llvm::Constant *isa = CGM.getNSConcreteStackBlock();
  isa = llvm::ConstantExpr::getBitCast(isa, VoidPtrTy);

  // Build the block descriptor.
  llvm::Constant *descriptor = buildBlockDescriptor(CGM, blockInfo);

  const llvm::Type *intTy = ConvertType(getContext().IntTy);

  llvm::AllocaInst *blockAddr =
    CreateTempAlloca(blockInfo.StructureType, "block");
  blockAddr->setAlignment(blockInfo.BlockAlign.getQuantity());

  // Compute the initial on-stack block flags.
  BlockFlags flags = BLOCK_HAS_SIGNATURE;
  if (blockInfo.NeedsCopyDispose) flags |= BLOCK_HAS_COPY_DISPOSE;
  if (blockInfo.HasCXXObject) flags |= BLOCK_HAS_CXX_OBJ;
  if (blockInfo.UsesStret) flags |= BLOCK_USE_STRET;

  // Initialize the block literal.
  Builder.CreateStore(isa, Builder.CreateStructGEP(blockAddr, 0, "block.isa"));
  Builder.CreateStore(llvm::ConstantInt::get(intTy, flags.getBitMask()),
                      Builder.CreateStructGEP(blockAddr, 1, "block.flags"));
  Builder.CreateStore(llvm::ConstantInt::get(intTy, 0),
                      Builder.CreateStructGEP(blockAddr, 2, "block.reserved"));
  Builder.CreateStore(blockFn, Builder.CreateStructGEP(blockAddr, 3,
                                                       "block.invoke"));
  Builder.CreateStore(descriptor, Builder.CreateStructGEP(blockAddr, 4,
                                                          "block.descriptor"));

  // Finally, capture all the values into the block.
  const BlockDecl *blockDecl = blockInfo.getBlockDecl();

  // First, 'this'.
  if (blockDecl->capturesCXXThis()) {
    llvm::Value *addr = Builder.CreateStructGEP(blockAddr,
                                                blockInfo.CXXThisIndex,
                                                "block.captured-this.addr");
    Builder.CreateStore(LoadCXXThis(), addr);
  }

  // Next, captured variables.
  for (BlockDecl::capture_const_iterator ci = blockDecl->capture_begin(),
         ce = blockDecl->capture_end(); ci != ce; ++ci) {
    const VarDecl *variable = ci->getVariable();
    const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);

    // Ignore constant captures.
    if (capture.isConstant()) continue;

    QualType type = variable->getType();

    // This will be a [[type]]*, except that a byref entry will just be
    // an i8**.
    llvm::Value *blockField =
      Builder.CreateStructGEP(blockAddr, capture.getIndex(),
                              "block.captured");

    // Compute the address of the thing we're going to move into the
    // block literal.
    llvm::Value *src;
    if (ci->isNested()) {
      // We need to use the capture from the enclosing block.
      const CGBlockInfo::Capture &enclosingCapture =
        BlockInfo->getCapture(variable);

      // This is a [[type]]*, except that a byref entry wil just be an i8**.
      src = Builder.CreateStructGEP(LoadBlockStruct(),
                                    enclosingCapture.getIndex(),
                                    "block.capture.addr");
    } else {
      // This is a [[type]]*.
      src = LocalDeclMap[variable];
    }

    // For byrefs, we just write the pointer to the byref struct into
    // the block field.  There's no need to chase the forwarding
    // pointer at this point, since we're building something that will
    // live a shorter life than the stack byref anyway.
    if (ci->isByRef()) {
      // Get a void* that points to the byref struct.
      if (ci->isNested())
        src = Builder.CreateLoad(src, "byref.capture");
      else
        src = Builder.CreateBitCast(src, VoidPtrTy);

      // Write that void* into the capture field.
      Builder.CreateStore(src, blockField);

    // If we have a copy constructor, evaluate that into the block field.
    } else if (const Expr *copyExpr = ci->getCopyExpr()) {
      EmitSynthesizedCXXCopyCtor(blockField, src, copyExpr);

    // If it's a reference variable, copy the reference into the block field.
    } else if (type->isReferenceType()) {
      Builder.CreateStore(Builder.CreateLoad(src, "ref.val"), blockField);

    // Otherwise, fake up a POD copy into the block field.
    } else {
      // Fake up a new variable so that EmitScalarInit doesn't think
      // we're referring to the variable in its own initializer.
      ImplicitParamDecl blockFieldPseudoVar(/*DC*/ 0, SourceLocation(),
                                            /*name*/ 0, type);

      // We use one of these or the other depending on whether the
      // reference is nested.
      DeclRefExpr notNested(const_cast<VarDecl*>(variable), type, VK_LValue,
                            SourceLocation());
      BlockDeclRefExpr nested(const_cast<VarDecl*>(variable), type,
                              VK_LValue, SourceLocation(), /*byref*/ false);

      Expr *declRef = 
        (ci->isNested() ? static_cast<Expr*>(&nested) : &notNested);

      ImplicitCastExpr l2r(ImplicitCastExpr::OnStack, type, CK_LValueToRValue,
                           declRef, VK_RValue);
      EmitExprAsInit(&l2r, &blockFieldPseudoVar,
                     LValue::MakeAddr(blockField, type,
                                      getContext().getDeclAlign(variable)
                                                  .getQuantity(),
                                      getContext()),
                     /*captured by init*/ false);
    }

    // Push a destructor if necessary.  The semantics for when this
    // actually gets run are really obscure.
    if (!ci->isByRef()) {
      switch (QualType::DestructionKind dtorKind = type.isDestructedType()) {
      case QualType::DK_none:
        break;

      // Block captures count as local values and have imprecise semantics.
      // They also can't be arrays, so need to worry about that.
      case QualType::DK_objc_strong_lifetime: {
        // This local is a GCC and MSVC compiler workaround.
        Destroyer *destroyer = &destroyARCStrongImprecise;
        pushDestroy(getCleanupKind(dtorKind), blockField, type,
                    *destroyer, /*useEHCleanupForArray*/ false);
        break;
      }

      case QualType::DK_objc_weak_lifetime:
      case QualType::DK_cxx_destructor:
        pushDestroy(dtorKind, blockField, type);
        break;
      }
    }
  }

  // Cast to the converted block-pointer type, which happens (somewhat
  // unfortunately) to be a pointer to function type.
  llvm::Value *result =
    Builder.CreateBitCast(blockAddr,
                          ConvertType(blockInfo.getBlockExpr()->getType()));

  return result;
}


llvm::Type *CodeGenModule::getBlockDescriptorType() {
  if (BlockDescriptorType)
    return BlockDescriptorType;

  llvm::Type *UnsignedLongTy =
    getTypes().ConvertType(getContext().UnsignedLongTy);

  // struct __block_descriptor {
  //   unsigned long reserved;
  //   unsigned long block_size;
  //
  //   // later, the following will be added
  //
  //   struct {
  //     void (*copyHelper)();
  //     void (*copyHelper)();
  //   } helpers;                // !!! optional
  //
  //   const char *signature;   // the block signature
  //   const char *layout;      // reserved
  // };
  BlockDescriptorType =
    llvm::StructType::createNamed("struct.__block_descriptor",
                                  UnsignedLongTy, UnsignedLongTy, NULL);

  // Now form a pointer to that.
  BlockDescriptorType = llvm::PointerType::getUnqual(BlockDescriptorType);
  return BlockDescriptorType;
}

llvm::Type *CodeGenModule::getGenericBlockLiteralType() {
  if (GenericBlockLiteralType)
    return GenericBlockLiteralType;

  llvm::Type *BlockDescPtrTy = getBlockDescriptorType();

  // struct __block_literal_generic {
  //   void *__isa;
  //   int __flags;
  //   int __reserved;
  //   void (*__invoke)(void *);
  //   struct __block_descriptor *__descriptor;
  // };
  GenericBlockLiteralType =
    llvm::StructType::createNamed("struct.__block_literal_generic",
                                  VoidPtrTy,
                                  IntTy,
                                  IntTy,
                                  VoidPtrTy,
                                  BlockDescPtrTy,
                                  NULL);

  return GenericBlockLiteralType;
}


RValue CodeGenFunction::EmitBlockCallExpr(const CallExpr* E, 
                                          ReturnValueSlot ReturnValue) {
  const BlockPointerType *BPT =
    E->getCallee()->getType()->getAs<BlockPointerType>();

  llvm::Value *Callee = EmitScalarExpr(E->getCallee());

  // Get a pointer to the generic block literal.
  const llvm::Type *BlockLiteralTy =
    llvm::PointerType::getUnqual(CGM.getGenericBlockLiteralType());

  // Bitcast the callee to a block literal.
  llvm::Value *BlockLiteral =
    Builder.CreateBitCast(Callee, BlockLiteralTy, "block.literal");

  // Get the function pointer from the literal.
  llvm::Value *FuncPtr = Builder.CreateStructGEP(BlockLiteral, 3, "tmp");

  BlockLiteral = Builder.CreateBitCast(BlockLiteral, VoidPtrTy, "tmp");

  // Add the block literal.
  CallArgList Args;
  Args.add(RValue::get(BlockLiteral), getContext().VoidPtrTy);

  QualType FnType = BPT->getPointeeType();

  // And the rest of the arguments.
  EmitCallArgs(Args, FnType->getAs<FunctionProtoType>(),
               E->arg_begin(), E->arg_end());

  // Load the function.
  llvm::Value *Func = Builder.CreateLoad(FuncPtr, "tmp");

  const FunctionType *FuncTy = FnType->castAs<FunctionType>();
  QualType ResultType = FuncTy->getResultType();

  const CGFunctionInfo &FnInfo =
    CGM.getTypes().getFunctionInfo(ResultType, Args,
                                   FuncTy->getExtInfo());

  // Cast the function pointer to the right type.
  const llvm::Type *BlockFTy =
    CGM.getTypes().GetFunctionType(FnInfo, false);

  const llvm::Type *BlockFTyPtr = llvm::PointerType::getUnqual(BlockFTy);
  Func = Builder.CreateBitCast(Func, BlockFTyPtr);

  // And call the block.
  return EmitCall(FnInfo, Func, ReturnValue, Args);
}

llvm::Value *CodeGenFunction::GetAddrOfBlockDecl(const VarDecl *variable,
                                                 bool isByRef) {
  assert(BlockInfo && "evaluating block ref without block information?");
  const CGBlockInfo::Capture &capture = BlockInfo->getCapture(variable);

  // Handle constant captures.
  if (capture.isConstant()) return LocalDeclMap[variable];

  llvm::Value *addr =
    Builder.CreateStructGEP(LoadBlockStruct(), capture.getIndex(),
                            "block.capture.addr");

  if (isByRef) {
    // addr should be a void** right now.  Load, then cast the result
    // to byref*.

    addr = Builder.CreateLoad(addr);
    const llvm::PointerType *byrefPointerType
      = llvm::PointerType::get(BuildByRefType(variable), 0);
    addr = Builder.CreateBitCast(addr, byrefPointerType,
                                 "byref.addr");

    // Follow the forwarding pointer.
    addr = Builder.CreateStructGEP(addr, 1, "byref.forwarding");
    addr = Builder.CreateLoad(addr, "byref.addr.forwarded");

    // Cast back to byref* and GEP over to the actual object.
    addr = Builder.CreateBitCast(addr, byrefPointerType);
    addr = Builder.CreateStructGEP(addr, getByRefValueLLVMField(variable), 
                                   variable->getNameAsString());
  }

  if (variable->getType()->isReferenceType())
    addr = Builder.CreateLoad(addr, "ref.tmp");

  return addr;
}

llvm::Constant *
CodeGenModule::GetAddrOfGlobalBlock(const BlockExpr *blockExpr,
                                    const char *name) {
  CGBlockInfo blockInfo(blockExpr, name);

  // Compute information about the layout, etc., of this block.
  computeBlockInfo(*this, blockInfo);

  // Using that metadata, generate the actual block function.
  llvm::Constant *blockFn;
  {
    llvm::DenseMap<const Decl*, llvm::Value*> LocalDeclMap;
    blockFn = CodeGenFunction(*this).GenerateBlockFunction(GlobalDecl(),
                                                           blockInfo,
                                                           0, LocalDeclMap);
  }
  blockFn = llvm::ConstantExpr::getBitCast(blockFn, VoidPtrTy);

  return buildGlobalBlock(*this, blockInfo, blockFn);
}

static llvm::Constant *buildGlobalBlock(CodeGenModule &CGM,
                                        const CGBlockInfo &blockInfo,
                                        llvm::Constant *blockFn) {
  assert(blockInfo.CanBeGlobal);

  // Generate the constants for the block literal initializer.
  llvm::Constant *fields[BlockHeaderSize];

  // isa
  fields[0] = CGM.getNSConcreteGlobalBlock();

  // __flags
  BlockFlags flags = BLOCK_IS_GLOBAL | BLOCK_HAS_SIGNATURE;
  if (blockInfo.UsesStret) flags |= BLOCK_USE_STRET;
                                      
  fields[1] = llvm::ConstantInt::get(CGM.IntTy, flags.getBitMask());

  // Reserved
  fields[2] = llvm::Constant::getNullValue(CGM.IntTy);

  // Function
  fields[3] = blockFn;

  // Descriptor
  fields[4] = buildBlockDescriptor(CGM, blockInfo);

  llvm::Constant *init = llvm::ConstantStruct::getAnon(fields);

  llvm::GlobalVariable *literal =
    new llvm::GlobalVariable(CGM.getModule(),
                             init->getType(),
                             /*constant*/ true,
                             llvm::GlobalVariable::InternalLinkage,
                             init,
                             "__block_literal_global");
  literal->setAlignment(blockInfo.BlockAlign.getQuantity());

  // Return a constant of the appropriately-casted type.
  const llvm::Type *requiredType =
    CGM.getTypes().ConvertType(blockInfo.getBlockExpr()->getType());
  return llvm::ConstantExpr::getBitCast(literal, requiredType);
}

llvm::Function *
CodeGenFunction::GenerateBlockFunction(GlobalDecl GD,
                                       const CGBlockInfo &blockInfo,
                                       const Decl *outerFnDecl,
                                       const DeclMapTy &ldm) {
  const BlockDecl *blockDecl = blockInfo.getBlockDecl();

  // Check if we should generate debug info for this block function.
  if (CGM.getModuleDebugInfo())
    DebugInfo = CGM.getModuleDebugInfo();

  BlockInfo = &blockInfo;

  // Arrange for local static and local extern declarations to appear
  // to be local to this function as well, in case they're directly
  // referenced in a block.
  for (DeclMapTy::const_iterator i = ldm.begin(), e = ldm.end(); i != e; ++i) {
    const VarDecl *var = dyn_cast<VarDecl>(i->first);
    if (var && !var->hasLocalStorage())
      LocalDeclMap[var] = i->second;
  }

  // Begin building the function declaration.

  // Build the argument list.
  FunctionArgList args;

  // The first argument is the block pointer.  Just take it as a void*
  // and cast it later.
  QualType selfTy = getContext().VoidPtrTy;
  IdentifierInfo *II = &CGM.getContext().Idents.get(".block_descriptor");

  ImplicitParamDecl selfDecl(const_cast<BlockDecl*>(blockDecl),
                             SourceLocation(), II, selfTy);
  args.push_back(&selfDecl);

  // Now add the rest of the parameters.
  for (BlockDecl::param_const_iterator i = blockDecl->param_begin(),
       e = blockDecl->param_end(); i != e; ++i)
    args.push_back(*i);

  // Create the function declaration.
  const FunctionProtoType *fnType =
    cast<FunctionProtoType>(blockInfo.getBlockExpr()->getFunctionType());
  const CGFunctionInfo &fnInfo =
    CGM.getTypes().getFunctionInfo(fnType->getResultType(), args,
                                   fnType->getExtInfo());
  if (CGM.ReturnTypeUsesSRet(fnInfo))
    blockInfo.UsesStret = true;

  const llvm::FunctionType *fnLLVMType =
    CGM.getTypes().GetFunctionType(fnInfo, fnType->isVariadic());

  MangleBuffer name;
  CGM.getBlockMangledName(GD, name, blockDecl);
  llvm::Function *fn =
    llvm::Function::Create(fnLLVMType, llvm::GlobalValue::InternalLinkage, 
                           name.getString(), &CGM.getModule());
  CGM.SetInternalFunctionAttributes(blockDecl, fn, fnInfo);

  // Begin generating the function.
  StartFunction(blockDecl, fnType->getResultType(), fn, fnInfo, args,
                blockInfo.getBlockExpr()->getBody()->getLocStart());
  CurFuncDecl = outerFnDecl; // StartFunction sets this to blockDecl

  // Okay.  Undo some of what StartFunction did.
  
  // Pull the 'self' reference out of the local decl map.
  llvm::Value *blockAddr = LocalDeclMap[&selfDecl];
  LocalDeclMap.erase(&selfDecl);
  BlockPointer = Builder.CreateBitCast(blockAddr,
                                       blockInfo.StructureType->getPointerTo(),
                                       "block");

  // If we have a C++ 'this' reference, go ahead and force it into
  // existence now.
  if (blockDecl->capturesCXXThis()) {
    llvm::Value *addr = Builder.CreateStructGEP(BlockPointer,
                                                blockInfo.CXXThisIndex,
                                                "block.captured-this");
    CXXThisValue = Builder.CreateLoad(addr, "this");
  }

  // LoadObjCSelf() expects there to be an entry for 'self' in LocalDeclMap;
  // appease it.
  if (const ObjCMethodDecl *method
        = dyn_cast_or_null<ObjCMethodDecl>(CurFuncDecl)) {
    const VarDecl *self = method->getSelfDecl();

    // There might not be a capture for 'self', but if there is...
    if (blockInfo.Captures.count(self)) {
      const CGBlockInfo::Capture &capture = blockInfo.getCapture(self);
      llvm::Value *selfAddr = Builder.CreateStructGEP(BlockPointer,
                                                      capture.getIndex(),
                                                      "block.captured-self");
      LocalDeclMap[self] = selfAddr;
    }
  }

  // Also force all the constant captures.
  for (BlockDecl::capture_const_iterator ci = blockDecl->capture_begin(),
         ce = blockDecl->capture_end(); ci != ce; ++ci) {
    const VarDecl *variable = ci->getVariable();
    const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);
    if (!capture.isConstant()) continue;

    unsigned align = getContext().getDeclAlign(variable).getQuantity();

    llvm::AllocaInst *alloca =
      CreateMemTemp(variable->getType(), "block.captured-const");
    alloca->setAlignment(align);

    Builder.CreateStore(capture.getConstant(), alloca, align);

    LocalDeclMap[variable] = alloca;
  }

  // Save a spot to insert the debug information for all the BlockDeclRefDecls.
  llvm::BasicBlock *entry = Builder.GetInsertBlock();
  llvm::BasicBlock::iterator entry_ptr = Builder.GetInsertPoint();
  --entry_ptr;

  EmitStmt(blockDecl->getBody());

  // Remember where we were...
  llvm::BasicBlock *resume = Builder.GetInsertBlock();

  // Go back to the entry.
  ++entry_ptr;
  Builder.SetInsertPoint(entry, entry_ptr);

  // Emit debug information for all the BlockDeclRefDecls.
  // FIXME: also for 'this'
  if (CGDebugInfo *DI = getDebugInfo()) {
    for (BlockDecl::capture_const_iterator ci = blockDecl->capture_begin(),
           ce = blockDecl->capture_end(); ci != ce; ++ci) {
      const VarDecl *variable = ci->getVariable();
      DI->setLocation(variable->getLocation());

      const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);
      if (capture.isConstant()) {
        DI->EmitDeclareOfAutoVariable(variable, LocalDeclMap[variable],
                                      Builder);
        continue;
      }

      DI->EmitDeclareOfBlockDeclRefVariable(variable, BlockPointer,
                                            Builder, blockInfo);
    }
  }

  // And resume where we left off.
  if (resume == 0)
    Builder.ClearInsertionPoint();
  else
    Builder.SetInsertPoint(resume);

  FinishFunction(cast<CompoundStmt>(blockDecl->getBody())->getRBracLoc());

  return fn;
}

/*
    notes.push_back(HelperInfo());
    HelperInfo &note = notes.back();
    note.index = capture.getIndex();
    note.RequiresCopying = (ci->hasCopyExpr() || BlockRequiresCopying(type));
    note.cxxbar_import = ci->getCopyExpr();

    if (ci->isByRef()) {
      note.flag = BLOCK_FIELD_IS_BYREF;
      if (type.isObjCGCWeak())
        note.flag |= BLOCK_FIELD_IS_WEAK;
    } else if (type->isBlockPointerType()) {
      note.flag = BLOCK_FIELD_IS_BLOCK;
    } else {
      note.flag = BLOCK_FIELD_IS_OBJECT;
    }
 */



llvm::Constant *
CodeGenFunction::GenerateCopyHelperFunction(const CGBlockInfo &blockInfo) {
  ASTContext &C = getContext();

  FunctionArgList args;
  ImplicitParamDecl dstDecl(0, SourceLocation(), 0, C.VoidPtrTy);
  args.push_back(&dstDecl);
  ImplicitParamDecl srcDecl(0, SourceLocation(), 0, C.VoidPtrTy);
  args.push_back(&srcDecl);

  const CGFunctionInfo &FI =
      CGM.getTypes().getFunctionInfo(C.VoidTy, args, FunctionType::ExtInfo());

  // FIXME: it would be nice if these were mergeable with things with
  // identical semantics.
  const llvm::FunctionType *LTy = CGM.getTypes().GetFunctionType(FI, false);

  llvm::Function *Fn =
    llvm::Function::Create(LTy, llvm::GlobalValue::InternalLinkage,
                           "__copy_helper_block_", &CGM.getModule());

  IdentifierInfo *II
    = &CGM.getContext().Idents.get("__copy_helper_block_");

  // Check if we should generate debug info for this block helper function.
  if (CGM.getModuleDebugInfo())
    DebugInfo = CGM.getModuleDebugInfo();

  FunctionDecl *FD = FunctionDecl::Create(C,
                                          C.getTranslationUnitDecl(),
                                          SourceLocation(),
                                          SourceLocation(), II, C.VoidTy, 0,
                                          SC_Static,
                                          SC_None,
                                          false,
                                          true);
  StartFunction(FD, C.VoidTy, Fn, FI, args, SourceLocation());

  const llvm::Type *structPtrTy = blockInfo.StructureType->getPointerTo();

  llvm::Value *src = GetAddrOfLocalVar(&srcDecl);
  src = Builder.CreateLoad(src);
  src = Builder.CreateBitCast(src, structPtrTy, "block.source");

  llvm::Value *dst = GetAddrOfLocalVar(&dstDecl);
  dst = Builder.CreateLoad(dst);
  dst = Builder.CreateBitCast(dst, structPtrTy, "block.dest");

  const BlockDecl *blockDecl = blockInfo.getBlockDecl();

  for (BlockDecl::capture_const_iterator ci = blockDecl->capture_begin(),
         ce = blockDecl->capture_end(); ci != ce; ++ci) {
    const VarDecl *variable = ci->getVariable();
    QualType type = variable->getType();

    const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);
    if (capture.isConstant()) continue;

    const Expr *copyExpr = ci->getCopyExpr();
    BlockFieldFlags flags;

    bool isARCWeakCapture = false;

    if (copyExpr) {
      assert(!ci->isByRef());
      // don't bother computing flags

    } else if (ci->isByRef()) {
      flags = BLOCK_FIELD_IS_BYREF;
      if (type.isObjCGCWeak())
        flags |= BLOCK_FIELD_IS_WEAK;

    } else if (type->isObjCRetainableType()) {
      flags = BLOCK_FIELD_IS_OBJECT;
      if (type->isBlockPointerType())
        flags = BLOCK_FIELD_IS_BLOCK;

      // Special rules for ARC captures:
      if (getLangOptions().ObjCAutoRefCount) {
        Qualifiers qs = type.getQualifiers();

        // Don't generate special copy logic for a captured object
        // unless it's __strong or __weak.
        if (!qs.hasStrongOrWeakObjCLifetime())
          continue;

        // Support __weak direct captures.
        if (qs.getObjCLifetime() == Qualifiers::OCL_Weak)
          isARCWeakCapture = true;
      }
    } else {
      continue;
    }

    unsigned index = capture.getIndex();
    llvm::Value *srcField = Builder.CreateStructGEP(src, index);
    llvm::Value *dstField = Builder.CreateStructGEP(dst, index);

    // If there's an explicit copy expression, we do that.
    if (copyExpr) {
      EmitSynthesizedCXXCopyCtor(dstField, srcField, copyExpr);
    } else if (isARCWeakCapture) {
      EmitARCCopyWeak(dstField, srcField);
    } else {
      llvm::Value *srcValue = Builder.CreateLoad(srcField, "blockcopy.src");
      srcValue = Builder.CreateBitCast(srcValue, VoidPtrTy);
      llvm::Value *dstAddr = Builder.CreateBitCast(dstField, VoidPtrTy);
      Builder.CreateCall3(CGM.getBlockObjectAssign(), dstAddr, srcValue,
                          llvm::ConstantInt::get(Int32Ty, flags.getBitMask()));
    }
  }

  FinishFunction();

  return llvm::ConstantExpr::getBitCast(Fn, VoidPtrTy);
}

llvm::Constant *
CodeGenFunction::GenerateDestroyHelperFunction(const CGBlockInfo &blockInfo) {
  ASTContext &C = getContext();

  FunctionArgList args;
  ImplicitParamDecl srcDecl(0, SourceLocation(), 0, C.VoidPtrTy);
  args.push_back(&srcDecl);

  const CGFunctionInfo &FI =
      CGM.getTypes().getFunctionInfo(C.VoidTy, args, FunctionType::ExtInfo());

  // FIXME: We'd like to put these into a mergable by content, with
  // internal linkage.
  const llvm::FunctionType *LTy = CGM.getTypes().GetFunctionType(FI, false);

  llvm::Function *Fn =
    llvm::Function::Create(LTy, llvm::GlobalValue::InternalLinkage,
                           "__destroy_helper_block_", &CGM.getModule());

  // Check if we should generate debug info for this block destroy function.
  if (CGM.getModuleDebugInfo())
    DebugInfo = CGM.getModuleDebugInfo();

  IdentifierInfo *II
    = &CGM.getContext().Idents.get("__destroy_helper_block_");

  FunctionDecl *FD = FunctionDecl::Create(C, C.getTranslationUnitDecl(),
                                          SourceLocation(),
                                          SourceLocation(), II, C.VoidTy, 0,
                                          SC_Static,
                                          SC_None,
                                          false, true);
  StartFunction(FD, C.VoidTy, Fn, FI, args, SourceLocation());

  const llvm::Type *structPtrTy = blockInfo.StructureType->getPointerTo();

  llvm::Value *src = GetAddrOfLocalVar(&srcDecl);
  src = Builder.CreateLoad(src);
  src = Builder.CreateBitCast(src, structPtrTy, "block");

  const BlockDecl *blockDecl = blockInfo.getBlockDecl();

  CodeGenFunction::RunCleanupsScope cleanups(*this);

  for (BlockDecl::capture_const_iterator ci = blockDecl->capture_begin(),
         ce = blockDecl->capture_end(); ci != ce; ++ci) {
    const VarDecl *variable = ci->getVariable();
    QualType type = variable->getType();

    const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);
    if (capture.isConstant()) continue;

    BlockFieldFlags flags;
    const CXXDestructorDecl *dtor = 0;

    bool isARCWeakCapture = false;

    if (ci->isByRef()) {
      flags = BLOCK_FIELD_IS_BYREF;
      if (type.isObjCGCWeak())
        flags |= BLOCK_FIELD_IS_WEAK;
    } else if (const CXXRecordDecl *record = type->getAsCXXRecordDecl()) {
      if (record->hasTrivialDestructor())
        continue;
      dtor = record->getDestructor();
    } else if (type->isObjCRetainableType()) {
      flags = BLOCK_FIELD_IS_OBJECT;
      if (type->isBlockPointerType())
        flags = BLOCK_FIELD_IS_BLOCK;

      // Special rules for ARC captures.
      if (getLangOptions().ObjCAutoRefCount) {
        Qualifiers qs = type.getQualifiers();

        // Don't generate special dispose logic for a captured object
        // unless it's __strong or __weak.
        if (!qs.hasStrongOrWeakObjCLifetime())
          continue;

        // Support __weak direct captures.
        if (qs.getObjCLifetime() == Qualifiers::OCL_Weak)
          isARCWeakCapture = true;
      }
    } else {
      continue;
    }

    unsigned index = capture.getIndex();
    llvm::Value *srcField = Builder.CreateStructGEP(src, index);

    // If there's an explicit copy expression, we do that.
    if (dtor) {
      PushDestructorCleanup(dtor, srcField);

    // If this is a __weak capture, emit the release directly.
    } else if (isARCWeakCapture) {
      EmitARCDestroyWeak(srcField);

    // Otherwise we call _Block_object_dispose.  It wouldn't be too
    // hard to just emit this as a cleanup if we wanted to make sure
    // that things were done in reverse.
    } else {
      llvm::Value *value = Builder.CreateLoad(srcField);
      value = Builder.CreateBitCast(value, VoidPtrTy);
      BuildBlockRelease(value, flags);
    }
  }

  cleanups.ForceCleanup();

  FinishFunction();

  return llvm::ConstantExpr::getBitCast(Fn, VoidPtrTy);
}

namespace {

/// Emits the copy/dispose helper functions for a __block object of id type.
class ObjectByrefHelpers : public CodeGenModule::ByrefHelpers {
  BlockFieldFlags Flags;

public:
  ObjectByrefHelpers(CharUnits alignment, BlockFieldFlags flags)
    : ByrefHelpers(alignment), Flags(flags) {}

  void emitCopy(CodeGenFunction &CGF, llvm::Value *destField,
                llvm::Value *srcField) {
    destField = CGF.Builder.CreateBitCast(destField, CGF.VoidPtrTy);

    srcField = CGF.Builder.CreateBitCast(srcField, CGF.VoidPtrPtrTy);
    llvm::Value *srcValue = CGF.Builder.CreateLoad(srcField);

    unsigned flags = (Flags | BLOCK_BYREF_CALLER).getBitMask();

    llvm::Value *flagsVal = llvm::ConstantInt::get(CGF.Int32Ty, flags);
    llvm::Value *fn = CGF.CGM.getBlockObjectAssign();
    CGF.Builder.CreateCall3(fn, destField, srcValue, flagsVal);
  }

  void emitDispose(CodeGenFunction &CGF, llvm::Value *field) {
    field = CGF.Builder.CreateBitCast(field, CGF.Int8PtrTy->getPointerTo(0));
    llvm::Value *value = CGF.Builder.CreateLoad(field);

    CGF.BuildBlockRelease(value, Flags | BLOCK_BYREF_CALLER);
  }

  void profileImpl(llvm::FoldingSetNodeID &id) const {
    id.AddInteger(Flags.getBitMask());
  }
};

/// Emits the copy/dispose helpers for an ARC __block __weak variable.
class ARCWeakByrefHelpers : public CodeGenModule::ByrefHelpers {
public:
  ARCWeakByrefHelpers(CharUnits alignment) : ByrefHelpers(alignment) {}

  void emitCopy(CodeGenFunction &CGF, llvm::Value *destField,
                llvm::Value *srcField) {
    CGF.EmitARCMoveWeak(destField, srcField);
  }

  void emitDispose(CodeGenFunction &CGF, llvm::Value *field) {
    CGF.EmitARCDestroyWeak(field);
  }

  void profileImpl(llvm::FoldingSetNodeID &id) const {
    // 0 is distinguishable from all pointers and byref flags
    id.AddInteger(0);
  }
};

/// Emits the copy/dispose helpers for an ARC __block __strong variable
/// that's not of block-pointer type.
class ARCStrongByrefHelpers : public CodeGenModule::ByrefHelpers {
public:
  ARCStrongByrefHelpers(CharUnits alignment) : ByrefHelpers(alignment) {}

  void emitCopy(CodeGenFunction &CGF, llvm::Value *destField,
                llvm::Value *srcField) {
    // Do a "move" by copying the value and then zeroing out the old
    // variable.

    llvm::Value *value = CGF.Builder.CreateLoad(srcField);
    llvm::Value *null =
      llvm::ConstantPointerNull::get(cast<llvm::PointerType>(value->getType()));
    CGF.Builder.CreateStore(value, destField);
    CGF.Builder.CreateStore(null, srcField);
  }

  void emitDispose(CodeGenFunction &CGF, llvm::Value *field) {
    llvm::Value *value = CGF.Builder.CreateLoad(field);
    CGF.EmitARCRelease(value, /*precise*/ false);
  }

  void profileImpl(llvm::FoldingSetNodeID &id) const {
    // 1 is distinguishable from all pointers and byref flags
    id.AddInteger(1);
  }
};

/// Emits the copy/dispose helpers for a __block variable with a
/// nontrivial copy constructor or destructor.
class CXXByrefHelpers : public CodeGenModule::ByrefHelpers {
  QualType VarType;
  const Expr *CopyExpr;

public:
  CXXByrefHelpers(CharUnits alignment, QualType type,
                  const Expr *copyExpr)
    : ByrefHelpers(alignment), VarType(type), CopyExpr(copyExpr) {}

  bool needsCopy() const { return CopyExpr != 0; }
  void emitCopy(CodeGenFunction &CGF, llvm::Value *destField,
                llvm::Value *srcField) {
    if (!CopyExpr) return;
    CGF.EmitSynthesizedCXXCopyCtor(destField, srcField, CopyExpr);
  }

  void emitDispose(CodeGenFunction &CGF, llvm::Value *field) {
    EHScopeStack::stable_iterator cleanupDepth = CGF.EHStack.stable_begin();
    CGF.PushDestructorCleanup(VarType, field);
    CGF.PopCleanupBlocks(cleanupDepth);
  }

  void profileImpl(llvm::FoldingSetNodeID &id) const {
    id.AddPointer(VarType.getCanonicalType().getAsOpaquePtr());
  }
};
} // end anonymous namespace

static llvm::Constant *
generateByrefCopyHelper(CodeGenFunction &CGF,
                        const llvm::StructType &byrefType,
                        CodeGenModule::ByrefHelpers &byrefInfo) {
  ASTContext &Context = CGF.getContext();

  QualType R = Context.VoidTy;

  FunctionArgList args;
  ImplicitParamDecl dst(0, SourceLocation(), 0, Context.VoidPtrTy);
  args.push_back(&dst);

  ImplicitParamDecl src(0, SourceLocation(), 0, Context.VoidPtrTy);
  args.push_back(&src);

  const CGFunctionInfo &FI =
    CGF.CGM.getTypes().getFunctionInfo(R, args, FunctionType::ExtInfo());

  CodeGenTypes &Types = CGF.CGM.getTypes();
  const llvm::FunctionType *LTy = Types.GetFunctionType(FI, false);

  // FIXME: We'd like to put these into a mergable by content, with
  // internal linkage.
  llvm::Function *Fn =
    llvm::Function::Create(LTy, llvm::GlobalValue::InternalLinkage,
                           "__Block_byref_object_copy_", &CGF.CGM.getModule());

  IdentifierInfo *II
    = &Context.Idents.get("__Block_byref_object_copy_");

  FunctionDecl *FD = FunctionDecl::Create(Context,
                                          Context.getTranslationUnitDecl(),
                                          SourceLocation(),
                                          SourceLocation(), II, R, 0,
                                          SC_Static,
                                          SC_None,
                                          false, true);

  CGF.StartFunction(FD, R, Fn, FI, args, SourceLocation());

  if (byrefInfo.needsCopy()) {
    const llvm::Type *byrefPtrType = byrefType.getPointerTo(0);

    // dst->x
    llvm::Value *destField = CGF.GetAddrOfLocalVar(&dst);
    destField = CGF.Builder.CreateLoad(destField);
    destField = CGF.Builder.CreateBitCast(destField, byrefPtrType);
    destField = CGF.Builder.CreateStructGEP(destField, 6, "x");

    // src->x
    llvm::Value *srcField = CGF.GetAddrOfLocalVar(&src);
    srcField = CGF.Builder.CreateLoad(srcField);
    srcField = CGF.Builder.CreateBitCast(srcField, byrefPtrType);
    srcField = CGF.Builder.CreateStructGEP(srcField, 6, "x");

    byrefInfo.emitCopy(CGF, destField, srcField);
  }  

  CGF.FinishFunction();

  return llvm::ConstantExpr::getBitCast(Fn, CGF.Int8PtrTy);
}

/// Build the copy helper for a __block variable.
static llvm::Constant *buildByrefCopyHelper(CodeGenModule &CGM,
                                            const llvm::StructType &byrefType,
                                            CodeGenModule::ByrefHelpers &info) {
  CodeGenFunction CGF(CGM);
  return generateByrefCopyHelper(CGF, byrefType, info);
}

/// Generate code for a __block variable's dispose helper.
static llvm::Constant *
generateByrefDisposeHelper(CodeGenFunction &CGF,
                           const llvm::StructType &byrefType,
                           CodeGenModule::ByrefHelpers &byrefInfo) {
  ASTContext &Context = CGF.getContext();
  QualType R = Context.VoidTy;

  FunctionArgList args;
  ImplicitParamDecl src(0, SourceLocation(), 0, Context.VoidPtrTy);
  args.push_back(&src);

  const CGFunctionInfo &FI =
    CGF.CGM.getTypes().getFunctionInfo(R, args, FunctionType::ExtInfo());

  CodeGenTypes &Types = CGF.CGM.getTypes();
  const llvm::FunctionType *LTy = Types.GetFunctionType(FI, false);

  // FIXME: We'd like to put these into a mergable by content, with
  // internal linkage.
  llvm::Function *Fn =
    llvm::Function::Create(LTy, llvm::GlobalValue::InternalLinkage,
                           "__Block_byref_object_dispose_",
                           &CGF.CGM.getModule());

  IdentifierInfo *II
    = &Context.Idents.get("__Block_byref_object_dispose_");

  FunctionDecl *FD = FunctionDecl::Create(Context,
                                          Context.getTranslationUnitDecl(),
                                          SourceLocation(),
                                          SourceLocation(), II, R, 0,
                                          SC_Static,
                                          SC_None,
                                          false, true);
  CGF.StartFunction(FD, R, Fn, FI, args, SourceLocation());

  if (byrefInfo.needsDispose()) {
    llvm::Value *V = CGF.GetAddrOfLocalVar(&src);
    V = CGF.Builder.CreateLoad(V);
    V = CGF.Builder.CreateBitCast(V, byrefType.getPointerTo(0));
    V = CGF.Builder.CreateStructGEP(V, 6, "x");

    byrefInfo.emitDispose(CGF, V);
  }

  CGF.FinishFunction();

  return llvm::ConstantExpr::getBitCast(Fn, CGF.Int8PtrTy);
}

/// Build the dispose helper for a __block variable.
static llvm::Constant *buildByrefDisposeHelper(CodeGenModule &CGM,
                                              const llvm::StructType &byrefType,
                                            CodeGenModule::ByrefHelpers &info) {
  CodeGenFunction CGF(CGM);
  return generateByrefDisposeHelper(CGF, byrefType, info);
}

/// 
template <class T> static T *buildByrefHelpers(CodeGenModule &CGM,
                                               const llvm::StructType &byrefTy,
                                               T &byrefInfo) {
  // Increase the field's alignment to be at least pointer alignment,
  // since the layout of the byref struct will guarantee at least that.
  byrefInfo.Alignment = std::max(byrefInfo.Alignment,
                              CharUnits::fromQuantity(CGM.PointerAlignInBytes));

  llvm::FoldingSetNodeID id;
  byrefInfo.Profile(id);

  void *insertPos;
  CodeGenModule::ByrefHelpers *node
    = CGM.ByrefHelpersCache.FindNodeOrInsertPos(id, insertPos);
  if (node) return static_cast<T*>(node);

  byrefInfo.CopyHelper = buildByrefCopyHelper(CGM, byrefTy, byrefInfo);
  byrefInfo.DisposeHelper = buildByrefDisposeHelper(CGM, byrefTy, byrefInfo);

  T *copy = new (CGM.getContext()) T(byrefInfo);
  CGM.ByrefHelpersCache.InsertNode(copy, insertPos);
  return copy;
}

CodeGenModule::ByrefHelpers *
CodeGenFunction::buildByrefHelpers(const llvm::StructType &byrefType,
                                   const AutoVarEmission &emission) {
  const VarDecl &var = *emission.Variable;
  QualType type = var.getType();

  if (const CXXRecordDecl *record = type->getAsCXXRecordDecl()) {
    const Expr *copyExpr = CGM.getContext().getBlockVarCopyInits(&var);
    if (!copyExpr && record->hasTrivialDestructor()) return 0;

    CXXByrefHelpers byrefInfo(emission.Alignment, type, copyExpr);
    return ::buildByrefHelpers(CGM, byrefType, byrefInfo);
  }

  // Otherwise, if we don't have a retainable type, there's nothing to do.
  // that the runtime does extra copies.
  if (!type->isObjCRetainableType()) return 0;

  Qualifiers qs = type.getQualifiers();

  // If we have lifetime, that dominates.
  if (Qualifiers::ObjCLifetime lifetime = qs.getObjCLifetime()) {
    assert(getLangOptions().ObjCAutoRefCount);

    switch (lifetime) {
    case Qualifiers::OCL_None: llvm_unreachable("impossible");

    // These are just bits as far as the runtime is concerned.
    case Qualifiers::OCL_ExplicitNone:
    case Qualifiers::OCL_Autoreleasing:
      return 0;

    // Tell the runtime that this is ARC __weak, called by the
    // byref routines.
    case Qualifiers::OCL_Weak: {
      ARCWeakByrefHelpers byrefInfo(emission.Alignment);
      return ::buildByrefHelpers(CGM, byrefType, byrefInfo);
    }

    // ARC __strong __block variables need to be retained.
    case Qualifiers::OCL_Strong:
      // Block-pointers need to be _Block_copy'ed, so we let the
      // runtime be in charge.  But we can't use the code below
      // because we don't want to set BYREF_CALLER, which will
      // just make the runtime ignore us.
      if (type->isBlockPointerType()) {
        BlockFieldFlags flags = BLOCK_FIELD_IS_BLOCK;
        ObjectByrefHelpers byrefInfo(emission.Alignment, flags);
        return ::buildByrefHelpers(CGM, byrefType, byrefInfo);

      // Otherwise, we transfer ownership of the retain from the stack
      // to the heap.
      } else {
        ARCStrongByrefHelpers byrefInfo(emission.Alignment);
        return ::buildByrefHelpers(CGM, byrefType, byrefInfo);
      }
    }
    llvm_unreachable("fell out of lifetime switch!");
  }

  BlockFieldFlags flags;
  if (type->isBlockPointerType()) {
    flags |= BLOCK_FIELD_IS_BLOCK;
  } else if (CGM.getContext().isObjCNSObjectType(type) || 
             type->isObjCObjectPointerType()) {
    flags |= BLOCK_FIELD_IS_OBJECT;
  } else {
    return 0;
  }

  if (type.isObjCGCWeak())
    flags |= BLOCK_FIELD_IS_WEAK;

  ObjectByrefHelpers byrefInfo(emission.Alignment, flags);
  return ::buildByrefHelpers(CGM, byrefType, byrefInfo);
}

unsigned CodeGenFunction::getByRefValueLLVMField(const ValueDecl *VD) const {
  assert(ByRefValueInfo.count(VD) && "Did not find value!");
  
  return ByRefValueInfo.find(VD)->second.second;
}

llvm::Value *CodeGenFunction::BuildBlockByrefAddress(llvm::Value *BaseAddr,
                                                     const VarDecl *V) {
  llvm::Value *Loc = Builder.CreateStructGEP(BaseAddr, 1, "forwarding");
  Loc = Builder.CreateLoad(Loc);
  Loc = Builder.CreateStructGEP(Loc, getByRefValueLLVMField(V),
                                V->getNameAsString());
  return Loc;
}

/// BuildByRefType - This routine changes a __block variable declared as T x
///   into:
///
///      struct {
///        void *__isa;
///        void *__forwarding;
///        int32_t __flags;
///        int32_t __size;
///        void *__copy_helper;       // only if needed
///        void *__destroy_helper;    // only if needed
///        char padding[X];           // only if needed
///        T x;
///      } x
///
const llvm::Type *CodeGenFunction::BuildByRefType(const VarDecl *D) {
  std::pair<const llvm::Type *, unsigned> &Info = ByRefValueInfo[D];
  if (Info.first)
    return Info.first;
  
  QualType Ty = D->getType();

  llvm::SmallVector<llvm::Type *, 8> types;
  
  llvm::StructType *ByRefType =
    llvm::StructType::createNamed(getLLVMContext(),
                                "struct.__block_byref_" + D->getNameAsString());
  
  // void *__isa;
  types.push_back(Int8PtrTy);
  
  // void *__forwarding;
  types.push_back(llvm::PointerType::getUnqual(ByRefType));
  
  // int32_t __flags;
  types.push_back(Int32Ty);
    
  // int32_t __size;
  types.push_back(Int32Ty);

  bool HasCopyAndDispose = getContext().BlockRequiresCopying(Ty);
  if (HasCopyAndDispose) {
    /// void *__copy_helper;
    types.push_back(Int8PtrTy);
    
    /// void *__destroy_helper;
    types.push_back(Int8PtrTy);
  }

  bool Packed = false;
  CharUnits Align = getContext().getDeclAlign(D);
  if (Align > getContext().toCharUnitsFromBits(Target.getPointerAlign(0))) {
    // We have to insert padding.
    
    // The struct above has 2 32-bit integers.
    unsigned CurrentOffsetInBytes = 4 * 2;
    
    // And either 2 or 4 pointers.
    CurrentOffsetInBytes += (HasCopyAndDispose ? 4 : 2) *
      CGM.getTargetData().getTypeAllocSize(Int8PtrTy);
    
    // Align the offset.
    unsigned AlignedOffsetInBytes = 
      llvm::RoundUpToAlignment(CurrentOffsetInBytes, Align.getQuantity());
    
    unsigned NumPaddingBytes = AlignedOffsetInBytes - CurrentOffsetInBytes;
    if (NumPaddingBytes > 0) {
      llvm::Type *Ty = llvm::Type::getInt8Ty(getLLVMContext());
      // FIXME: We need a sema error for alignment larger than the minimum of
      // the maximal stack alignment and the alignment of malloc on the system.
      if (NumPaddingBytes > 1)
        Ty = llvm::ArrayType::get(Ty, NumPaddingBytes);
    
      types.push_back(Ty);

      // We want a packed struct.
      Packed = true;
    }
  }

  // T x;
  types.push_back(ConvertTypeForMem(Ty));
  
  ByRefType->setBody(types, Packed);
  
  Info.first = ByRefType;
  
  Info.second = types.size() - 1;
  
  return Info.first;
}

/// Initialize the structural components of a __block variable, i.e.
/// everything but the actual object.
void CodeGenFunction::emitByrefStructureInit(const AutoVarEmission &emission) {
  // Find the address of the local.
  llvm::Value *addr = emission.Address;

  // That's an alloca of the byref structure type.
  const llvm::StructType *byrefType = cast<llvm::StructType>(
                 cast<llvm::PointerType>(addr->getType())->getElementType());

  // Build the byref helpers if necessary.  This is null if we don't need any.
  CodeGenModule::ByrefHelpers *helpers =
    buildByrefHelpers(*byrefType, emission);

  const VarDecl &D = *emission.Variable;
  QualType type = D.getType();

  llvm::Value *V;

  // Initialize the 'isa', which is just 0 or 1.
  int isa = 0;
  if (type.isObjCGCWeak())
    isa = 1;
  V = Builder.CreateIntToPtr(Builder.getInt32(isa), Int8PtrTy, "isa");
  Builder.CreateStore(V, Builder.CreateStructGEP(addr, 0, "byref.isa"));

  // Store the address of the variable into its own forwarding pointer.
  Builder.CreateStore(addr,
                      Builder.CreateStructGEP(addr, 1, "byref.forwarding"));

  // Blocks ABI:
  //   c) the flags field is set to either 0 if no helper functions are
  //      needed or BLOCK_HAS_COPY_DISPOSE if they are,
  BlockFlags flags;
  if (helpers) flags |= BLOCK_HAS_COPY_DISPOSE;
  Builder.CreateStore(llvm::ConstantInt::get(IntTy, flags.getBitMask()),
                      Builder.CreateStructGEP(addr, 2, "byref.flags"));

  CharUnits byrefSize = CGM.GetTargetTypeStoreSize(byrefType);
  V = llvm::ConstantInt::get(IntTy, byrefSize.getQuantity());
  Builder.CreateStore(V, Builder.CreateStructGEP(addr, 3, "byref.size"));

  if (helpers) {
    llvm::Value *copy_helper = Builder.CreateStructGEP(addr, 4);
    Builder.CreateStore(helpers->CopyHelper, copy_helper);

    llvm::Value *destroy_helper = Builder.CreateStructGEP(addr, 5);
    Builder.CreateStore(helpers->DisposeHelper, destroy_helper);
  }
}

void CodeGenFunction::BuildBlockRelease(llvm::Value *V, BlockFieldFlags flags) {
  llvm::Value *F = CGM.getBlockObjectDispose();
  llvm::Value *N;
  V = Builder.CreateBitCast(V, Int8PtrTy);
  N = llvm::ConstantInt::get(Int32Ty, flags.getBitMask());
  Builder.CreateCall2(F, V, N);
}

namespace {
  struct CallBlockRelease : EHScopeStack::Cleanup {
    llvm::Value *Addr;
    CallBlockRelease(llvm::Value *Addr) : Addr(Addr) {}

    void Emit(CodeGenFunction &CGF, Flags flags) {
      // Should we be passing FIELD_IS_WEAK here?
      CGF.BuildBlockRelease(Addr, BLOCK_FIELD_IS_BYREF);
    }
  };
}

/// Enter a cleanup to destroy a __block variable.  Note that this
/// cleanup should be a no-op if the variable hasn't left the stack
/// yet; if a cleanup is required for the variable itself, that needs
/// to be done externally.
void CodeGenFunction::enterByrefCleanup(const AutoVarEmission &emission) {
  // We don't enter this cleanup if we're in pure-GC mode.
  if (CGM.getLangOptions().getGCMode() == LangOptions::GCOnly)
    return;

  EHStack.pushCleanup<CallBlockRelease>(NormalAndEHCleanup, emission.Address);
}
