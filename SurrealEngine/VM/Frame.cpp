
#include "Precomp.h"
#include "Frame.h"
#include "Bytecode.h"
#include "ExpressionEvaluator.h"
#include "NativeFunc.h"
#include "Packages/Core/UTextBuffer.h"
#include "Packages/Core/UFunction.h"
#include "Packages/Engine/Subsystems/USurrealAudioDevice.h"
#include "Engine.h"
#include "Package/PackageManager.h"
#include "Utils/AlignedAlloc.h"
#include "Commandlet/VM/DisassemblyCommandlet.h"
#include <cassert>

std::function<void()> Frame::RunDebugger;
Array<Breakpoint> Frame::Breakpoints;
Array<Trace> Frame::Traces;
Array<Frame*> Frame::Callstack;
FrameRunState Frame::RunState = FrameRunState::Running;
Frame* Frame::StepFrame = nullptr;
Expression* Frame::StepExpression = nullptr;
std::string Frame::ExceptionText;
bool Frame::BreakOnError;
std::unique_ptr<Iterator> Frame::CreatedIterator;

Frame::Frame(UObject* instance, UStruct* func, size_t statementIndex, bool isStateFrame) :
	Object(instance), Func(func), StatementIndex(statementIndex), IsStateFrame(isStateFrame)
{
	InitVariables();
}

Frame::~Frame()
{
	DeinitVariables();
}

void Frame::SetState(UStruct* func)
{
	DeinitVariables();
	Func = func;
	InitVariables();
}

#ifdef __N64__
// N64 port: 8 MiB RDRAM; high-water mark checked at boot (UNREAL-PORT.md "Linked size")
#define VM_VARIABLE_STACK_CAPACITY (256*1024)
#else
#define VM_VARIABLE_STACK_CAPACITY (1024*1024)
#endif
static Array<size_t> VariableStackAllocations;
static size_t VariableStackOffset = 0;
static char VariableStack[VM_VARIABLE_STACK_CAPACITY];

static void* VMVariableStackAlloc(size_t alignment, size_t size)
{
	char* current_ptr = VariableStack + VariableStackOffset;
	size_t space = VM_VARIABLE_STACK_CAPACITY - VariableStackOffset;
	void* aligned_ptr = current_ptr;
	if (std::align(alignment, size, aligned_ptr, space))
	{
		VariableStackAllocations.push_back(VariableStackOffset);
		VariableStackOffset = static_cast<char*>(aligned_ptr) - VariableStack + size;
		return aligned_ptr;
	}
	return nullptr;
}

void VMFreeVariables()
{
	VariableStackOffset = VariableStackAllocations.back();
	VariableStackAllocations.pop_back();
}

void Frame::InitVariables()
{
	if (Func)
	{
		if (IsStateFrame)
			Variables = AlignedAlloc(Func->StructAlignment, Func->StructSize);
		else
			Variables = VMVariableStackAlloc(Func->StructAlignment, Func->StructSize);

		if (!Func->IsNativeFunc)
		{
			for (UProperty* prop : Func->Properties)
			{
				prop->ConstructArray(static_cast<uint8_t*>(Variables) + prop->DataOffset.DataOffset);
			}
		}
		else
		{
			auto v = (char*)Variables;
			for (size_t i = 0, count = Func->Properties.size(); i < count; i++)
			{
				new(v + i * sizeof(ExpressionValue)) ExpressionValue();
			}
		}
	}
}

void Frame::DeinitVariables()
{
	if (Variables)
	{
		if (!Func->IsNativeFunc)
		{
			for (UProperty* prop : Func->Properties)
			{
				prop->DestructArray(static_cast<uint8_t*>(Variables) + prop->DataOffset.DataOffset);
			}
		}
		else
		{
			auto v = (ExpressionValue*)Variables;
			for (size_t i = 0, count = Func->Properties.size(); i < count; i++)
			{
				v[i].~ExpressionValue();
			}
		}
		if (IsStateFrame)
			AlignedFree(Variables);
		else
			VMFreeVariables();
		Variables = nullptr;
	}
}

void Frame::AddTrace(const NameString& func, std::function<void(UFunction* func, UObject* instance, const ArrayView<ExpressionValue>& args)> callback)
{
	Trace trace;
	trace.Function = func;
	trace.Callback = callback;
	Traces.push_back(trace);
}

bool Frame::AddBreakpoint(const NameString& clsName, const NameString& funcName, const NameString& stateName, int statementIndex)
{
	Breakpoint bp;
	bp.Class = clsName;
	bp.Function = funcName;
	bp.State = stateName;

	UClass* cls = engine->packages->FindClass(clsName);
	if (!cls)
		return false;

	if (stateName.IsNone())
	{
		for (UField* child = cls->Children; child; child = child->Next)
		{
			if (child->Name == funcName && UObject::IsType<UFunction>(child))
			{
				UFunction* func = UObject::Cast<UFunction>(child);
				if (statementIndex < 0 || (size_t)statementIndex >= func->Code->Statements.size())
					return false;
				bp.Expr = func->Code->Statements[statementIndex];
				Breakpoints.push_back(bp);
				return true;
			}
		}
	}
	else
	{
		for (UField* child = cls->Children; child; child = child->Next)
		{
			if (child->Name == stateName && UObject::IsType<UState>(child))
			{
				UState* state = UObject::Cast<UState>(child);
				for (UField* stateChild = state->Children; stateChild; stateChild = stateChild->Next)
				{
					if (stateChild->Name == funcName && UObject::IsType<UFunction>(stateChild))
					{
						UFunction* func = UObject::Cast<UFunction>(stateChild);
						if (statementIndex < 0 || (size_t)statementIndex >= func->Code->Statements.size())
							return false;
						bp.Expr = func->Code->Statements[statementIndex];
						Breakpoints.push_back(bp);
						return true;
					}
				}
			}
		}
	}
	return false;
}

void Frame::Break()
{
	RunState = FrameRunState::DebugBreak;

	if (RunDebugger)
	{
		engine->audiodev->BreakpointTriggered();
		RunDebugger();
	}
	else
	{
		if (!ExceptionText.empty())
		{
			std::string callstack = Frame::GetCallstack();
			std::string message = "Script execution error:\r\n\r\n";
			message += ExceptionText;
			message += "\r\n\r\nCall stack:\r\n\r\n" + callstack;
			Exception::Throw(message);
		}
	}
}

void Frame::Resume()
{
	RunState = FrameRunState::Running;
}

void Frame::StepInto()
{
	StepFrame = Callstack.back();
	RunState = FrameRunState::StepInto;
}

void Frame::StepOver()
{
	StepFrame = Callstack.back();
	RunState = FrameRunState::StepOver;
}

void Frame::StepOut()
{
	StepFrame = Callstack.back();
	RunState = FrameRunState::StepOut;
}

void Frame::ThrowException(const std::string& text)
{
#if defined(_DEBUG) && defined(WIN32)
	DebugBreak();
#endif

	ExceptionText = text;
	Break();
}

std::string debugCallstack;
const char* GetCallStack()
{
	debugCallstack = Frame::GetCallstack();
	return debugCallstack.c_str();
}

std::string Frame::GetName()
{
	std::string name;
	if (Func)
	{
		for (UStruct* s = Func; s != nullptr; s = s->StructParent)
		{
			if (name.empty())
				name = s->Name.ToString();
			else
				name = s->Name.ToString() + "." + name;
		}
	}
	return name;
}

std::string Frame::GetDisassembly(Expression* statement)
{
	std::string result;
	PrintPrettyExpression::Print([&](const std::string& text) { result += text; }, statement);
	return result;
}

std::string Frame::GetCallstack()
{
	std::string result;

#ifdef WIN32
	std::string newline = "\r\n";
#else
	std::string newline = "\n";
#endif

	for (auto it = Callstack.rbegin(); it != Callstack.rend(); ++it)
	{
		Frame* frame = *it;
		std::string name = frame->GetName();
		if (UStruct* func = frame->Func)
		{
			if (frame->StatementIndex > 0) // StatementIndex points at the NEXT statement to be executed
			{
				int line = func->GetStatementLine(func->Code->Statements[frame->StatementIndex - 1]);
				if (line > 0)
				{
					name += " line " + std::to_string(line);
				}
				else
				{
					name += " line " + std::to_string(func->Line) + " [debug info failed]";
					line = func->GetStatementLine(func->Code->Statements[frame->StatementIndex - 1]);
				}
				name += ": ";
				name += GetDisassembly(func->Code->Statements[frame->StatementIndex - 1]);
			}
			else
			{
				name += " line " + std::to_string(func->Line);
			}
		}
		if (!result.empty()) result += newline;
		result += "at " + name;
	}
	return result;
}

#ifdef __N64__
#include "ScriptCall.h"
// N64 port: per-UFunction call info, filled on first call (UFunctions live as long as the packages)
struct K64FuncInfo
{
	UFunction* Func;
	short Event;   // EventName, -1 = not an event name
	short NumArgs; // parms that are not the return value
	short RetPos;  // -1 no return parm, -2 return parm not last (no fast native path)
	UProperty* RetProp;
	NativeFuncHandler* ByName;
};
static K64FuncInfo k64FuncInfo[1024];

static K64FuncInfo& K64GetFuncInfo(UFunction* func)
{
	uintptr_t h = (uintptr_t)func;
	K64FuncInfo& fi = k64FuncInfo[((h >> 4) ^ (h >> 14)) & 1023];
	if (fi.Func != func)
	{
		fi.Func = func;
		EventName ev = {};
		fi.Event = NameStringToEventName(func->Name, ev) ? (short)(int)ev : (short)-1;
		fi.NumArgs = 0;
		fi.RetPos = -1;
		fi.RetProp = nullptr;
		fi.ByName = nullptr;
		int pos = 0;
		for (UProperty* prop : func->Properties)
		{
			if (!AllFlags(prop->PropFlags, PropertyFlags::Parm))
				continue;
			if (fi.RetPos != -1)
				fi.RetPos = -2; // a parm after the return value
			if (AllFlags(prop->PropFlags, PropertyFlags::ReturnParm))
			{
				if (fi.RetPos == -1)
					fi.RetPos = (short)pos;
				fi.RetProp = prop;
			}
			else
				fi.NumArgs++;
			pos++;
		}
		if (AllFlags(func->FuncFlags, FunctionFlags::Native) && func->NativeFuncIndex == 0 && func->NativeStruct)
			fi.ByName = &NativeFunctions::NativeByName[{ func->Name, func->NativeStruct->Name }];
	}
	return fi;
}

// Same answer as instance->IsEventEnabled(func->Name) without the name -> event hash per call
static inline bool K64EventEnabled(UObject* instance, UFunction* func, const K64FuncInfo& fi)
{
	if (fi.Event >= 0)
		return instance->IsEventEnabled((EventName)fi.Event);
	if (!instance->DisabledEvents.m)
		return true;
	return instance->IsEventEnabled(func->Name);
}
#endif

ExpressionValue Frame::Call(UFunction* func, UObject* instance, const ArrayView<ExpressionValue>& args)
{
#ifdef __N64__
	if (!func)
	{
		LogMessage("Call with null function");
		LogMessage(Frame::GetCallstack());
		return ExpressionValue::NothingValue();
	}
#endif
	if (!instance)
	{
		LogMessage("Accessed None when calling " + func->Name.ToString());
		LogMessage(Frame::GetCallstack());
		return ExpressionValue::NothingValue();
	}

#ifdef __N64__
	if (!Traces.empty())
		TraceCall(func, instance, args);

	if (!K64EventEnabled(instance, func, K64GetFuncInfo(func)))
#else
	TraceCall(func, instance, args);

	if (!instance->IsEventEnabled(func->Name))
#endif
	{
		return ExpressionValue::NothingValue();
	}

	if (AllFlags(func->FuncFlags, FunctionFlags::Native))
	{
		return CallNative(func, instance, args);
	}
	else
	{
		Frame frame(instance, func, 0, false);

		// Store args in function frame local variables
		int argindex = 0;
		for (UProperty* prop : func->Properties)
		{
			ExpressionValue lvalue = ExpressionValue::Variable(frame.Variables, prop);
			if (AllFlags(prop->PropFlags, PropertyFlags::Parm))
			{
				if (argindex < args.size())
				{
					lvalue.Store(args[argindex]);
				}
				else
				{
					// Trailing optional args may be missing.
					lvalue.Store(ExpressionValue::DefaultValue(prop));
				}

				argindex++;
			}
		}

		// Run the function
		ExpressionValue result = frame.Run().Value;

		// Load the result from the frame local result variable
		result.Load();

		// Copy out params from frame local variables
		argindex = 0;
		for (UProperty* prop : func->Properties)
		{
			ExpressionValue lvalue = ExpressionValue::Variable(frame.Variables, prop);

			if (AllFlags(prop->PropFlags, PropertyFlags::Parm | PropertyFlags::OutParm) && argindex < args.size())
			{
				args[argindex].Store(lvalue);
			}

			if (AllFlags(prop->PropFlags, PropertyFlags::ReturnParm) && result.GetType() == ExpressionValueType::Nothing)
			{
				result = ExpressionValue::DefaultValue(prop);
			}

			if (AllFlags(prop->PropFlags, PropertyFlags::Parm))
				argindex++;
		}

		return result;
	}
}

ExpressionValue Frame::CallNative(UFunction* func, UObject* instance, const ArrayView<ExpressionValue>& args)
{
	Frame frame(instance, func, 0, false);
	ActiveCallStackFrame activeFrame(&frame);

	auto frameArgs = (ExpressionValue*)frame.Variables;
	int argindex = 0;
	for (UProperty* prop : func->Properties)
	{
		if (AllFlags(prop->PropFlags, PropertyFlags::Parm))
		{
			if (AllFlags(prop->PropFlags, PropertyFlags::ReturnParm))
			{
				frameArgs[argindex++] = ExpressionValue::PropertyValue(prop);
			}
			else if (argindex < args.size())
			{
				frameArgs[argindex++] = args[argindex];
			}
			else
			{
				// Trailing optional args may be missing.
				frameArgs[argindex++] = ExpressionValue::NothingValue();
			}
		}
	}

	NativeFuncHandler* callback = nullptr;
	if (func->NativeFuncIndex != 0)
		callback = &NativeFunctions::NativeByIndex[func->NativeFuncIndex];
	else
		callback = &NativeFunctions::NativeByName[{ func->Name, func->NativeStruct->Name }];

#ifdef __N64__
	if (callback && !*callback)
	{
		static int unn = 0;
		if (unn++ < 60)
			printf("[ue] native sin handler %s.%s (ignorada)\n", func->NativeStruct ? func->NativeStruct->Name.ToString().c_str() : "?", func->Name.ToString().c_str());
		return ExpressionValue::NothingValue();
	}
#endif
	if (callback)
	{
		try
		{
			(*callback)(instance, frameArgs);
		}
		catch (const std::exception& e)
		{
#ifdef __N64__
			{
				int dbgi = 0;
				printf("[ue] native %s args %d:", func->Name.ToString().c_str(), (int)args.size());
				for (UProperty* p : func->Properties)
					printf(" [%s vt=%d got=%d]", p->Name.ToString().c_str(), (int)p->ValueType, (int)frameArgs[dbgi++].GetType());
				printf("\n");
			}
#endif
			LogMessage(std::string("Script error: ") + e.what());
			return ExpressionValue::NothingValue();
		}
		catch (...)
		{
			LogMessage("Script error: Unknown error");
			return ExpressionValue::NothingValue();
		}
	}
	else
	{
		Exception::Throw("Unknown native function " + func->NativeStruct->Name.ToString() + "." + func->Name.ToString());
	}

	ExpressionValue result = ExpressionValue::NothingValue();
	argindex = 0;
	for (UProperty* prop : func->Properties)
	{
		if (AllFlags(prop->PropFlags, PropertyFlags::Parm))
		{
			if (AllFlags(prop->PropFlags, PropertyFlags::ReturnParm))
			{
				result = frameArgs[argindex];
				break;
			}
			argindex++;
		}
	}
	return result;
}

void Frame::TraceCall(UFunction* func, UObject* instance, const ArrayView<ExpressionValue>& args)
{
	for (const auto& trace : Traces)
	{
		if (trace.Enabled && trace.Function == func->Name)
			trace.Callback(func, instance, args);
	}
}

void Frame::GotoLabel(const NameString& label)
{
	for (UClass* cls = Object->Class; cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
	{
		UState* state = cls->GetState(Func->Name);
		if (state)
		{
			int labelIndex = state->Code->FindLabelIndex(label.IsNone() ? NameString("Begin") : label);
			if (labelIndex != -1)
			{
				Func = state;
				StatementIndex = labelIndex;
				LatentState = LatentRunState::Continue;
				return;
			}
		}
	}
	LatentState = LatentRunState::Stop;
}

void Frame::Tick()
{
	if (LatentState == LatentRunState::Continue)
		Run();
}

#ifdef __N64__
// Jump target cache: (bytecode, offset) -> statement index (Bytecode::FindStatementIndex is a map)
struct K64JEntry
{
	::Bytecode* Code;
	int Offset;
	int Index;
};
static K64JEntry k64JCache[1024];

static int K64FindStatement(::Bytecode* code, uint16_t offset)
{
	K64JEntry& je = k64JCache[(((uintptr_t)code >> 4) ^ offset) & 1023];
	if (je.Code == code && je.Offset == (int)offset)
		return je.Index;
	int index = code->FindStatementIndex(offset);
	je.Code = code;
	je.Offset = offset;
	je.Index = index;
	return index;
}
#endif

ExpressionEvalResult Frame::Run()
{
	if (!Func)
		return {};

	ActiveCallStackFrame activeFrame(this);

	if (!Func->Code->Statements.empty())
		StepExpression = Func->Code->Statements[StatementIndex];

	if (RunState == FrameRunState::StepInto)
	{
		// We entered a new function. Break for step into.
		Break();
	}

	const int maxInstructions = 500'000;
	int instructionsRetired = 0;
	while (true)
	{
#ifdef __N64__
		::Bytecode* k64code = Func->Code.get();
		if (StatementIndex >= k64code->Statements.size())
#else
		if (StatementIndex >= Func->Code->Statements.size())
#endif
			ThrowException("Unexpected end of code statements");

		// Note: GotoState may change StatementIndex (jump to a different location) so we have to increment the index before executing the statement
		size_t curStatementIndex = StatementIndex;
		StatementIndex++;

#ifdef __N64__
		StepExpression = k64code->Statements[curStatementIndex];
#else
		StepExpression = Func->Code->Statements[curStatementIndex];
#endif

		if (instructionsRetired >= maxInstructions)
		{
			LogMessage("Too many VM instructions executed in a single tick");
			Break();
		}
		else if ((RunState == FrameRunState::StepOver || RunState == FrameRunState::StepInto) && StepFrame == this)
		{
			// We are running a new expression. Break on step over, but also step into as there might not been a function to step into.
			Break();
		}
		else if (RunState == FrameRunState::StepOut && StepFrame == nullptr)
		{
			// We found the function exit point. Break the debugger.
			Break();
		}

#ifdef __N64__
		Expression* statement = k64code->Statements[curStatementIndex];
#else
		Expression* statement = Func->Code->Statements[curStatementIndex];
#endif
		//ExpressionEvalResult result = ExpressionEvaluator::Eval(statement, Object, Object, Variables);
		ExpressionEvalResult result = RunExpr(statement, Object, Object, Variables);
		if (!Func)
			return result;
		switch (result.Result)
		{
		case StatementResult::Next:
			break;
		case StatementResult::Jump:
#ifdef __N64__
			StatementIndex = K64FindStatement(Func->Code.get(), result.JumpAddress);
#else
			StatementIndex = Func->Code->FindStatementIndex(result.JumpAddress);
#endif
			break;
		case StatementResult::Switch:
			ProcessSwitch(result.Value);
			break;
		case StatementResult::GotoLabel:
			{
				int index = Func->Code->FindLabelIndex(result.Label);
				if (index != -1)
				{
					StatementIndex = index;
				}
				else
				{
					// State gotos can jump to a parent state block!
					bool found = false;
					for (UClass* cls = Object->Class; cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
					{
						UState* state = cls->GetState(Func->Name);
						if (state)
						{
							int labelIndex = state->Code->FindLabelIndex(result.Label);
							if (labelIndex != -1)
							{
								Func = state;
								StatementIndex = labelIndex;
								found = true;
								break;
							}
						}
					}
					if (!found)
					{
						if (Object->Class)
							LogMessage(Object->Class->Name.ToString() + "." + Func->Name.ToString() + ": GotoLabel (" + result.Label.ToString() + "): Label not found");
						LatentState = LatentRunState::Stop;
						result.Result = StatementResult::Stop;
						return result;
					}
				}
			}
			break;
		case StatementResult::Stop:
			LatentState = LatentRunState::Stop;
			return result;
		case StatementResult::Return:
			// Package 61 and earlier transfered the return value in an out parameter
			if (!static_cast<ReturnExpression*>(statement)->Value)
			{
				for (UProperty* prop : Func->Properties)
				{
					if (AllFlags(prop->PropFlags, PropertyFlags::Parm | PropertyFlags::ReturnParm))
					{
						result.Value = ExpressionValue::Variable(Variables, prop);
						result.Value.Load();
						break;
					}
				}
			}

			if (RunState == FrameRunState::StepOut && StepFrame == this)
			{
				// We are exiting the function. Break on next instruction by requesting a break on next instruction.
				StepFrame = nullptr;
			}
			return result;
		case StatementResult::Iterator:
			if (!result.Iter)
				ThrowException("Iterator statement without an iterator in " + Object->Name.ToString() + "." + Func->Name.ToString());
			Iterators.push_back(std::move(result.Iter));
			Iterators.back()->StartStatementIndex = curStatementIndex + 1;
			Iterators.back()->EndStatementIndex = Func->Code->FindStatementIndex(result.JumpAddress);
			if (Iterators.back()->Next())
				StatementIndex = Iterators.back()->StartStatementIndex;
			else
				StatementIndex = Iterators.back()->EndStatementIndex;
			break;
		case StatementResult::IteratorNext:
			if (Iterators.empty())
				ThrowException("Iterator next statement without an iterator in " + Object->Name.ToString() + "." + Func->Name.ToString());
			if (Iterators.back()->Next())
				StatementIndex = Iterators.back()->StartStatementIndex;
			else
				StatementIndex = Iterators.back()->EndStatementIndex;
			break;
		case StatementResult::IteratorPop:
			if (Iterators.empty())
				ThrowException("Iterator pop statement without an iterator in " + Object->Name.ToString() + "." + Func->Name.ToString());
			Iterators.pop_back();
			break;
		case StatementResult::AccessedNone:
			LogMessage("Accessed None");
			if (BreakOnError)
			{
				Break();
			}
			else
			{
				LogMessage(Frame::GetCallstack());
			}
			break;
		}

		if (Object->StateFrame.get() == this && LatentState != LatentRunState::Continue)
		{
			return result;
		}

		instructionsRetired++;
	}
}

void Frame::ProcessSwitch(const ExpressionValue& condition)
{
	SwitchExpression* switchexpr = static_cast<SwitchExpression*>(Func->Code->Statements[StatementIndex - 1]);
	while (true)
	{
		CaseExpression* caseexpr = static_cast<CaseExpression*>(Func->Code->Statements[StatementIndex++]);
		if (caseexpr->Value)
		{
			//ExpressionValue casevalue = ExpressionEvaluator::Eval(caseexpr->Value, Object, Object, Variables).Value;
			ExpressionValue casevalue = RunExpr(caseexpr->Value, Object, Object, Variables).Value;
			if (condition.IsEqual(casevalue))
				break;
			else
				StatementIndex = Func->Code->FindStatementIndex(caseexpr->NextOffset);
		}
		else
		{
			break;
		}
	}
}

#define MAX_VMSTACK_DEPTH 1000

struct alignas(ExpressionValue) ExpressionValueMemory
{
	char buffer[sizeof(ExpressionValue)];
};

static struct VMStack
{
	int exprEnd = 0;
	int valueEnd = 0;
	int contextEnd = 0;
	Expression* expr[MAX_VMSTACK_DEPTH];
	int pass[MAX_VMSTACK_DEPTH];
	ExpressionValueMemory value[MAX_VMSTACK_DEPTH];
	UObject* context[MAX_VMSTACK_DEPTH];
} vmstack;

#define PushExpr(e) vmstack.pass[vmstack.exprEnd] = 0; vmstack.expr[vmstack.exprEnd++] = (e);
#define PopExpr() --vmstack.exprEnd
#define PushContext(c) vmstack.context[vmstack.contextEnd++] = context; context = (c)
#define PopContext() context = vmstack.context[--vmstack.contextEnd]
#define PushValue(v) new(vmstack.value[vmstack.valueEnd++].buffer) ExpressionValue(v)

static ExpressionValue PopValue()
{
	--vmstack.valueEnd;
	ExpressionValue result = std::move(*reinterpret_cast<ExpressionValue*>(vmstack.value[vmstack.valueEnd].buffer));
	reinterpret_cast<ExpressionValue*>(vmstack.value[vmstack.valueEnd].buffer)->~ExpressionValue();
	return result;
}

static void PopValues(size_t count)
{
	while (count > 0)
	{
		reinterpret_cast<ExpressionValue*>(vmstack.value[--vmstack.valueEnd].buffer)->~ExpressionValue();
		--count;
	}
}

#ifdef __N64__
// N64 port: native call with the args already on the vmstack. The handler gets a pointer to them,
// the missing optional args and the return slot are pushed right after; no Frame, no copies.
// Returns false when this function needs the generic Frame::CallNative path.
static bool K64NativeFast(UFunction* func, UObject* instance, size_t n)
{
	const K64FuncInfo& fi = K64GetFuncInfo(func);
	if (fi.RetPos == -2 || n > (size_t)fi.NumArgs)
		return false;
	if (!K64EventEnabled(instance, func, fi))
	{
		PopValues(n);
		PushValue(ExpressionValue::NothingValue());
		return true;
	}
	NativeFuncHandler* cb = func->NativeFuncIndex != 0 ? &NativeFunctions::NativeByIndex[func->NativeFuncIndex] : fi.ByName;
	if (!cb || !*cb)
		return false; // generic path logs / throws
	int base = vmstack.valueEnd - (int)n;
	for (size_t i = n; i < (size_t)fi.NumArgs; i++)
		PushValue(ExpressionValue::NothingValue());
	if (fi.RetPos >= 0)
		PushValue(ExpressionValue::PropertyValue(fi.RetProp));
	bool ok = true;
	try
	{
		(*cb)(instance, reinterpret_cast<ExpressionValue*>(vmstack.value[base].buffer));
	}
	catch (const std::exception& e)
	{
		LogMessage(std::string("Script error: ") + e.what());
		ok = false;
	}
	catch (...)
	{
		LogMessage("Script error: Unknown error");
		ok = false;
	}
	if (ok && fi.RetPos >= 0)
	{
		ExpressionValue result = std::move(*reinterpret_cast<ExpressionValue*>(vmstack.value[vmstack.valueEnd - 1].buffer));
		PopValues(vmstack.valueEnd - base);
		PushValue(std::move(result));
	}
	else
	{
		PopValues(vmstack.valueEnd - base);
		PushValue(ExpressionValue::NothingValue());
	}
	return true;
}

// Pure leaf expressions pushed straight onto the value stack (false = not a leaf)
static inline bool K64Leaf(Expression* e, UObject* self, UObject* context, void* variables)
{
	switch (e->Type)
	{
	case ExpressionType::LocalVariable: PushValue(ExpressionValue::Variable(variables, static_cast<LocalVariableExpression*>(e)->Variable)); return true;
	case ExpressionType::InstanceVariable: PushValue(ExpressionValue::Variable(context->PropertyData.Data, static_cast<InstanceVariableExpression*>(e)->Variable)); return true;
	case ExpressionType::Self: PushValue(ExpressionValue::ObjectValue(self)); return true;
	case ExpressionType::IntConst: PushValue(ExpressionValue::IntValue(static_cast<IntConstExpression*>(e)->Value)); return true;
	case ExpressionType::FloatConst: PushValue(ExpressionValue::FloatValue(static_cast<FloatConstExpression*>(e)->Value)); return true;
	case ExpressionType::ObjectConst: PushValue(ExpressionValue::ObjectValue(static_cast<ObjectConstExpression*>(e)->Object)); return true;
	case ExpressionType::NameConst: PushValue(ExpressionValue::NameValue(static_cast<NameConstExpression*>(e)->Value)); return true;
	case ExpressionType::ByteConst: PushValue(ExpressionValue::ByteValue(static_cast<ByteConstExpression*>(e)->Value)); return true;
	case ExpressionType::IntConstByte: PushValue(ExpressionValue::ByteValue(static_cast<IntConstByteExpression*>(e)->Value)); return true;
	case ExpressionType::IntZero: PushValue(ExpressionValue::IntValue(0)); return true;
	case ExpressionType::IntOne: PushValue(ExpressionValue::IntValue(1)); return true;
	case ExpressionType::True: PushValue(ExpressionValue::BoolValue(true)); return true;
	case ExpressionType::False: PushValue(ExpressionValue::BoolValue(false)); return true;
	case ExpressionType::NoObject: PushValue(ExpressionValue::ObjectValue(nullptr)); return true;
	default: return false;
	}
}

// Push the args of a call: the leading leaf args are evaluated now, the rest go on the expr stack
// (evaluated later, in order). Returns true when every arg was a leaf.
template<typename T>
static inline bool K64PushArgs(const T& args, UObject* self, UObject* context, void* variables)
{
	size_t i = 0, n = args.size();
	if (Frame::Breakpoints.empty())
	{
		while (i < n && K64Leaf(args[i], self, context, variables))
			i++;
	}
	for (size_t k = n; k > i; k--)
	{
		PushExpr(args[k - 1]);
	}
	return i == n;
}

// VirtualFunction lookup cache: (call site, context class, state name) -> function
struct K64VEntry
{
	Expression* Expr;
	UClass* Cls;
	NameString State;
	UFunction* Func;
};
static K64VEntry k64VCache[2048];

static inline K64VEntry& K64VSlot(Expression* e, UClass* c)
{
	uintptr_t h = ((uintptr_t)e >> 3) ^ ((uintptr_t)c >> 4) ^ ((uintptr_t)c >> 13);
	return k64VCache[h & 2047];
}

// Call + replace the n args on the vmstack with the result
static void K64CallPush(UFunction* func, UObject* context, size_t n)
{
	if (func && context && Frame::Traces.empty() && AllFlags(func->FuncFlags, FunctionFlags::Native) && K64NativeFast(func, context, n))
		return;
	ArrayView<ExpressionValue> funcArgs((ExpressionValue*)vmstack.value[vmstack.valueEnd - n].buffer, n);
	auto callresult = Frame::Call(func, context, funcArgs);
	PopValues(n);
	PushValue(std::move(callresult));
}
#endif

ExpressionEvalResult Frame::RunExpr(Expression* statementExpr, UObject* self, UObject* context, void* variables)
{
	int enterExprEnd = vmstack.exprEnd;
	int enterValueEnd = vmstack.valueEnd;
	int enterContextEnd = vmstack.contextEnd;

	try
	{
		ExpressionEvalResult result;

		PushExpr(statementExpr);

		while (vmstack.exprEnd > enterExprEnd)
		{
			Expression* expr = vmstack.expr[vmstack.exprEnd - 1];
			int pass = vmstack.pass[vmstack.exprEnd - 1]++;

			Frame::StepExpression = expr;
			for (const Breakpoint& bp : Breakpoints)
			{
				if (bp.Expr == expr && bp.Enabled)
				{
					Frame::Break();
				}
			}

			switch (expr->Type)
			{
			default:
				break;
			case ExpressionType::LocalVariable:
				PushValue(ExpressionValue::Variable(variables, static_cast<LocalVariableExpression*>(expr)->Variable));
				PopExpr();
				break;

			case ExpressionType::InstanceVariable:
				PushValue(ExpressionValue::Variable(context->PropertyData.Data, static_cast<InstanceVariableExpression*>(expr)->Variable));
				PopExpr();
				break;

			case ExpressionType::DefaultVariable:
				if (UObject::TryCast<UClass>(context))
					PushValue(ExpressionValue::Variable(context->PropertyData.Data, static_cast<DefaultVariableExpression*>(expr)->Variable));
				else
					PushValue(ExpressionValue::Variable(context->Class->GetDefaultObject<UObject>()->PropertyData.Data, static_cast<DefaultVariableExpression*>(expr)->Variable));
				PopExpr();
				break;

			case ExpressionType::Return:
				if (pass == 0)
				{
					if (static_cast<ReturnExpression*>(expr)->Value)
					{
						PushExpr(static_cast<ReturnExpression*>(expr)->Value);
					}
					else
					{
						PushValue(ExpressionValue::NothingValue());
					}
				}
				else if (pass == 1)
				{
					result.Result = StatementResult::Return;
					PopExpr();
				}
				break;

			case ExpressionType::Switch:
				if (pass == 0)
				{
					PushExpr(static_cast<SwitchExpression*>(expr)->Condition);
				}
				else
				{
					result.Result = StatementResult::Switch;
					PopExpr();
				}
				break;

			case ExpressionType::Jump:
				result.Result = StatementResult::Jump;
				result.JumpAddress = static_cast<JumpExpression*>(expr)->Offset;
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::JumpIfNot:
				if (pass == 0)
				{
					PushExpr(static_cast<JumpIfNotExpression*>(expr)->Condition);
				}
				else
				{
					if (!PopValue().ToBool())
					{
						result.Result = StatementResult::Jump;
						result.JumpAddress = static_cast<JumpIfNotExpression*>(expr)->Offset;
					}
					PopExpr();
				}
				break;

			case ExpressionType::Stop:
				result.Result = StatementResult::Stop;
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::Assert:
				if (pass == 0)
				{
					PushExpr(static_cast<AssertExpression*>(expr)->Condition);
				}
				else
				{
					if (!PopValue().ToBool())
					{
						Frame::ThrowException("Script assert failed for " + self->Name.ToString() + " line " + std::to_string(static_cast<AssertExpression*>(expr)->Line));
					}
					PushValue(ExpressionValue::NothingValue());
					PopExpr();
				}
				break;

			case ExpressionType::Case:
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::Nothing:
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::LabelTable:
				// Klingon honor guard has this! (UE 251)
				Frame::ThrowException("Label table expression is not implemented");
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::GotoLabel:
				if (pass == 0)
				{
					PushExpr(static_cast<GotoLabelExpression*>(expr)->Value);
				}
				else
				{
					result.Result = StatementResult::GotoLabel;
					result.Label = PopValue().ToName();
					PopExpr();
				}
				break;

			case ExpressionType::EatString:
				if (pass == 0)
				{
					PushExpr(static_cast<EatStringExpression*>(expr)->Value);
				}
				else
				{
					PopValue();
					PushValue(ExpressionValue::NothingValue());
					PopExpr();
				}
				break;

			case ExpressionType::Let:
				if (pass == 0)
				{
#ifdef __N64__
					if (!Breakpoints.empty() || !K64Leaf(static_cast<LetExpression*>(expr)->RightSide, self, context, variables))
					{
						PushExpr(static_cast<LetExpression*>(expr)->LeftSide);
						PushExpr(static_cast<LetExpression*>(expr)->RightSide);
						break;
					}
					if (!K64Leaf(static_cast<LetExpression*>(expr)->LeftSide, self, context, variables))
					{
						PushExpr(static_cast<LetExpression*>(expr)->LeftSide);
						break;
					}
				}
				{
#else
					PushExpr(static_cast<LetExpression*>(expr)->LeftSide);
					PushExpr(static_cast<LetExpression*>(expr)->RightSide);
				}
				else
				{
#endif
					ExpressionValue lvalue = std::move(PopValue());
					ExpressionValue rvalue = std::move(PopValue());
					if (lvalue.GetType() != ExpressionValueType::Nothing)
					{
						lvalue.Store(rvalue);
						PushValue(std::move(lvalue));
					}
					else
					{
						PushValue(std::move(rvalue));
					}
					PopExpr();
				}
				break;

			case ExpressionType::LetBool:
				if (pass == 0)
				{
					PushExpr(static_cast<LetBoolExpression*>(expr)->LeftSide);
					PushExpr(static_cast<LetBoolExpression*>(expr)->RightSide);
				}
				else
				{
					ExpressionValue lvalue = std::move(PopValue());
					ExpressionValue rvalue = std::move(PopValue());
					if (lvalue.GetType() != ExpressionValueType::Nothing)
					{
						lvalue.Store(rvalue);
						PushValue(std::move(lvalue));
					}
					else
					{
						PushValue(std::move(rvalue));
					}
					PopExpr();
				}
				break;

			case ExpressionType::DynArrayElement:
				if (pass == 0)
				{
					PushExpr(static_cast<DynArrayElementExpression*>(expr)->Index);
					PushExpr(static_cast<DynArrayElementExpression*>(expr)->Array);
				}
				else
				{
					int index = PopValue().ToInt();
					ExpressionValue arrayval = std::move(PopValue());
					if (arrayval.IsVariable())
					{
						if (index < 0)
						{
							LogMessage("Negative index used");
							PushValue(ExpressionValue::NothingValue());
						}
						else
						{
							PushValue(arrayval.DynArrayItemAt(index));
						}
					}
					else
					{
						Frame::ThrowException("Array is not a variable in DynArrayElementExpression");
						PushValue(ExpressionValue::NothingValue());
					}
					PopExpr();
				}
				break;

			case ExpressionType::New:
				if (pass == 0)
				{
					PushExpr(static_cast<NewExpression*>(expr)->ParentExpr);
					PushExpr(static_cast<NewExpression*>(expr)->NameExpr);
					PushExpr(static_cast<NewExpression*>(expr)->FlagsExpr);
					PushExpr(static_cast<NewExpression*>(expr)->ClassExpr);
				}
				else
				{
					auto newExpr = static_cast<NewExpression*>(expr);
					ExpressionValue outer = std::move(PopValue());
					ExpressionValue name = std::move(PopValue());
					ExpressionValue flags = std::move(PopValue());
					UClass* cls = UObject::Cast<UClass>(PopValue().ToObject());

					// To do: package needs to be grabbed from outer, or the "transient package" if it is None, a virtual package for runtime objects
					Package* package = engine->packages->GetPackage("Engine");

					UObject* newObj = package->NewObject(
						name.GetType() == ExpressionValueType::Nothing ? NameString() : name.ToName(),
						cls,
						flags.GetType() == ExpressionValueType::Nothing ? ObjectFlags::NoFlags : (ObjectFlags)flags.ToInt(),
						true);

					if (outer.GetType() != ExpressionValueType::Nothing)
						newObj->Outer() = outer.ToObject();

					PushValue(ExpressionValue::ObjectValue(newObj));
					PopExpr();
				}
				break;

			case ExpressionType::ClassContext:
				if (pass == 0)
				{
					PushExpr(static_cast<ClassContextExpression*>(expr)->ObjectExpr);
				}
				else if (pass == 1)
				{
					UClass* cls = UObject::TryCast<UClass>(PopValue().ToObject());
					if (cls)
					{
						PushContext(cls->GetDefaultObject<UObject>());
						PushExpr(static_cast<ClassContextExpression*>(expr)->ContextExpr);
					}
					else
					{
						Frame::ThrowException("Class reference is None");
						PushValue(ExpressionValue::NothingValue());
						PopExpr();
					}
				}
				else
				{
					PopContext();
					PopExpr();
				}
				break;

			case ExpressionType::MetaCast:
				if (pass == 0)
				{
					PushExpr(static_cast<MetaCastExpression*>(expr)->Value);
				}
				else
				{
					auto castExpr = static_cast<MetaCastExpression*>(expr);
					UObject* value = PopValue().ToObject();
					if (value && value != castExpr->Class)
					{
						UClass* cls = UObject::TryCast<UClass>(value);
						while (cls)
						{
							if (cls == castExpr->Class)
								break;
							cls = static_cast<UClass*>(cls->BaseStruct);
						}
						if (!cls)
							value = nullptr;
					}
					PushValue(ExpressionValue::ObjectValue(value));
					PopExpr();
				}
				break;

			case ExpressionType::Unknown0x15:
				// Klingon honor guard has this! (UE 251)
				//Frame::ThrowException("Unknown0x15 expression encountered");
				result.Result = StatementResult::Stop;
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::Self:
				PushValue(ExpressionValue::ObjectValue(self));
				PopExpr();
				break;

			case ExpressionType::Skip:
				if (pass == 0)
				{
					PushExpr(static_cast<SkipExpression*>(expr)->Value);
				}
				else
				{
					PopExpr();
				}
				break;

			case ExpressionType::Context:
				if (pass == 0)
				{
					PushExpr(static_cast<ContextExpression*>(expr)->ObjectExpr);
				}
				else if (pass == 1)
				{
					UObject* newContext = PopValue().ToObject();
					if (newContext)
					{
						PushContext(newContext);
						PushExpr(static_cast<ContextExpression*>(expr)->ContextExpr);
					}
					else
					{
						result.Result = StatementResult::AccessedNone;
						PushValue(ExpressionValue::NothingValue());
						PopExpr();
					}
				}
				else
				{
					PopContext();
					PopExpr();
				}
				break;

			case ExpressionType::ArrayElement:
				if (pass == 0)
				{
					PushExpr(static_cast<ArrayElementExpression*>(expr)->Index);
					PushExpr(static_cast<ArrayElementExpression*>(expr)->Array);
				}
				else
				{
					int index = PopValue().ToInt();
					ExpressionValue arrayval = std::move(PopValue());
					if (arrayval.IsVariable())
					{
						PushValue(arrayval.ItemAt(index));
					}
					else
					{
						Frame::ThrowException("Array is not a variable in ArrayElementExpression");
						PushValue(ExpressionValue::NothingValue());
					}
					PopExpr();
				}
				break;

			case ExpressionType::IntConst:
				PushValue(ExpressionValue::IntValue(static_cast<IntConstExpression*>(expr)->Value));
				PopExpr();
				break;

			case ExpressionType::FloatConst:
				PushValue(ExpressionValue::FloatValue(static_cast<FloatConstExpression*>(expr)->Value));
				PopExpr();
				break;

			case ExpressionType::StringConst:
				PushValue(ExpressionValue::StringValue(static_cast<StringConstExpression*>(expr)->Value));
				PopExpr();
				break;

			case ExpressionType::ObjectConst:
				PushValue(ExpressionValue::ObjectValue(static_cast<ObjectConstExpression*>(expr)->Object));
				PopExpr();
				break;

			case ExpressionType::NameConst:
				PushValue(ExpressionValue::NameValue(static_cast<NameConstExpression*>(expr)->Value));
				PopExpr();
				break;

			case ExpressionType::RotationConst:
				PushValue(ExpressionValue::RotatorValue({
					static_cast<RotationConstExpression*>(expr)->Pitch,
					static_cast<RotationConstExpression*>(expr)->Yaw,
					static_cast<RotationConstExpression*>(expr)->Roll
					}));
				PopExpr();
				break;

			case ExpressionType::VectorConst:
				PushValue(ExpressionValue::VectorValue({
					static_cast<VectorConstExpression*>(expr)->X,
					static_cast<VectorConstExpression*>(expr)->Y,
					static_cast<VectorConstExpression*>(expr)->Z
					}));
				PopExpr();
				break;

			case ExpressionType::ByteConst:
				PushValue(ExpressionValue::ByteValue(static_cast<ByteConstExpression*>(expr)->Value));
				PopExpr();
				break;

			case ExpressionType::IntZero:
				PushValue(ExpressionValue::IntValue(0));
				PopExpr();
				break;

			case ExpressionType::IntOne:
				PushValue(ExpressionValue::IntValue(1));
				PopExpr();
				break;

			case ExpressionType::True:
				PushValue(ExpressionValue::BoolValue(true));
				PopExpr();
				break;

			case ExpressionType::False:
				PushValue(ExpressionValue::BoolValue(false));
				PopExpr();
				break;

			case ExpressionType::NativeParm:
				Frame::ThrowException("Native parm expression is not implemented");
				PopExpr();
				break;

			case ExpressionType::NoObject:
				PushValue(ExpressionValue::ObjectValue(nullptr));
				PopExpr();
				break;

			case ExpressionType::Unknown0x2b:
				if (pass == 0)
				{
					PushExpr(static_cast<Unknown0x2bExpression*>(expr)->Value);
				}
				else
				{
					// This may have been a truncating instruction from back when strings had a fixed size (package version 61 and earlier)
					PopExpr();
				}
				break;

			case ExpressionType::IntConstByte:
				PushValue(ExpressionValue::ByteValue(static_cast<IntConstByteExpression*>(expr)->Value));
				PopExpr();
				break;

			case ExpressionType::BoolVariable:
				if (pass == 0)
				{
					PushExpr(static_cast<BoolVariableExpression*>(expr)->Variable);
				}
				else
				{
					PopExpr();
				}
				break;

			case ExpressionType::DynamicCast:
				if (pass == 0)
				{
					PushExpr(static_cast<DynamicCastExpression*>(expr)->Value);
				}
				else
				{
					UObject* value = PopValue().ToObject();
					if (value && !value->IsA(static_cast<DynamicCastExpression*>(expr)->Class->Name))
						value = nullptr;
					PushValue(ExpressionValue::ObjectValue(value));
					PopExpr();
				}
				break;

			case ExpressionType::Iterator:
				if (pass == 0)
				{
					PushExpr(static_cast<IteratorExpression*>(expr)->Value);
				}
				else
				{
					result.Result = StatementResult::Iterator;
					result.Iter = std::move(Frame::CreatedIterator);
					result.JumpAddress = static_cast<IteratorExpression*>(expr)->Offset;
					PopExpr();
				}
				break;

			case ExpressionType::IteratorPop:
				result.Result = StatementResult::IteratorPop;
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::IteratorNext:
				result.Result = StatementResult::IteratorNext;
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;

			case ExpressionType::StructCmpEq:
				if (pass == 0)
				{
					PushExpr(static_cast<StructCmpEqExpression*>(expr)->Value1);
					PushExpr(static_cast<StructCmpEqExpression*>(expr)->Value2);
				}
				else
				{
					ExpressionValue val1 = std::move(PopValue());
					ExpressionValue val2 = std::move(PopValue());
					PushValue(ExpressionValue::BoolValue(val1.IsEqual(val2)));
					PopExpr();
				}
				break;

			case ExpressionType::StructCmpNe:
				if (pass == 0)
				{
					PushExpr(static_cast<StructCmpNeExpression*>(expr)->Value1);
					PushExpr(static_cast<StructCmpNeExpression*>(expr)->Value2);
				}
				else
				{
					ExpressionValue val1 = std::move(PopValue());
					ExpressionValue val2 = std::move(PopValue());
					PushValue(ExpressionValue::BoolValue(!val1.IsEqual(val2)));
					PopExpr();
				}
				break;

			case ExpressionType::StructMember:
				if (pass == 0)
				{
					if (!static_cast<StructMemberExpression*>(expr)->Field)
						Frame::ThrowException("Null field encountered in struct member expression");
					PushExpr(static_cast<StructMemberExpression*>(expr)->Value);
				}
				else
				{
					ExpressionValue val = std::move(PopValue());
					PushValue(val.Member(static_cast<StructMemberExpression*>(expr)->Field));
					PopExpr();
				}
				break;

			case ExpressionType::UnicodeStringConst:
			{
				std::string s;
				s.reserve(static_cast<UnicodeStringConstExpression*>(expr)->Value.size());
				for (wchar_t c : static_cast<UnicodeStringConstExpression*>(expr)->Value)
					s.push_back(c < 128 ? c : '?');
				PushValue(ExpressionValue::StringValue(s));
				PopExpr();
				break;
			}

			case ExpressionType::RotatorToVector:
				if (pass == 0)
				{
					PushExpr(static_cast<RotatorToVectorExpression*>(expr)->Value);
				}
				else
				{
					Rotator rot = PopValue().ToRotator();
					PushValue(ExpressionValue::VectorValue(Coords::Rotation(rot).XAxis));
					PopExpr();
				}
				break;

			case ExpressionType::ByteToInt:
				if (pass == 0)
				{
					PushExpr(static_cast<ByteToIntExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToByte();
					PushValue(ExpressionValue::IntValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::ByteToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<ByteToBoolExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToByte() != 0;
					PushValue(ExpressionValue::BoolValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::ByteToFloat:
				if (pass == 0)
				{
					PushExpr(static_cast<ByteToFloatExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToByte();
					PushValue(ExpressionValue::FloatValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::IntToByte:
				if (pass == 0)
				{
					PushExpr(static_cast<IntToByteExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToInt();
					PushValue(ExpressionValue::ByteValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::IntToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<IntToBoolExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToInt();
					PushValue(ExpressionValue::BoolValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::IntToFloat:
				if (pass == 0)
				{
					PushExpr(static_cast<IntToFloatExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToInt();
					PushValue(ExpressionValue::FloatValue((float)val));
					PopExpr();
				}
				break;

			case ExpressionType::BoolToByte:
				if (pass == 0)
				{
					PushExpr(static_cast<BoolToByteExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToBool();
					PushValue(ExpressionValue::ByteValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::BoolToInt:
				if (pass == 0)
				{
					PushExpr(static_cast<BoolToIntExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToBool();
					PushValue(ExpressionValue::IntValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::BoolToFloat:
				if (pass == 0)
				{
					PushExpr(static_cast<BoolToFloatExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToBool();
					PushValue(ExpressionValue::FloatValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::FloatToByte:
				if (pass == 0)
				{
					PushExpr(static_cast<FloatToByteExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToFloat();
					PushValue(ExpressionValue::ByteValue((uint8_t)val));
					PopExpr();
				}
				break;

			case ExpressionType::FloatToInt:
				if (pass == 0)
				{
					PushExpr(static_cast<FloatToIntExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToFloat();
					PushValue(ExpressionValue::IntValue((int)val));
					PopExpr();
				}
				break;

			case ExpressionType::FloatToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<FloatToBoolExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToFloat();
					PushValue(ExpressionValue::BoolValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::Unknown0x46:
				Frame::ThrowException("Unknown0x46 expression encountered");
				break;

			case ExpressionType::ObjectToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<ObjectToBoolExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToObject() != nullptr;
					PushValue(ExpressionValue::BoolValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::NameToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<NameToBoolExpression*>(expr)->Value);
				}
				else
				{
					auto val = PopValue().ToName() != "None";
					PushValue(ExpressionValue::BoolValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::StringToByte:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToByteExpression*>(expr)->Value);
				}
				else
				{
					auto val = std::atoi(PopValue().ToString().c_str());
					PushValue(ExpressionValue::ByteValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::StringToInt:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToIntExpression*>(expr)->Value);
				}
				else
				{
					auto val = std::atoi(PopValue().ToString().c_str());
					PushValue(ExpressionValue::IntValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::StringToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToBoolExpression*>(expr)->Value);
				}
				else
				{
					auto val = std::atoi(PopValue().ToString().c_str()) != 0;
					PushValue(ExpressionValue::BoolValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::StringToFloat:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToFloatExpression*>(expr)->Value);
				}
				else
				{
					auto val = (float)std::atof(PopValue().ToString().c_str());
					PushValue(ExpressionValue::FloatValue(val));
					PopExpr();
				}
				break;

			case ExpressionType::StringToVector:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToVectorExpression*>(expr)->Value);
				}
				else
				{
					std::string v = PopValue().ToString();
					auto pos1 = v.find_first_of(',');
					auto pos2 = v.find_first_of(',', pos1 + 1);
					if (pos1 != std::string::npos && pos2 != std::string::npos)
					{
						PushValue(ExpressionValue::VectorValue({
							(float)std::atof(v.substr(0, pos1).c_str()),
							(float)std::atof(v.substr(pos1 + 1, pos2 - pos1 - 1).c_str()),
							(float)std::atof(v.substr(pos2 + 1).c_str())
							}));
					}
					else
					{
						PushValue(ExpressionValue::VectorValue({ 0.0f }));
					}
					PopExpr();
				}
				break;

			case ExpressionType::StringToRotator:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToRotatorExpression*>(expr)->Value);
				}
				else
				{
					std::string v = PopValue().ToString();
					auto pos1 = v.find_first_of(',');
					auto pos2 = v.find_first_of(',', pos1 + 1);
					if (pos1 != std::string::npos && pos2 != std::string::npos)
					{
						PushValue(ExpressionValue::RotatorValue({
							std::atoi(v.substr(0, pos1).c_str()),
							std::atoi(v.substr(pos1 + 1, pos2 - pos1 - 1).c_str()),
							std::atoi(v.substr(pos2 + 1).c_str())
							}));
					}
					else
					{
						PushValue(ExpressionValue::RotatorValue({ 0, 0, 0 }));
					}
					PopExpr();
					break;
				}
				break;

			case ExpressionType::VectorToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<VectorToBoolExpression*>(expr)->Value);
				}
				else
				{
					vec3 v = PopValue().ToVector();
					PushValue(ExpressionValue::BoolValue(v != vec3(0.0f)));
					PopExpr();
				}
				break;

			case ExpressionType::VectorToRotator:
				if (pass == 0)
				{
					PushExpr(static_cast<VectorToRotatorExpression*>(expr)->Value);
				}
				else
				{
					vec3 v = PopValue().ToVector();
					PushValue(ExpressionValue::RotatorValue(Rotator::FromVector(v)));
					PopExpr();
				}
				break;

			case ExpressionType::RotatorToBool:
				if (pass == 0)
				{
					PushExpr(static_cast<RotatorToBoolExpression*>(expr)->Value);
				}
				else
				{
					bool v = PopValue().ToRotator() != Rotator(0, 0, 0);
					PushValue(ExpressionValue::BoolValue(v));
					PopExpr();
				}
				break;

			case ExpressionType::ByteToString:
				if (pass == 0)
				{
					PushExpr(static_cast<ByteToStringExpression*>(expr)->Value);
				}
				else
				{
					auto v = PopValue().ToByte();
					PushValue(ExpressionValue::StringValue(std::to_string(v)));
					PopExpr();
				}
				break;

			case ExpressionType::IntToString:
				if (pass == 0)
				{
					PushExpr(static_cast<IntToStringExpression*>(expr)->Value);
				}
				else
				{
					auto v = PopValue().ToInt();
					PushValue(ExpressionValue::StringValue(std::to_string(v)));
					PopExpr();
				}
				break;

			case ExpressionType::BoolToString:
				if (pass == 0)
				{
					PushExpr(static_cast<BoolToStringExpression*>(expr)->Value);
				}
				else
				{
					auto v = PopValue().ToBool();
					PushValue(ExpressionValue::StringValue(std::to_string(v)));
					PopExpr();
				}
				break;

			case ExpressionType::FloatToString:
				if (pass == 0)
				{
					PushExpr(static_cast<FloatToStringExpression*>(expr)->Value);
				}
				else
				{
					auto v = PopValue().ToFloat();
					PushValue(ExpressionValue::StringValue(std::to_string(v)));
					PopExpr();
				}
				break;

			case ExpressionType::ObjectToString:
				if (pass == 0)
				{
					PushExpr(static_cast<ObjectToStringExpression*>(expr)->Value);
				}
				else
				{
					UObject* obj = PopValue().ToObject();
					PushValue(ExpressionValue::StringValue(obj ? obj->package->GetPackageName().ToString() + "." + obj->Name.ToString() : "None"));
					PopExpr();
				}
				break;

			case ExpressionType::NameToString:
				if (pass == 0)
				{
					PushExpr(static_cast<NameToStringExpression*>(expr)->Value);
				}
				else
				{
					NameString v = PopValue().ToName();
					PushValue(ExpressionValue::StringValue(v.ToString()));
					PopExpr();
				}
				break;

			case ExpressionType::VectorToString:
				if (pass == 0)
				{
					PushExpr(static_cast<VectorToStringExpression*>(expr)->Value);
				}
				else
				{
					vec3 v = PopValue().ToVector();
					PushValue(ExpressionValue::StringValue(std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z)));
					PopExpr();
				}
				break;

			case ExpressionType::RotatorToString:
				if (pass == 0)
				{
					PushExpr(static_cast<RotatorToStringExpression*>(expr)->Value);
				}
				else
				{
					Rotator v = PopValue().ToRotator();
					PushValue(ExpressionValue::StringValue(std::to_string(v.Pitch & 0xffff) + "," + std::to_string(v.Yaw & 0xffff) + "," + std::to_string(v.Roll & 0xffff)));
					PopExpr();
				}
				break;

			case ExpressionType::StringToName:
				if (pass == 0)
				{
					PushExpr(static_cast<StringToNameExpression*>(expr)->Value);
				}
				else
				{
					std::string v = PopValue().ToString();
					PushValue(ExpressionValue::NameValue(v));
					PopExpr();
				}
				break;

			case ExpressionType::DynArrayToInt:
				if (pass == 0)
				{
					PushExpr(static_cast<DynArrayToIntExpression*>(expr)->Value);
				}
				else
				{
					size_t count = PopValue().ToArray().GetSize();
					PushValue(ExpressionValue::IntValue((int)count));
					PopExpr();
				}
				break;

			case ExpressionType::VirtualFunction:
				if (pass == 0)
				{
					PushContext(self);
					const auto& args = static_cast<VirtualFunctionExpression*>(expr)->Args;
#ifdef __N64__
					if (!K64PushArgs(args, self, context, variables))
						break;
				}
				{
#else
					for (auto it = args.rbegin(); it != args.rend(); ++it)
					{
						PushExpr(*it);
					}
				}
				else
				{
#endif
					PopContext();

					auto funcExpr = static_cast<VirtualFunctionExpression*>(expr);
					ArrayView<ExpressionValue> funcArgs((ExpressionValue*)vmstack.value[vmstack.valueEnd - funcExpr->Args.size()].buffer, funcExpr->Args.size());

					UClass* contextClass = UObject::TryCast<UClass>(context);
					if (!contextClass)
						contextClass = context->Class;

					// Search states first

					NameString stateName = context->GetStateName();
					bool found = false;
#ifdef __N64__
					{
						K64VEntry& ve = K64VSlot(expr, contextClass);
						if (ve.Expr == expr && ve.Cls == contextClass && ve.State == stateName)
						{
							K64CallPush(ve.Func, context, funcExpr->Args.size());
							PopExpr();
							break;
						}
						UFunction* vf = nullptr;
						for (UClass* cls = contextClass; !vf && cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
						{
							UState* state = cls->GetState(stateName);
							if (state)
								vf = state->GetFunction(funcExpr->Name);
						}
						for (UClass* cls = contextClass; !vf && cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
							vf = cls->GetFunction(funcExpr->Name);
						if (vf)
						{
							ve.Expr = expr;
							ve.Cls = contextClass;
							ve.State = stateName;
							ve.Func = vf;
							K64CallPush(vf, context, funcExpr->Args.size());
							PopExpr();
							break;
						}
					}
#endif
					for (UClass* cls = contextClass; cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
					{
						UState* state = cls->GetState(stateName);
						if (state)
						{
							UFunction* func = state->GetFunction(funcExpr->Name);
							if (func)
							{
#ifdef __N64__
								K64CallPush(func, context, funcExpr->Args.size());
#else
								auto callresult = Frame::Call(func, context, funcArgs);
								PopValues(funcExpr->Args.size());
								PushValue(callresult);
#endif
								PopExpr();
								found = true;
								break;
							}
						}
					}

					if (!found)
					{
						// Search normal member functions next
						for (UClass* cls = contextClass; !found && cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
						{
							if (UFunction* func = cls->GetFunction(funcExpr->Name))
							{
#ifdef __N64__
								K64CallPush(func, context, funcExpr->Args.size());
#else
								auto callresult = Frame::Call(func, context, funcArgs);
								PopValues(funcExpr->Args.size());
								PushValue(callresult);
#endif
								PopExpr();
								found = true;
								break;
							}
						}
					}

					if (!found)
					{
						Frame::ThrowException("Script virtual function " + funcExpr->Name.ToString() + " not found!");
						for (size_t i = 0, count = funcExpr->Args.size(); i < count; i++)
							PopValue();
						PushValue(ExpressionValue::NothingValue());
						PopExpr();
					}
				}
				break;

			case ExpressionType::FinalFunction:
				if (pass == 0)
				{
					PushContext(self);
					const auto& args = static_cast<FinalFunctionExpression*>(expr)->Args;
#ifdef __N64__
					if (!K64PushArgs(args, self, context, variables))
						break;
				}
				{
#else
					for (auto it = args.rbegin(); it != args.rend(); ++it)
					{
						PushExpr(*it);
					}
				}
				else
				{
#endif
					PopContext();
					auto funcExpr = static_cast<FinalFunctionExpression*>(expr);
					ArrayView<ExpressionValue> funcArgs((ExpressionValue*)vmstack.value[vmstack.valueEnd - funcExpr->Args.size()].buffer, funcExpr->Args.size());

#ifdef __N64__
					K64CallPush(funcExpr->Func, context, funcExpr->Args.size());
#else
					auto callresult = Frame::Call(funcExpr->Func, context, funcArgs);
					PopValues(funcExpr->Args.size());
					PushValue(callresult);
#endif
					PopExpr();
				}
				break;

			case ExpressionType::GlobalFunction:
				if (pass == 0)
				{
					PushContext(self);
					const auto& args = static_cast<GlobalFunctionExpression*>(expr)->Args;
#ifdef __N64__
					if (!K64PushArgs(args, self, context, variables))
						break;
				}
				{
#else
					for (auto it = args.rbegin(); it != args.rend(); ++it)
					{
						PushExpr(*it);
					}
				}
				else
				{
#endif
					PopContext();
					auto funcExpr = static_cast<GlobalFunctionExpression*>(expr);
					ArrayView<ExpressionValue> funcArgs((ExpressionValue*)vmstack.value[vmstack.valueEnd - funcExpr->Args.size()].buffer, funcExpr->Args.size());

					// Global function calls skip the states and only searches normal member functions

					UClass* contextClass = UObject::TryCast<UClass>(context);
					if (!contextClass)
						contextClass = context->Class;

					bool found = false;
					for (UClass* cls = contextClass; cls != nullptr; cls = static_cast<UClass*>(cls->BaseStruct))
					{
						UFunction* func = cls->GetFunction(funcExpr->Name);
						if (func)
						{
#ifdef __N64__
							K64CallPush(func, context, funcExpr->Args.size());
#else
							auto callresult = Frame::Call(func, context, funcArgs);
							PopValues(funcExpr->Args.size());
							PushValue(callresult);
#endif
							PopExpr();
							found = true;
							break;
						}
					}

					if (!found)
					{
						Frame::ThrowException("Script global function " + static_cast<GlobalFunctionExpression*>(expr)->Name.ToString() + " not found!");
						PopValues(funcExpr->Args.size());
						PushValue(ExpressionValue::NothingValue());
					}
				}
				break;

			case ExpressionType::NativeFunction:
				// To do: conditional operator should probably be handled by the skip expression
				if (static_cast<NativeFunctionExpression*>(expr)->nativeindex == 130) // conditional operator &&
				{
					auto funcExpr = static_cast<NativeFunctionExpression*>(expr);
					if (pass == 0)
					{
						PushContext(self);
						PushExpr(funcExpr->Args[0]);
					}
					else if (pass == 1)
					{
						if (PopValue().ToBool())
						{
							PushExpr(funcExpr->Args[1]);
						}
						else
						{
							PopContext();
							PushValue(ExpressionValue::BoolValue(false));
							PopExpr();
						}
					}
					else
					{
						PopContext();
						bool value = PopValue().ToBool();
						PushValue(ExpressionValue::BoolValue(value));
						PopExpr();
					}
				}
				else if (static_cast<NativeFunctionExpression*>(expr)->nativeindex == 542 && ((size_t)542 >= NativeFunctions::FuncByIndex.size() || !NativeFunctions::FuncByIndex[542])) // U227 ternary cond ? a : b
				{
					auto funcExpr = static_cast<NativeFunctionExpression*>(expr);
					if (pass == 0)
					{
						PushContext(self);
						PushExpr(funcExpr->Args[0]);
					}
					else if (pass == 1)
					{
						PushExpr(PopValue().ToBool() ? funcExpr->Args[2] : funcExpr->Args[3]);
					}
					else
					{
						PopContext();
						PopExpr();
					}
				}
				else if (static_cast<NativeFunctionExpression*>(expr)->nativeindex == 132) // conditional operator ||
				{
					auto funcExpr = static_cast<NativeFunctionExpression*>(expr);
					if (pass == 0)
					{
						PushContext(self);
						PushExpr(funcExpr->Args[0]);
					}
					else if (pass == 1)
					{
						if (!PopValue().ToBool())
						{
							PushExpr(funcExpr->Args[1]);
						}
						else
						{
							PopContext();
							PushValue(ExpressionValue::BoolValue(true));
							PopExpr();
						}
					}
					else
					{
						PopContext();
						bool value = PopValue().ToBool();
						PushValue(ExpressionValue::BoolValue(value));
						PopExpr();
					}
				}
				else
				{
					if (pass == 0)
					{
						PushContext(self);
						const auto& args = static_cast<NativeFunctionExpression*>(expr)->Args;
#ifdef __N64__
						if (!K64PushArgs(args, self, context, variables))
							break;
					}
					{
#else
						for (auto it = args.rbegin(); it != args.rend(); ++it)
						{
							PushExpr(*it);
						}
					}
					else
					{
#endif
						PopContext();
						auto funcExpr = static_cast<NativeFunctionExpression*>(expr);
						ArrayView<ExpressionValue> funcArgs((ExpressionValue*)vmstack.value[vmstack.valueEnd - funcExpr->Args.size()].buffer, funcExpr->Args.size());

#ifdef __N64__
						if ((size_t)funcExpr->nativeindex >= NativeFunctions::FuncByIndex.size() || !NativeFunctions::FuncByIndex[funcExpr->nativeindex])
							printf("[ue] native index %d not registered (table %d)\n", funcExpr->nativeindex, (int)NativeFunctions::FuncByIndex.size());
#endif
#ifdef __N64__
						if ((size_t)funcExpr->nativeindex >= NativeFunctions::FuncByIndex.size() || !NativeFunctions::FuncByIndex[funcExpr->nativeindex])
						{
							printf("[ue] native index %d not registered (table %d)\n", funcExpr->nativeindex, (int)NativeFunctions::FuncByIndex.size());
						{ printf("[ue] natexpr type %d nat %d nargs %d\n", (int)funcExpr->Type, (int)funcExpr->nativeindex, (int)funcExpr->Args.size()); for (size_t k = 0; k < funcExpr->Args.size() && k < 8; k++) printf("[ue]  arg %d type %d\n", (int)k, (int)funcExpr->Args[k]->Type); }
						if (!Callstack.empty() && Callstack.back()->Func && Callstack.back()->Func->Code) { auto* cf = Callstack.back()->Func; for (size_t k = 0; k < cf->Code->Statements.size() && k < 8; k++) printf("[ue] stmt %d: %s\n", (int)k, GetDisassembly(cf->Code->Statements[k]).c_str()); }
						}
#endif
#ifdef __N64__
						K64CallPush(NativeFunctions::FuncByIndex[funcExpr->nativeindex], context, funcExpr->Args.size());
#else
						auto callresult = Frame::Call(NativeFunctions::FuncByIndex[funcExpr->nativeindex], context, funcArgs);
						PopValues(funcExpr->Args.size());
						PushValue(callresult);
#endif
						PopExpr();
					}
				}
				break;

			case ExpressionType::Construct:
			{
				auto* cexpr = static_cast<ConstructExpression*>(expr);
				if (pass < (int)cexpr->Args.size())
				{
					PushExpr(cexpr->Args[pass].Value);
				}
				else
				{
					size_t n = cexpr->Args.size();
					ExpressionValue result = ExpressionValue::NothingValue();
					std::string sn = cexpr->Struct ? cexpr->Struct->Name.ToString() : std::string();
					if (sn == "Vector") result = ExpressionValue::VectorValue(vec3(0.0f));
					else if (sn == "Rotator") result = ExpressionValue::RotatorValue(Rotator(0, 0, 0));
					for (size_t i = 0; i < n; i++)
					{
						ExpressionValue& av = *reinterpret_cast<ExpressionValue*>(vmstack.value[vmstack.valueEnd - n + i].buffer);
						if (cexpr->Args[i].Name && result.GetType() != ExpressionValueType::Nothing)
							result.Member(cexpr->Args[i].Name).Store(av);
					}
					PopValues(n);
					PushValue(result);
					PopExpr();
				}
				break;
			}

			case ExpressionType::FunctionArguments:
				PushValue(ExpressionValue::NothingValue());
				PopExpr();
				break;
			}
		}

		if (vmstack.valueEnd > enterValueEnd)
			result.Value = std::move(PopValue());

		assert(enterExprEnd == vmstack.exprEnd);
		assert(enterValueEnd == vmstack.valueEnd);
		assert(enterContextEnd == vmstack.contextEnd);

		return result;
	}
	catch (...)
	{
		vmstack.exprEnd = enterExprEnd;
		vmstack.contextEnd = enterContextEnd;
		PopValues(vmstack.valueEnd - enterValueEnd);
		throw;
	}
}
