#include "isolate_handle.h"
#include "context_handle.h"
#include "external_copy.h"
#include "external_copy_handle.h"
#include "script_handle.h"
#include "session_handle.h"
#include "isolate/three_phase_task.h"
#include <cmath>

using namespace v8;
using std::shared_ptr;
using std::unique_ptr;

namespace ivm {

/**
 * Parses script origin information from an option object and returns a non-v8 holder for the
 * information which can then be converted to a ScriptOrigin, perhaps in a different isolate from
 * the one it was read in.
 */
class ScriptOriginHolder {
	private:
		std::string filename;
		int columnOffset;
		int lineOffset;

	public:
		explicit ScriptOriginHolder(MaybeLocal<Object> maybe_options) : filename("<isolated-vm>"), columnOffset(0), lineOffset(0) {
			Local<Object> options;
			if (maybe_options.ToLocal(&options)) {
				Local<Context> context = Isolate::GetCurrent()->GetCurrentContext();
				Local<Value> filename = Unmaybe(options->Get(context, v8_string("filename")));
				if (!filename->IsUndefined()) {
					if (!filename->IsString()) {
						throw js_type_error("`filename` must be a string");
					}
					this->filename = *String::Utf8Value(filename.As<String>());
				}
				Local<Value> columnOffset = Unmaybe(options->Get(context, v8_string("columnOffset")));
				if (!columnOffset->IsUndefined()) {
					if (!columnOffset->IsInt32()) {
						throw js_type_error("`columnOffset` must be an integer");
					}
					this->columnOffset = columnOffset.As<Int32>()->Value();
				}
				Local<Value> lineOffset = Unmaybe(options->Get(context, v8_string("lineOffset")));
				if (!lineOffset->IsUndefined()) {
					if (!lineOffset->IsInt32()) {
						throw js_type_error("`lineOffset` must be an integer");
					}
					this->lineOffset = lineOffset.As<Int32>()->Value();
				}
			}
		}

		ScriptOrigin ToScriptOrigin() {
			Isolate* isolate = Isolate::GetCurrent();
			return { v8_string(filename.c_str()), Integer::New(isolate, columnOffset), Integer::New(isolate, lineOffset) };
		}
};

/**
 * ArrayBuffer::Allocator that enforces memory limits. The v8 documentation specifically says
 * that it's unsafe to call back into v8 from this class but I took a look at
 * GetHeapStatistics() and I think it'll be ok.
 */
class LimitedAllocator : public ArrayBuffer::Allocator {
	private:
		size_t limit;
		size_t v8_heap;
		size_t my_heap;
		size_t next_check;

		bool Check(const size_t length) {
			if (v8_heap + my_heap + length > next_check) {
				HeapStatistics heap_statistics;
				Isolate::GetCurrent()->GetHeapStatistics(&heap_statistics);
				v8_heap = heap_statistics.total_heap_size();
				if (v8_heap + my_heap + length > limit) {
					return false;
				}
				next_check = v8_heap + my_heap + length + 1024 * 1024;
			}
			if (v8_heap + my_heap + length > limit) {
				return false;
			}
			my_heap += length;
			return true;
		}

	public:
		explicit LimitedAllocator(size_t limit) : limit(limit), v8_heap(1024 * 1024 * 4), my_heap(0), next_check(1024 * 1024) {}

		void* Allocate(size_t length) final {
			if (Check(length)) {
				return calloc(length, 1);
			} else {
				return nullptr;
			}
		}

		void* AllocateUninitialized(size_t length) final {
			if (Check(length)) {
				return malloc(length);
			} else {
				return nullptr;
			}
		}

		void Free(void* data, size_t length) final {
			my_heap -= length;
			next_check -= length;
			free(data);
		}

		size_t GetAllocatedSize() const {
			return my_heap;
		}
};

/**
 * IsolateHandle implementation
 */
IsolateHandle::IsolateHandleTransferable::IsolateHandleTransferable(shared_ptr<IsolateHolder> isolate) : isolate(std::move(isolate)) {}

Local<Value> IsolateHandle::IsolateHandleTransferable::TransferIn() {
	return ClassHandle::NewInstance<IsolateHandle>(isolate);
}

IsolateHandle::IsolateHandle(shared_ptr<IsolateHolder> isolate) : isolate(std::move(isolate)) {}

IsolateEnvironment::IsolateSpecific<FunctionTemplate>& IsolateHandle::TemplateSpecific() {
	static IsolateEnvironment::IsolateSpecific<FunctionTemplate> tmpl;
	return tmpl;
}

Local<FunctionTemplate> IsolateHandle::Definition() {
	return Inherit<TransferableHandle>(MakeClass(
	 "Isolate", ParameterizeCtor<decltype(&New), &New>(),
		"createSnapshot", ParameterizeStatic<decltype(&CreateSnapshot), &CreateSnapshot>(),
		"compileScript", Parameterize<decltype(&IsolateHandle::CompileScript<1>), &IsolateHandle::CompileScript<1>>(),
		"compileScriptSync", Parameterize<decltype(&IsolateHandle::CompileScript<0>), &IsolateHandle::CompileScript<0>>(),
		"createContext", Parameterize<decltype(&IsolateHandle::CreateContext<1>), &IsolateHandle::CreateContext<1>>(),
		"createContextSync", Parameterize<decltype(&IsolateHandle::CreateContext<0>), &IsolateHandle::CreateContext<0>>(),
		"createInspectorSession", Parameterize<decltype(&IsolateHandle::CreateInspectorSession), &IsolateHandle::CreateInspectorSession>(),
		"dispose", Parameterize<decltype(&IsolateHandle::Dispose), &IsolateHandle::Dispose>(),
		"getHeapStatistics", Parameterize<decltype(&IsolateHandle::GetHeapStatistics<1>), &IsolateHandle::GetHeapStatistics<1>>(),
		"getHeapStatisticsSync", Parameterize<decltype(&IsolateHandle::GetHeapStatistics<0>), &IsolateHandle::GetHeapStatistics<0>>()
	));
}

/**
 * Create a new Isolate. It all starts here!
 */
unique_ptr<ClassHandle> IsolateHandle::New(MaybeLocal<Object> maybe_options) {
	Local<Context> context = Isolate::GetCurrent()->GetCurrentContext();
	shared_ptr<ExternalCopyArrayBuffer> snapshot_blob;
	ResourceConstraints rc;
	size_t memory_limit = 128;
	bool inspector = false;

	// Parse options
	if (!maybe_options.IsEmpty()) {
		Local<Object> options = maybe_options.ToLocalChecked();

		// Check memory limits
		Local<Value> maybe_memory_limit = Unmaybe(options->Get(context, v8_symbol("memoryLimit")));
		if (!maybe_memory_limit->IsUndefined()) {
			if (!maybe_memory_limit->IsNumber()) {
				throw js_generic_error("`memoryLimit` must be a number");
			}
			memory_limit = (size_t)maybe_memory_limit.As<Number>()->Value();
			if (memory_limit < 8) {
				throw js_generic_error("`memoryLimit` must be at least 8");
			}
		}

		// Set snapshot
		Local<Value> snapshot_handle = Unmaybe(options->Get(context, v8_symbol("snapshot")));
		if (!snapshot_handle->IsUndefined()) {
			if (
				!snapshot_handle->IsObject() ||
				!ClassHandle::GetFunctionTemplate<ExternalCopyHandle>()->HasInstance(snapshot_handle.As<Object>())
			) {
				throw js_type_error("`snapshot` must be an ExternalCopy to ArrayBuffer");
			}
			ExternalCopyHandle& copy_handle = *dynamic_cast<ExternalCopyHandle*>(ClassHandle::Unwrap(snapshot_handle.As<Object>()));
			snapshot_blob = std::dynamic_pointer_cast<ExternalCopyArrayBuffer>(copy_handle.GetValue());
			if (!snapshot_blob) {
				throw js_type_error("`snapshot` must be an ExternalCopy to ArrayBuffer");
			}
		}

		// Check inspector flag
		inspector = Unmaybe(options->Get(context, v8_symbol("inspector")))->IsTrue();
	}

	// Set memory limit
	rc.set_max_semi_space_size((int)std::pow(2, std::min(sizeof(void*) >= 8 ? 4 : 3, (int)(memory_limit / 128))));
	rc.set_max_old_space_size((int)(memory_limit * 2));
	auto allocator_ptr = std::make_unique<LimitedAllocator>(memory_limit * 1024 * 1024);

	// Return isolate handle
	auto isolate = IsolateEnvironment::New(rc, std::move(allocator_ptr), std::move(snapshot_blob), memory_limit);
	if (inspector) {
		isolate->GetIsolate()->EnableInspectorAgent();
	}
	return std::make_unique<IsolateHandle>(isolate);
}

unique_ptr<Transferable> IsolateHandle::TransferOut() {
	return std::make_unique<IsolateHandleTransferable>(isolate);
}

/**
 * Create a new v8::Context in this isolate and returns a ContextHandle
 */
struct CreateContextRunner : public ThreePhaseTask {
	bool enable_inspector = false;
	shared_ptr<IsolateHolder> isolate;
	shared_ptr<Persistent<Context>> context;
	shared_ptr<Persistent<Value>> global;

	CreateContextRunner(MaybeLocal<Object>& maybe_options, shared_ptr<IsolateHolder> isolate) : isolate(std::move(isolate)) {
		Local<Object> options;
		if (maybe_options.ToLocal(&options)) {
			Local<Value> enable_inspector = Unmaybe(options->Get(Isolate::GetCurrent()->GetCurrentContext(), v8_symbol("inspector")));
			if (enable_inspector->IsTrue()) {
				this->enable_inspector = true;
			}
		}
	}

	void Phase2() final {
		// Use custom deleter on the shared_ptr which will notify the isolate when the context is gone
		struct ContextDeleter {
			std::weak_ptr<IsolateHolder> isolate;
			bool has_inspector;

			explicit ContextDeleter(bool has_inspector) : isolate(IsolateEnvironment::GetCurrentHolder()), has_inspector(has_inspector) {}

			void operator() (Persistent<Context>* ptr) {
				// This part is called by any isolate. We need to schedule a task to run in the isolate so
				// we can dispatch the correct notifications.
				struct ContextDisposer : public Runnable {
					unique_ptr<Persistent<Context>> context;
					bool has_inspector;
					ContextDisposer(unique_ptr<Persistent<Context>> context, bool has_inspector) : context(std::move(context)), has_inspector(has_inspector) {}
					void Run() final {
						Isolate* isolate = Isolate::GetCurrent();
						if (has_inspector) {
							HandleScope handle_scope(isolate);
							Local<Context> context = Local<Context>::New(isolate, *this->context);
							this->context->Reset();
							IsolateEnvironment::GetCurrent()->GetInspectorAgent()->ContextDestroyed(context);
						}
						this->context->Reset();
						isolate->ContextDisposedNotification();
					}
				};
				auto context = unique_ptr<Persistent<Context>>(ptr);
				shared_ptr<IsolateHolder> isolate_ref = isolate.lock();
				if (isolate_ref) {
					isolate_ref->ScheduleTask(std::make_unique<ContextDisposer>(std::move(context), has_inspector), true, false);
				}
			}
		};

		Isolate* isolate = Isolate::GetCurrent();
		auto env = IsolateEnvironment::GetCurrent();

		// Sanity check before we build the context
		if (enable_inspector && !env->GetInspectorAgent()) {
			Context::Scope context_scope(env->DefaultContext()); // TODO: This is needed to throw, but is stupid and sloppy
			throw js_generic_error("Inspector is not enabled for this isolate");
		}

		// Make a new context and setup shared pointers
		Local<Context> context_handle = Context::New(isolate);
		if (enable_inspector) {
			env->GetInspectorAgent()->ContextCreated(context_handle, "<isolated-vm>");
		}
		context = shared_ptr<Persistent<Context>>(new Persistent<Context>(isolate, context_handle), ContextDeleter(enable_inspector));
		global = std::make_shared<Persistent<Value>>(isolate, context_handle->Global());
	}

	Local<Value> Phase3() final {
		// Make a new Context{} JS class
		return ClassHandle::NewInstance<ContextHandle>(std::move(isolate), std::move(context), std::move(global));
	}
};
template <int async>
Local<Value> IsolateHandle::CreateContext(MaybeLocal<Object> maybe_options) {
	return ThreePhaseTask::Run<async, CreateContextRunner>(*isolate, maybe_options, isolate);
}

/**
 * Compiles a script in this isolate and returns a ScriptHandle
 */
struct CompileScriptRunner : public ThreePhaseTask {
	// phase 2
	shared_ptr<IsolateHolder> isolate;
	unique_ptr<ExternalCopyString> code_string;
	unique_ptr<ScriptOriginHolder> script_origin_holder;
	shared_ptr<ExternalCopyArrayBuffer> cached_data_blob; // also phase 3
	bool produce_cached_data { false };
	// phase 3
	shared_ptr<Persistent<UnboundScript>> script;
	bool supplied_cached_data { false };
	bool cached_data_rejected { false };

	CompileScriptRunner(shared_ptr<IsolateHolder> isolate, const Local<String>& code_handle, const MaybeLocal<Object>& maybe_options) : isolate(std::move(isolate)) {
		// Read options
		Local<Context> context = Isolate::GetCurrent()->GetCurrentContext();
		Local<Object> options;

		script_origin_holder = std::make_unique<ScriptOriginHolder>(maybe_options);
		if (maybe_options.ToLocal(&options)) {

			// Get cached data blob
			Local<Value> cached_data_handle = Unmaybe(options->Get(context, v8_symbol("cachedData")));
			if (!cached_data_handle->IsUndefined()) {
				if (
					!cached_data_handle->IsObject() ||
					!ClassHandle::GetFunctionTemplate<ExternalCopyHandle>()->HasInstance(cached_data_handle.As<Object>())
				) {
					throw js_type_error("`cachedData` must be an ExternalCopy to ArrayBuffer");
				}
				ExternalCopyHandle& copy_handle = *dynamic_cast<ExternalCopyHandle*>(ClassHandle::Unwrap(cached_data_handle.As<Object>()));
				cached_data_blob = std::dynamic_pointer_cast<ExternalCopyArrayBuffer>(copy_handle.GetValue());
				if (!cached_data_blob) {
					throw js_type_error("`cachedData` must be an ExternalCopy to ArrayBuffer");
				}
			}

			// Get cached data flag
			Local<Value> produce_cached_data_val;
			if (options->Get(context, v8_symbol("produceCachedData")).ToLocal(&produce_cached_data_val)) {
				produce_cached_data = produce_cached_data_val->IsTrue();
			}
		}

		// Copy code string
		code_string = std::make_unique<ExternalCopyString>(code_handle);
	}

	void Phase2() final {
		// Compile in second isolate and return UnboundScript persistent
		auto isolate = IsolateEnvironment::GetCurrent();
		Context::Scope context_scope(isolate->DefaultContext());
		Local<String> code_inner = code_string->CopyIntoCheckHeap().As<String>();
		ScriptOrigin script_origin = script_origin_holder->ToScriptOrigin();
		ScriptCompiler::CompileOptions compile_options = ScriptCompiler::kNoCompileOptions;
		unique_ptr<ScriptCompiler::CachedData> cached_data = nullptr;
		if (cached_data_blob) {
			compile_options = ScriptCompiler::kConsumeCodeCache;
			cached_data = std::make_unique<ScriptCompiler::CachedData>((const uint8_t*)cached_data_blob->Data(), cached_data_blob->Length());
		} else if (produce_cached_data) {
			compile_options = ScriptCompiler::kProduceCodeCache;
		}
		ScriptCompiler::Source source(code_inner, script_origin, cached_data.release());
		script = std::make_shared<Persistent<UnboundScript>>(Isolate::GetCurrent(), RunWithAnnotatedErrors<Local<UnboundScript>>(
			[&isolate, &source, compile_options]() { return Unmaybe(ScriptCompiler::CompileUnboundScript(*isolate, &source, compile_options)); }
		));

		// Check cached data flags
		if (cached_data_blob) {
			supplied_cached_data = true;
			cached_data_rejected = source.GetCachedData()->rejected;
			cached_data_blob.reset();
		} else if (produce_cached_data) {
			const ScriptCompiler::CachedData* cached_data = source.GetCachedData();
			assert(cached_data != nullptr);
			cached_data_blob = std::make_shared<ExternalCopyArrayBuffer>((void*)cached_data->data, cached_data->length);
		}
	}

	Local<Value> Phase3() final {
		// Wrap UnboundScript in JS Script{} class
		Local<Object> value = ClassHandle::NewInstance<ScriptHandle>(std::move(isolate), std::move(script));
		Isolate* isolate = Isolate::GetCurrent();
		if (supplied_cached_data) {
			value->Set(v8_symbol("cachedDataRejected"), Boolean::New(isolate, cached_data_rejected));
		} else if (cached_data_blob) {
			value->Set(v8_symbol("cachedData"), ClassHandle::NewInstance<ExternalCopyHandle>(cached_data_blob));
		}
		return value;
	}
};
template <int async>
Local<Value> IsolateHandle::CompileScript(Local<String> code_handle, MaybeLocal<Object> maybe_options) {
	return ThreePhaseTask::Run<async, CompileScriptRunner>(*this->isolate, this->isolate, code_handle, maybe_options);
}

/**
 * Create a new channel for debugging on the inspector
 */
Local<Value> IsolateHandle::CreateInspectorSession() {
	if (IsolateEnvironment::GetCurrentHolder() == isolate) {
		throw js_generic_error("An isolate is not debuggable from within itself");
	}
	shared_ptr<IsolateEnvironment> env = isolate->GetIsolate();
	if (!env) {
		throw js_generic_error("Isolate is diposed");
	}
	if (env->GetInspectorAgent() == nullptr) {
		throw js_generic_error("Inspector is not enabled for this isolate");
	}
	return ClassHandle::NewInstance<SessionHandle>(*env);
}

/**
 * Dispose an isolate
 */
Local<Value> IsolateHandle::Dispose() {
	isolate->Dispose();
	return Undefined(Isolate::GetCurrent());
}

/**
 * Get heap statistics from v8
 */
struct HeapStatRunner : public ThreePhaseTask {
	HeapStatistics heap;
	size_t externally_allocated_size = 0;

	// Dummy constructor to workaround gcc bug
	HeapStatRunner(int /* unused */) {}

	void Phase2() final {
		Isolate::GetCurrent()->GetHeapStatistics(&heap);
		externally_allocated_size = dynamic_cast<LimitedAllocator*>(IsolateEnvironment::GetCurrent()->GetAllocator())->GetAllocatedSize();
	}

	Local<Value> Phase3() final {
		Isolate* isolate = Isolate::GetCurrent();
		Local<Object> ret = Object::New(isolate);
		ret->Set(v8_string("total_heap_size"), Number::New(isolate, heap.total_heap_size()));
		ret->Set(v8_string("total_heap_size_executable"), Number::New(isolate, heap.total_heap_size_executable()));
		ret->Set(v8_string("total_physical_size"), Number::New(isolate, heap.total_physical_size()));
		ret->Set(v8_string("total_available_size"), Number::New(isolate, heap.total_available_size()));
		ret->Set(v8_string("used_heap_size"), Number::New(isolate, heap.used_heap_size()));
		ret->Set(v8_string("heap_size_limit"), Number::New(isolate, heap.heap_size_limit()));
		ret->Set(v8_string("malloced_memory"), Number::New(isolate, heap.malloced_memory()));
		ret->Set(v8_string("peak_malloced_memory"), Number::New(isolate, heap.peak_malloced_memory()));
		ret->Set(v8_string("does_zap_garbage"), Number::New(isolate, heap.does_zap_garbage()));
		ret->Set(v8_string("externally_allocated_size"), Number::New(isolate, externally_allocated_size));
		return ret;
	}
};
template <int async>
Local<Value> IsolateHandle::GetHeapStatistics() {
	return ThreePhaseTask::Run<async, HeapStatRunner>(*isolate, 0);
}

/**
* Create a snapshot from some code and return it as an external ArrayBuffer
*/
Local<Value> IsolateHandle::CreateSnapshot(Local<Array> script_handles, MaybeLocal<String> warmup_handle) {

	// Copy embed scripts and warmup script from outer isolate
	std::vector<std::pair<std::string, ScriptOriginHolder>> scripts;
	Local<Context> context = Isolate::GetCurrent()->GetCurrentContext();
	Local<Array> keys = Unmaybe(script_handles->GetOwnPropertyNames(context));
	scripts.reserve(keys->Length());
	for (uint32_t ii = 0; ii < keys->Length(); ++ii) {
		Local<Uint32> key = Unmaybe(Unmaybe(keys->Get(context, ii))->ToArrayIndex(context));
		if (key->Value() != ii) {
			throw js_type_error("Invalid `scripts` array");
		}
		Local<Value> script_handle = Unmaybe(script_handles->Get(context, key));
		if (!script_handle->IsObject()) {
			throw js_type_error("`scripts` should be array of objects");
		}
		Local<Value> script = Unmaybe(script_handle.As<Object>()->Get(context, v8_string("code")));
		if (!script->IsString()) {
			throw js_type_error("`code` property is required");
		}
		ScriptOriginHolder script_origin(script_handle.As<Object>());
		scripts.emplace_back(std::string(*String::Utf8Value(script.As<String>())), std::move(script_origin));
	}
	std::string warmup_script;
	if (!warmup_handle.IsEmpty()) {
		warmup_script = *String::Utf8Value(warmup_handle.ToLocalChecked().As<String>());
	}

	// Create the snapshot
	StartupData snapshot {};
	unique_ptr<const char> snapshot_data_ptr;
	shared_ptr<ExternalCopy> error;
	{
		SnapshotCreator snapshot_creator;
		Isolate* isolate = snapshot_creator.GetIsolate();
		{
			Locker locker(isolate);
			TryCatch try_catch(isolate);
			HandleScope handle_scope(isolate);
			Local<Context> context = Context::New(isolate);
			snapshot_creator.SetDefaultContext(context);
			try {
				{
					HandleScope handle_scope(isolate);
					Local<Context> context_dirty = Context::New(isolate);
					for (auto& script : scripts) {
						Local<String> code = v8_string(script.first.c_str());
						ScriptOrigin script_origin = script.second.ToScriptOrigin();
						ScriptCompiler::Source source(code, script_origin);
						Local<UnboundScript> unbound_script;
						{
							Context::Scope context_scope(context);
							Local<Script> compiled_script = RunWithAnnotatedErrors<Local<Script>>(
								[&context, &source]() { return Unmaybe(ScriptCompiler::Compile(context, &source, ScriptCompiler::kNoCompileOptions)); }
							);
							Unmaybe(compiled_script->Run(context));
							unbound_script = compiled_script->GetUnboundScript();
						}
						{
							Context::Scope context_scope(context_dirty);
							Unmaybe(unbound_script->BindToCurrentContext()->Run(context_dirty));
						}
					}
					if (warmup_script.length() != 0) {
						Context::Scope context_scope(context_dirty);
						MaybeLocal<Object> tmp;
						ScriptOriginHolder script_origin(tmp);
						ScriptCompiler::Source source(v8_string(warmup_script.c_str()), script_origin.ToScriptOrigin());
						RunWithAnnotatedErrors<void>([&context_dirty, &source]() {
							Unmaybe(Unmaybe(ScriptCompiler::Compile(context_dirty, &source, ScriptCompiler::kNoCompileOptions))->Run(context_dirty));
						});
					}
				}
				isolate->ContextDisposedNotification(false);
				snapshot_creator.AddContext(context);
			} catch (const js_runtime_error& cc_error) {
				assert(try_catch.HasCaught());
				HandleScope handle_scope(isolate);
				Context::Scope context_scope(context);
				error = ExternalCopy::CopyIfPrimitiveOrError(try_catch.Exception());
			}
		}
		if (!error) {
			snapshot = snapshot_creator.CreateBlob(SnapshotCreator::FunctionCodeHandling::kKeep);
			snapshot_data_ptr.reset(snapshot.data);
		}
	}

	// Export to outer scope
	if (error) {
		Isolate::GetCurrent()->ThrowException(error->CopyInto());
		return Undefined(Isolate::GetCurrent());
	} else if (snapshot.raw_size == 0) {
		throw js_generic_error("Failure creating snapshot");
	}
	auto buffer = std::make_shared<ExternalCopyArrayBuffer>((void*)snapshot.data, snapshot.raw_size);
	return ClassHandle::NewInstance<ExternalCopyHandle>(buffer);
}

} // namespace ivm
