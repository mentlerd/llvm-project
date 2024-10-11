//===-- CPPLanguageRuntime.cpp---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <iostream>

#include <memory>

#include "CPPLanguageRuntime.h"
#include "CommandObjectCPlusPlus.h"
#include "VerboseTrapFrameRecognizer.h"

#include "llvm/ADT/StringRef.h"

#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Core/UniqueCStringMap.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/StackFrameRecognizer.h"
#include "lldb/Target/ThreadPlanRunToAddress.h"
#include "lldb/Target/ThreadPlanStepInRange.h"
#include "lldb/Utility/Timer.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE_ADV(CPPLanguageRuntime, CPPRuntime)

static ConstString g_this = ConstString("this");
// Artificial coroutine-related variables emitted by clang.
static ConstString g_promise = ConstString("__promise");
static ConstString g_coro_frame = ConstString("__coro_frame");

char CPPLanguageRuntime::ID = 0;

/// A frame recognizer that is installed to hide libc++ implementation
/// details from the backtrace.
class LibCXXFrameRecognizer : public StackFrameRecognizer {
  std::array<RegularExpression, 2> m_hidden_regex;
  RecognizedStackFrameSP m_hidden_frame;

  struct LibCXXHiddenFrame : public RecognizedStackFrame {
    bool ShouldHide() override { return true; }
  };

public:
  LibCXXFrameRecognizer()
      : m_hidden_regex{
            // internal implementation details in the `std::` namespace
            //    std::__1::__function::__alloc_func<void (*)(), std::__1::allocator<void (*)()>, void ()>::operator()[abi:ne200000]
            //    std::__1::__function::__func<void (*)(), std::__1::allocator<void (*)()>, void ()>::operator()
            //    std::__1::__function::__value_func<void ()>::operator()[abi:ne200000]() const
            //    std::__2::__function::__policy_invoker<void (int, int)>::__call_impl[abi:ne200000]<std::__2::__function::__default_alloc_func<int (*)(int, int), int (int, int)>>
            //    std::__1::__invoke[abi:ne200000]<void (*&)()>
            //    std::__1::__invoke_void_return_wrapper<void, true>::__call[abi:ne200000]<void (*&)()>
            RegularExpression{R"(^std::__[^:]*::__)"},
            // internal implementation details in the `std::ranges` namespace
            //    std::__1::ranges::__sort::__sort_fn_impl[abi:ne200000]<std::__1::__wrap_iter<int*>, std::__1::__wrap_iter<int*>, bool (*)(int, int), std::__1::identity>
            RegularExpression{R"(^std::__[^:]*::ranges::__)"},
        },
        m_hidden_frame(new LibCXXHiddenFrame()) {}

  std::string GetName() override { return "libc++ frame recognizer"; }

  lldb::RecognizedStackFrameSP
  RecognizeFrame(lldb::StackFrameSP frame_sp) override {
    if (!frame_sp)
      return {};
    const auto &sc = frame_sp->GetSymbolContext(lldb::eSymbolContextFunction);
    if (!sc.function)
      return {};

    // Check if we have a regex match
    for (RegularExpression &r : m_hidden_regex) {
      if (!r.Execute(sc.function->GetNameNoArguments()))
        continue;

      // Only hide this frame if the immediate caller is also within libc++.
      lldb::ThreadSP thread_sp = frame_sp->GetThread();
      if (!thread_sp)
        return {};
      lldb::StackFrameSP parent_frame_sp =
          thread_sp->GetStackFrameAtIndex(frame_sp->GetFrameIndex() + 1);
      if (!parent_frame_sp)
        return {};
      const auto &parent_sc =
          parent_frame_sp->GetSymbolContext(lldb::eSymbolContextFunction);
      if (!parent_sc.function)
        return {};
      if (parent_sc.function->GetNameNoArguments().GetStringRef().starts_with(
              "std::"))
        return m_hidden_frame;
    }

    return {};
  }
};

CPPLanguageRuntime::CPPLanguageRuntime(Process *process)
    : LanguageRuntime(process), m_itanium_runtime(process) {
  if (process) {
    process->GetTarget().GetFrameRecognizerManager().AddRecognizer(
        StackFrameRecognizerSP(new LibCXXFrameRecognizer()), {},
        std::make_shared<RegularExpression>("^std::__[^:]*::"),
        /*mangling_preference=*/Mangled::ePreferDemangledWithoutArguments,
        /*first_instruction_only=*/false);

    RegisterVerboseTrapFrameRecognizer(*process);
  }
}

bool CPPLanguageRuntime::IsAllowedRuntimeValue(ConstString name) {
  return name == g_this || name == g_promise || name == g_coro_frame;
}

llvm::Error CPPLanguageRuntime::GetObjectDescription(Stream &str,
                                                     ValueObject &object) {
  // C++ has no generic way to do this.
  return llvm::createStringError("C++ does not support object descriptions");
}

llvm::Error
CPPLanguageRuntime::GetObjectDescription(Stream &str, Value &value,
                                         ExecutionContextScope *exe_scope) {
  // C++ has no generic way to do this.
  return llvm::createStringError("C++ does not support object descriptions");
}

llvm::Expected<CPPLanguageRuntime::UnwrappedLibCppFunction>
CPPLanguageRuntime::UnwrapLibCppFunction(lldb::ValueObjectSP &valobj_sp) {
  if (!valobj_sp)
    return llvm::createStringError("valobj in invalid state");
  
  // std::function has many variants, try to disambiguate going from most
  // recent to oldest known implementation
  ValueObjectSP base_ptr;
  {
    ValueObjectSP outer_f = valobj_sp->GetChildMemberWithName("__f_");

    if (!base_ptr) {
      // git: 050b064f15ee56ee0b42c9b957a3dd0f32532394 __functional/function.h
      //
      // template<class _Rp, class ..._ArgTypes>
      // class function<_Rp(_ArgTypes...)> {
      // #ifndef _LIBCPP_ABI_OPTIMIZED_FUNCTION
      //   typedef __function::__value_func<_Rp(_ArgTypes...)> __func;
      // #else
      //   typedef __function::__policy_func<_Rp(_ArgTypes...)> __func;
      // #endif
      //
      //   __func __f_;
      // }
      //
      // template <class _Fp> class __value_func;
      // template <class _Fp> class __policy_func;
      //
      // template <class _Rp, class... _ArgTypes>
      // class __value_func<_Rp(_ArgTypes...)> {
      //   typename aligned_storage<3 * sizeof(void*)>::type __buf_;
      //
      //   typedef __base<_Rp(_ArgTypes...)> __func;
      //   __func* __f_;
      // }
      //
      // template <class _Rp, class... _ArgTypes>
      // class __policy_func<_Rp(_ArgTypes...)> {
      //   __policy_storage __buf_;
      //
      //   typedef __function::__policy_invoker<_Rp(_ArgTypes...)> __invoker;
      //   __invoker __invoker_;
      //
      //   const __policy* __policy_;
      // }
      if (auto outer_f = valobj_sp->GetChildMemberWithName("__f_")) {
        if (auto inner_f = outer_f->GetChildMemberWithName("__f_"))
          base_ptr = std::move(inner_f);
      } else if (valobj_sp->GetChildMemberWithName("__invoker_"))
        return llvm::createStringError("__policy_func implementation is "
                                       "not supported");
    }
    
    if (!base_ptr) {
      // git: 3e519524c118651123eecf60c2bbc5d65ad9bac3 include/__functional_03
      //
      // class function<_Rp()> {
      //   aligned_storage<3*sizeof(void*)>::type __buf_;
      //   __base<_Rp>* __f_;
      // }
      if (auto outer_f = valobj_sp->GetChildMemberWithName("__f_"))
        base_ptr = std::move(outer_f);
    }
    
    if (!base_ptr)
      return llvm::createStringError("unrecognized implementation");
  }

  // __base<...> is a pure virtual class with an interface to manage the
  // the wrapped value. This interface is implemented by partial specializations
  // of the __func<_Fp, _Alloc, ...> template where _Fp is the wrapped callable
  //
  // We'll try to extract the concrete __func type pointed to by base_ptr by
  // analysing it's vtable.
  Status status;
  ValueObjectSP base = base_ptr->Dereference(status);
  if (status.Fail())
    return llvm::createStringError("failed to dereference __base pointer");
  
  Address virtual_method_addr;
  CompilerType func_type;
  {
    ValueObjectSP vtable = base->GetVTable();

    llvm::Expected<uint32_t> num_entries = vtable->GetNumChildren();
    if (auto error = num_entries.takeError())
      return error;
    
    // __base is pure virtual, __func is final. All member function pointers
    // are equally good candidates to find the enclosing class.
    //
    // In practice the first two vtable entries point to artificial destructors
    // which the type system refuses to elaborate as their artificial
    // specifications are not added to the enclosing class' declaration
    // context.
    //
    // This causes various warnings, and don't get us any closer to the
    // concrete type thus we skip them.
    for (uint32_t idx = 2; idx < *num_entries; idx++) {
      ValueObjectSP entry = vtable->GetChildAtIndex(idx);
      
      // Points to a potentially interesting member function
      addr_t mfunc_load_addr = entry->GetValueAsUnsigned(0);
      if (!mfunc_load_addr)
        continue;
      
      if (!valobj_sp->GetTargetSP()->ResolveLoadAddress(mfunc_load_addr,
                                                        virtual_method_addr))
        continue;
      
      Function* func = virtual_method_addr.CalculateSymbolContextFunction();
      if (!func)
        continue;
      
      CompilerDeclContext decl_ctx = func->GetDeclContext();
      if (!decl_ctx.IsClassMethod())
        continue;
      
      // Member functions are contained in their enclosing class' decl context
      CompilerDeclContext enclosing_class = decl_ctx.GetDecl().GetDeclContext();
      if (!enclosing_class.IsValid())
        continue;
      
      func_type = enclosing_class.GetDecl().GetType();
      break;
    }
  }
  
  if (!func_type)
    return llvm::createStringError("failed to find suitable virtual function "
                                   "to determine __func type");
  
  ValueObjectSP func = base->Cast(func_type);
  if (!func)
    return llvm::createStringError("failed to cast __base to __func type");
  
  // Now that the __func is a known type we can dig for the wrapped callable
  CompilerType callable_type = func_type.GetTypeTemplateArgument(0);
  if (!callable_type)
    return llvm::createStringError("failed to get wrapped callable type from "
                                   "first template parameter");
  
  // git: 050b064f15ee56ee0b42c9b957a3dd0f32532394
  //
  // class __func<_Fp, _Alloc, _Rp(_ArgTypes...)>
  //     : __base<_Rp(_ArgTypes...)> {
  //   __alloc_func<_Fp, _Alloc, _Rp(_ArgTypes...)> __f_;
  // }
  //
  // class __alloc_func<_Fp, _Ap, _Rp(_ArgTypes...)> {
  //   __compressed_pair<_Fp, _Ap> __f_;
  // }
  //
  // class __compressed_pair : __compressed_pair_elem<_T1, 0>,
  //                           __compressed_pair_elem<_T2, 1> {
  // }
  //
  // struct __compressed_pair_elem {
  //   _Tp __value_;
  // }
  ValueObjectSP pair = func->GetChildAtNamePath({"__f_", "__f_"});
  if (!pair)
    return llvm::createStringError("__compressed_pair is not where expected");
  
  // The callable may be an empty class in which case the empty base class
  // optimization will eliminate it completely from the type hierarchy
  //
  // Serve a dummy value which for all intents and purposes is just as good
  if (callable_type.IsRecordType() && callable_type.GetNumFields() == 0)
    return UnwrappedLibCppFunction {
      .callable = valobj_sp->CreateValueObjectFromAddress(
        "__value_",
        pair->GetLoadAddress(),
        pair->GetExecutionContextRef(),
        callable_type
      ),
      .in_module = virtual_method_addr.CalculateSymbolContextModule()
    };
  
  ValueObjectSP elem0 = pair->GetChildAtIndex(0);
  if (!elem0)
    return llvm::createStringError("__compressed_pair element 0 not where "
                                   "expected");
  
  ValueObjectSP callable = elem0->GetChildMemberWithName("__value_");
  if (!callable)
    return llvm::createStringError("__compressed_pair element value not "
                                   "where expected");
  
  return UnwrappedLibCppFunction {
    .callable = std::move(callable),
    .in_module = virtual_method_addr.CalculateSymbolContextModule(),
  };
}

CPPLanguageRuntime::LibCppStdFunctionCallableInfo
CPPLanguageRuntime::FindLibCppStdFunctionCallableInfo(
  lldb::ValueObjectSP &valobj_sp) {
  LLDB_SCOPED_TIMER();

  LibCppStdFunctionCallableInfo optional_info;

  auto unwrap_r = UnwrapLibCppFunction(valobj_sp);
  if (unwrap_r.takeError())
    return optional_info;
  
  UnwrappedLibCppFunction& wrapped = unwrap_r.get();
  
  CompilerType callable_t = wrapped.callable->GetCompilerType();
  
  if (callable_t.IsFunctionPointerType() ||
      callable_t.IsMemberFunctionPointerType()) {
    // Target is a standard function pointer, or member function pointer.
    // In either case on Itanium both contain a function address
    AddressType addr_type;
    addr_t addr = wrapped.callable->GetPointerValue(&addr_type);
    
    if (!addr || addr == LLDB_INVALID_ADDRESS)
      return optional_info;
    
    if (addr_type != eAddressTypeLoad)
      return optional_info;
    
    Address callable_addr;
    if (!wrapped.in_module->ResolveFileAddress(addr, callable_addr))
      return optional_info;
    
    SymbolContext sc;
    wrapped.in_module->ResolveSymbolContextForAddress(callable_addr,
                                                      eSymbolContextSymbol |
                                                      eSymbolContextLineEntry,
                                                      sc);
    if (!sc.symbol)
      return optional_info;
    
    return LibCppStdFunctionCallableInfo {
      .callable_symbol = *sc.symbol,
      .callable_address = sc.symbol->GetAddress(),
      .callable_line_entry = sc.line_entry,
      .callable_case = LibCppStdFunctionCallableCase::FreeOrMemberFunction
    };
  } else if (callable_t.IsRecordType()) {
    // Target is a lambda, or a generic callable. Search for a single
    // operator() overload and assume it is the target
    std::optional<ConstString> mangled_func_name;
    
    for (uint32_t idx = 0; idx < callable_t.GetNumMemberFunctions(); idx++) {
      TypeMemberFunctionImpl mfunc = callable_t.GetMemberFunctionAtIndex(idx);
      
      if (mfunc.GetKind() != eMemberFunctionKindInstanceMethod)
        continue;
      
      if (mfunc.GetName() != "operator()")
        continue;
        
      if (mangled_func_name)
        return optional_info; // Cannot resolve ambiguous target
      
      mangled_func_name = mfunc.GetMangledName();
    }
    
    // Locate the symbol context corresponding to the target function
    SymbolContext sc;
    {
      // Limit our lookup to callable_type
      CompilerDeclContext decl_ctx = callable_t.GetTypeSystem()
        ->GetCompilerDeclContextForType(callable_t);
      
      SymbolContextList list;
      wrapped.in_module->FindFunctions(*mangled_func_name, decl_ctx,
                                       eFunctionNameTypeFull, {}, list);
      if (list.GetSize() != 1)
        return optional_info;
      
      list.GetContextAtIndex(0, sc);
    }
    
    // TODO: This feels a bit clunky, I am probably misusing the API?
    // FindFunctions returns me SymbolContexts with the .function set but not
    // .symbol ... At first glance it seemed like if we know the function there
    // must be a symbol too!
    if (!sc.function)
      return optional_info;
    
    Symbol* symbol = sc.function->GetAddressRange().GetBaseAddress()
      .CalculateSymbolContextSymbol();
    if (!symbol)
      return optional_info;
    
    return LibCppStdFunctionCallableInfo {
      .callable_symbol = *symbol,
      .callable_address = symbol->GetAddress(),
      .callable_line_entry = sc.GetFunctionStartLineEntry(),
      
      // TODO: Can't tell lambdas apart from generic callables.. do we really
      //  need to? Is it important to have the correct qualification in the
      // summary?
      .callable_case = LibCppStdFunctionCallableCase::Lambda
    };
  }
  
  // Unrecognized callable type
  return optional_info;
}

lldb::ThreadPlanSP
CPPLanguageRuntime::GetStepThroughTrampolinePlan(Thread &thread,
                                                 bool stop_others) {
  ThreadPlanSP ret_plan_sp;

  lldb::addr_t curr_pc = thread.GetRegisterContext()->GetPC();

  TargetSP target_sp(thread.CalculateTarget());

  if (!target_sp->HasLoadedSections())
    return ret_plan_sp;

  Address pc_addr_resolved;
  SymbolContext sc;
  Symbol *symbol;

  if (!target_sp->ResolveLoadAddress(curr_pc, pc_addr_resolved))
    return ret_plan_sp;

  target_sp->GetImages().ResolveSymbolContextForAddress(
      pc_addr_resolved, eSymbolContextEverything, sc);
  symbol = sc.symbol;

  if (symbol == nullptr)
    return ret_plan_sp;

  llvm::StringRef function_name(symbol->GetName().GetCString());

  // Handling the case where we are attempting to step into std::function.
  // The behavior will be that we will attempt to obtain the wrapped
  // callable via FindLibCppStdFunctionCallableInfo() and if we find it we
  // will return a ThreadPlanRunToAddress to the callable. Therefore we will
  // step into the wrapped callable.
  //
  bool found_expected_start_string =
      function_name.starts_with("std::__1::function<");

  if (!found_expected_start_string)
    return ret_plan_sp;

  AddressRange range_of_curr_func;
  sc.GetAddressRange(eSymbolContextEverything, 0, false, range_of_curr_func);

  StackFrameSP frame = thread.GetStackFrameAtIndex(0);

  if (frame) {
    Address func_start_address =
        sc.function ? sc.function->GetAddress() : symbol->GetAddress();
    lldb::addr_t func_start =
        func_start_address.GetLoadAddress(target_sp.get());

    if (func_start == LLDB_INVALID_ADDRESS)
      return ret_plan_sp;

    // Advance past the prologue if we stopped there.
    uint32_t prologue_size = sc.function ? sc.function->GetPrologueByteSize()
                                         : symbol->GetPrologueByteSize();
    if (curr_pc < func_start + prologue_size) {
      func_start_address.Slide(prologue_size);
      return std::make_shared<ThreadPlanRunToAddress>(
          thread, func_start_address, stop_others);
    }

    ValueObjectSP value_sp = frame->FindVariable(g_this);

    CPPLanguageRuntime::LibCppStdFunctionCallableInfo callable_info =
        FindLibCppStdFunctionCallableInfo(value_sp);

    if (callable_info.callable_case != LibCppStdFunctionCallableCase::Invalid &&
        value_sp->GetValueIsValid()) {
      // We found the std::function wrapped callable and we have its address.
      // We now create a ThreadPlan to run to the callable.
      ret_plan_sp = std::make_shared<ThreadPlanRunToAddress>(
          thread, callable_info.callable_address, stop_others);
      return ret_plan_sp;
    } else {
      // We are in std::function but we could not obtain the callable.
      // We create a ThreadPlan to keep stepping through using the address range
      // of the current function.
      ret_plan_sp = std::make_shared<ThreadPlanStepInRange>(
          thread, range_of_curr_func, sc, nullptr, eOnlyThisThread,
          eLazyBoolYes, eLazyBoolYes);
      return ret_plan_sp;
    }
  }

  return ret_plan_sp;
}

bool CPPLanguageRuntime::IsSymbolARuntimeThunk(const Symbol &symbol) {
  llvm::StringRef mangled_name =
      symbol.GetMangled().GetMangledName().GetStringRef();
  // Virtual function overriding from a non-virtual base use a "Th" prefix.
  // Virtual function overriding from a virtual base must use a "Tv" prefix.
  // Virtual function overriding thunks with covariant returns use a "Tc"
  // prefix.
  return mangled_name.starts_with("_ZTh") || mangled_name.starts_with("_ZTv") ||
         mangled_name.starts_with("_ZTc");
}

bool CPPLanguageRuntime::CouldHaveDynamicValue(ValueObject &in_value) {
  const bool check_cxx = true;
  const bool check_objc = false;
  return in_value.GetCompilerType().IsPossibleDynamicType(nullptr, check_cxx,
                                                          check_objc);
}

bool CPPLanguageRuntime::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &dynamic_address,
    Value::ValueType &value_type, llvm::ArrayRef<uint8_t> &local_buffer) {
  class_type_or_name.Clear();
  value_type = Value::ValueType::Scalar;

  if (!CouldHaveDynamicValue(in_value))
    return false;

  return m_itanium_runtime.GetDynamicTypeAndAddress(
      in_value, use_dynamic, class_type_or_name, dynamic_address, value_type);
}

TypeAndOrName
CPPLanguageRuntime::FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                     ValueObject &static_value) {
  CompilerType static_type(static_value.GetCompilerType());
  Flags static_type_flags(static_type.GetTypeInfo());

  TypeAndOrName ret(type_and_or_name);
  if (type_and_or_name.HasType()) {
    // The type will always be the type of the dynamic object.  If our parent's
    // type was a pointer, then our type should be a pointer to the type of the
    // dynamic object.  If a reference, then the original type should be
    // okay...
    CompilerType orig_type = type_and_or_name.GetCompilerType();
    CompilerType corrected_type = orig_type;
    if (static_type_flags.AllSet(eTypeIsPointer))
      corrected_type = orig_type.GetPointerType();
    else if (static_type_flags.AllSet(eTypeIsReference))
      corrected_type = orig_type.GetLValueReferenceType();
    ret.SetCompilerType(corrected_type);
  } else {
    // If we are here we need to adjust our dynamic type name to include the
    // correct & or * symbol
    std::string corrected_name(type_and_or_name.GetName().GetCString());
    if (static_type_flags.AllSet(eTypeIsPointer))
      corrected_name.append(" *");
    else if (static_type_flags.AllSet(eTypeIsReference))
      corrected_name.append(" &");
    // the parent type should be a correctly pointer'ed or referenc'ed type
    ret.SetCompilerType(static_type);
    ret.SetName(corrected_name.c_str());
  }
  return ret;
}

LanguageRuntime *
CPPLanguageRuntime::CreateInstance(Process *process,
                                   lldb::LanguageType language) {
  if (language == eLanguageTypeC_plus_plus ||
      language == eLanguageTypeC_plus_plus_03 ||
      language == eLanguageTypeC_plus_plus_11 ||
      language == eLanguageTypeC_plus_plus_14)
    return new CPPLanguageRuntime(process);
  else
    return nullptr;
}

void CPPLanguageRuntime::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(), "C++ language runtime", CreateInstance,
      [](CommandInterpreter &interpreter) -> lldb::CommandObjectSP {
        return CommandObjectSP(new CommandObjectCPlusPlus(interpreter));
      });
}

void CPPLanguageRuntime::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

llvm::Expected<LanguageRuntime::VTableInfo>
CPPLanguageRuntime::GetVTableInfo(ValueObject &in_value, bool check_type) {
  return m_itanium_runtime.GetVTableInfo(in_value, check_type);
}

BreakpointResolverSP
CPPLanguageRuntime::CreateExceptionResolver(const BreakpointSP &bkpt,
                                            bool catch_bp, bool throw_bp) {
  return CreateExceptionResolver(bkpt, catch_bp, throw_bp, false);
}

BreakpointResolverSP
CPPLanguageRuntime::CreateExceptionResolver(const BreakpointSP &bkpt,
                                            bool catch_bp, bool throw_bp,
                                            bool for_expressions) {
  std::vector<const char *> exception_names;
  m_itanium_runtime.AppendExceptionBreakpointFunctions(
      exception_names, catch_bp, throw_bp, for_expressions);

  BreakpointResolverSP resolver_sp(new BreakpointResolverName(
      bkpt, exception_names.data(), exception_names.size(),
      eFunctionNameTypeBase, eLanguageTypeUnknown, 0, eLazyBoolNo));

  return resolver_sp;
}

lldb::SearchFilterSP CPPLanguageRuntime::CreateExceptionSearchFilter() {
  Target &target = m_process->GetTarget();

  FileSpecList filter_modules;
  m_itanium_runtime.AppendExceptionBreakpointFilterModules(filter_modules,
                                                           target);
  return target.GetSearchFilterForModuleList(&filter_modules);
}

lldb::BreakpointSP CPPLanguageRuntime::CreateExceptionBreakpoint(
    bool catch_bp, bool throw_bp, bool for_expressions, bool is_internal) {
  Target &target = m_process->GetTarget();
  FileSpecList filter_modules;
  BreakpointResolverSP exception_resolver_sp =
      CreateExceptionResolver(nullptr, catch_bp, throw_bp, for_expressions);
  SearchFilterSP filter_sp(CreateExceptionSearchFilter());
  const bool hardware = false;
  const bool resolve_indirect_functions = false;
  return target.CreateBreakpoint(filter_sp, exception_resolver_sp, is_internal,
                                 hardware, resolve_indirect_functions);
}

void CPPLanguageRuntime::SetExceptionBreakpoints() {
  if (!m_process)
    return;

  const bool catch_bp = false;
  const bool throw_bp = true;
  const bool is_internal = true;
  const bool for_expressions = true;

  // For the exception breakpoints set by the Expression parser, we'll be a
  // little more aggressive and stop at exception allocation as well.

  if (m_cxx_exception_bp_sp) {
    m_cxx_exception_bp_sp->SetEnabled(true);
  } else {
    m_cxx_exception_bp_sp = CreateExceptionBreakpoint(
        catch_bp, throw_bp, for_expressions, is_internal);
    if (m_cxx_exception_bp_sp)
      m_cxx_exception_bp_sp->SetBreakpointKind("c++ exception");
  }
}

void CPPLanguageRuntime::ClearExceptionBreakpoints() {
  if (!m_process)
    return;

  if (m_cxx_exception_bp_sp) {
    m_cxx_exception_bp_sp->SetEnabled(false);
  }
}

bool CPPLanguageRuntime::ExceptionBreakpointsAreSet() {
  return m_cxx_exception_bp_sp && m_cxx_exception_bp_sp->IsEnabled();
}

bool CPPLanguageRuntime::ExceptionBreakpointsExplainStop(
    lldb::StopInfoSP stop_reason) {
  if (!m_process)
    return false;

  if (!stop_reason || stop_reason->GetStopReason() != eStopReasonBreakpoint)
    return false;

  uint64_t break_site_id = stop_reason->GetValue();
  return m_process->GetBreakpointSiteList().StopPointSiteContainsBreakpoint(
      break_site_id, m_cxx_exception_bp_sp->GetID());
}

lldb::ValueObjectSP
CPPLanguageRuntime::GetExceptionObjectForThread(lldb::ThreadSP thread_sp) {
  return m_itanium_runtime.GetExceptionObjectForThread(std::move(thread_sp));
}
