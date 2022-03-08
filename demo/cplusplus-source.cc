#include <iostream>
using namespace std;

int main() {

  int year;
  cout << "Enter a year: ";
  cin >> year;

  // leap year if perfectly divisible by 400
  if (year % 400 == 0) {
    cout << year << " is a leap year.";
  }
  // not a leap year if divisible by 100
  // but not divisible by 400
  else if (year % 100 == 0) {
    cout << year << " is not a leap year.";
  }
  // leap year if not divisible by 100
  // but divisible by 4
  else if (year % 4 == 0) {
    cout << year << " is a leap year.";
  }
  // all other years are not leap years
  else {
    cout << year << " is not a leap year.";
  }

  return 0;
}


#define LOG_API(isolate, class_name, function_name)                           \
  i::RuntimeCallTimerScope _runtime_timer(                                    \
      isolate, i::RuntimeCallCounterId::kAPI_##class_name##_##function_name); \
  LOG(isolate, ApiEntryCall("v8::" #class_name "::" #function_name))

namespace {

Local<Context> ContextFromNeverReadOnlySpaceObject(
    i::Handle<i::NeverReadOnlySpaceObject> obj) {
  return reinterpret_cast<v8::Isolate*>(obj->GetIsolate())->GetCurrentContext();
}

class InternalEscapableScope : public v8::EscapableHandleScope {
 public:
  explicit inline InternalEscapableScope(i::Isolate* isolate)
      : v8::EscapableHandleScope(reinterpret_cast<v8::Isolate*>(isolate)) {}
};

// TODO(jochen): This should be #ifdef DEBUG
#ifdef V8_CHECK_MICROTASKS_SCOPES_CONSISTENCY
void CheckMicrotasksScopesConsistency(i::Isolate* isolate) {
  auto handle_scope_implementer = isolate->handle_scope_implementer();
  if (handle_scope_implementer->microtasks_policy() ==
      v8::MicrotasksPolicy::kScoped) {
    DCHECK(handle_scope_implementer->GetMicrotasksScopeDepth() ||
           !handle_scope_implementer->DebugMicrotasksScopeDepthIsZero());
  }
}
#endif

template <bool do_callback>
class CallDepthScope {
 public:
  explicit CallDepthScope(i::Isolate* isolate, Local<Context> context)
      : isolate_(isolate),
        context_(context),
        escaped_(false),
        safe_for_termination_(isolate->next_v8_call_is_safe_for_termination()),
        interrupts_scope_(isolate_, i::StackGuard::TERMINATE_EXECUTION,
                          isolate_->only_terminate_in_safe_scope()
                              ? (safe_for_termination_
                                     ? i::InterruptsScope::kRunInterrupts
                                     : i::InterruptsScope::kPostponeInterrupts)
                              : i::InterruptsScope::kNoop) {
    // TODO(dcarney): remove this when blink stops crashing.
    DCHECK(!isolate_->external_caught_exception());
    isolate_->handle_scope_implementer()->IncrementCallDepth();
    isolate_->set_next_v8_call_is_safe_for_termination(false);
    if (!context.IsEmpty()) {
      i::Handle<i::Context> env = Utils::OpenHandle(*context);
      i::HandleScopeImplementer* impl = isolate->handle_scope_implementer();
      if (isolate->context() != nullptr &&
          isolate->context()->native_context() == env->native_context()) {
        context_ = Local<Context>();
      } else {
        impl->SaveContext(isolate->context());
        isolate->set_context(*env);
      }
    }
    if (do_callback) isolate_->FireBeforeCallEnteredCallback();
  }
  ~CallDepthScope() {
    if (!context_.IsEmpty()) {
      i::HandleScopeImplementer* impl = isolate_->handle_scope_implementer();
      isolate_->set_context(impl->RestoreContext());
    }
    if (!escaped_) isolate_->handle_scope_implementer()->DecrementCallDepth();
    if (do_callback) isolate_->FireCallCompletedCallback();
// TODO(jochen): This should be #ifdef DEBUG
#ifdef V8_CHECK_MICROTASKS_SCOPES_CONSISTENCY
    if (do_callback) CheckMicrotasksScopesConsistency(isolate_);
#endif
    isolate_->set_next_v8_call_is_safe_for_termination(safe_for_termination_);
  }

  void Escape() {
    DCHECK(!escaped_);
    escaped_ = true;
    auto handle_scope_implementer = isolate_->handle_scope_implementer();
    handle_scope_implementer->DecrementCallDepth();
    bool call_depth_is_zero = handle_scope_implementer->CallDepthIsZero();
    isolate_->OptionalRescheduleException(call_depth_is_zero);
  }

 private:
  i::Isolate* const isolate_;
  Local<Context> context_;
  bool escaped_;
  bool do_callback_;
  bool safe_for_termination_;
  i::InterruptsScope interrupts_scope_;
};

}  // namespace


static ScriptOrigin GetScriptOriginForScript(i::Isolate* isolate,
                                             i::Handle<i::Script> script) {
  i::Handle<i::Object> scriptName(script->GetNameOrSourceURL(), isolate);
  i::Handle<i::Object> source_map_url(script->source_mapping_url(), isolate);
  i::Handle<i::FixedArray> host_defined_options(script->host_defined_options(),
                                                isolate);
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  ScriptOriginOptions options(script->origin_options());
  v8::ScriptOrigin origin(
      Utils::ToLocal(scriptName),
      v8::Integer::New(v8_isolate, script->line_offset()),
      v8::Integer::New(v8_isolate, script->column_offset()),
      v8::Boolean::New(v8_isolate, options.IsSharedCrossOrigin()),
      v8::Integer::New(v8_isolate, script->id()),
      Utils::ToLocal(source_map_url),
      v8::Boolean::New(v8_isolate, options.IsOpaque()),
      v8::Boolean::New(v8_isolate, script->type() == i::Script::TYPE_WASM),
      v8::Boolean::New(v8_isolate, options.IsModule()),
      Utils::ToLocal(host_defined_options));
  return origin;
}


namespace {

template <typename Getter, typename Setter>
i::Handle<i::AccessorInfo> MakeAccessorInfo(
    i::Isolate* isolate, v8::Local<Name> name, Getter getter, Setter setter,
    v8::Local<Value> data, v8::AccessControl settings,
    v8::Local<AccessorSignature> signature, bool is_special_data_property,
    bool replace_on_access) {
  i::Handle<i::AccessorInfo> obj = isolate->factory()->NewAccessorInfo();
  SET_FIELD_WRAPPED(isolate, obj, set_getter, getter);
  DCHECK_IMPLIES(replace_on_access,
                 is_special_data_property && setter == nullptr);
  if (is_special_data_property && setter == nullptr) {
    setter = reinterpret_cast<Setter>(&i::Accessors::ReconfigureToDataProperty);
  }
  SET_FIELD_WRAPPED(isolate, obj, set_setter, setter);
  i::Address redirected = obj->redirected_getter();
  if (redirected != i::kNullAddress) {
    SET_FIELD_WRAPPED(isolate, obj, set_js_getter, redirected);
  }
  if (data.IsEmpty()) {
    data = v8::Undefined(reinterpret_cast<v8::Isolate*>(isolate));
  }
  obj->set_data(*Utils::OpenHandle(*data));
  obj->set_is_special_data_property(is_special_data_property);
  obj->set_replace_on_access(replace_on_access);
  i::Handle<i::Name> accessor_name = Utils::OpenHandle(*name);
  if (!accessor_name->IsUniqueName()) {
    accessor_name = isolate->factory()->InternalizeString(
        i::Handle<i::String>::cast(accessor_name));
  }
  obj->set_name(*accessor_name);
  if (settings & ALL_CAN_READ) obj->set_all_can_read(true);
  if (settings & ALL_CAN_WRITE) obj->set_all_can_write(true);
  obj->set_initial_property_attributes(i::NONE);
  if (!signature.IsEmpty()) {
    obj->set_expected_receiver_type(*Utils::OpenHandle(*signature));
  }
  return obj;
}

void EmbedderHeapTracer::GarbageCollectionForTesting(
    EmbedderStackState stack_state) {
  CHECK(isolate_);
  CHECK(i::FLAG_expose_gc);
  i::Heap* const heap = reinterpret_cast<i::Isolate*>(isolate_)->heap();
  heap->SetEmbedderStackStateForNextFinalizaton(stack_state);
  heap->PreciseCollectAllGarbage(i::Heap::kNoGCFlags,
                                 i::GarbageCollectionReason::kTesting,
                                 kGCCallbackFlagForced);



int HandleScopeImplementer::ArchiveSpacePerThread() {
  return sizeof(HandleScopeImplementer);
}


char* HandleScopeImplementer::RestoreThread(char* storage) {
  MemCopy(this, storage, sizeof(*this));
  *isolate_->handle_scope_data() = handle_scope_data_;
  return storage + ArchiveSpacePerThread();
}

void HandleScopeImplementer::IterateThis(RootVisitor* v) {
#ifdef DEBUG
  bool found_block_before_deferred = false;
#endif
  // Iterate over all handles in the blocks except for the last.
  for (int i = static_cast<int>(blocks()->size()) - 2; i >= 0; --i) {
    Object** block = blocks()->at(i);
    if (last_handle_before_deferred_block_ != nullptr &&
        (last_handle_before_deferred_block_ <= &block[kHandleBlockSize]) &&
        (last_handle_before_deferred_block_ >= block)) {
      v->VisitRootPointers(Root::kHandleScope, nullptr, ObjectSlot(block),
                           ObjectSlot(last_handle_before_deferred_block_));
      DCHECK(!found_block_before_deferred);
#ifdef DEBUG
      found_block_before_deferred = true;
#endif
    } else {
      v->VisitRootPointers(Root::kHandleScope, nullptr, ObjectSlot(block),
                           ObjectSlot(&block[kHandleBlockSize]));
    }
  }

  DCHECK(last_handle_before_deferred_block_ == nullptr ||
         found_block_before_deferred);

  // Iterate over live handles in the last block (if any).
  if (!blocks()->empty()) {
    v->VisitRootPointers(Root::kHandleScope, nullptr,
                         ObjectSlot(blocks()->back()),
                         ObjectSlot(handle_scope_data_.next));
  }

  DetachableVector<Context*>* context_lists[2] = {&saved_contexts_,
                                                  &entered_contexts_};
  for (unsigned i = 0; i < arraysize(context_lists); i++) {
    if (context_lists[i]->empty()) continue;
    ObjectSlot start(reinterpret_cast<Address>(&context_lists[i]->front()));
    v->VisitRootPointers(Root::kHandleScope, nullptr, start,
                         start + static_cast<int>(context_lists[i]->size()));
  }
  if (microtask_context_) {
    v->VisitRootPointer(
        Root::kHandleScope, nullptr,
        ObjectSlot(reinterpret_cast<Address>(&microtask_context_)));
  }
}

void HandleScopeImplementer::Iterate(RootVisitor* v) {
  HandleScopeData* current = isolate_->handle_scope_data();
  handle_scope_data_ = *current;
  IterateThis(v);
}

void HandleScopeImplementer::BeginDeferredScope() {
  DCHECK_NULL(last_handle_before_deferred_block_);
  last_handle_before_deferred_block_ = isolate()->handle_scope_data()->next;
}


DeferredHandles::~DeferredHandles() {
  isolate_->UnlinkDeferredHandles(this);

  for (size_t i = 0; i < blocks_.size(); i++) {
#ifdef ENABLE_HANDLE_ZAPPING
    HandleScope::ZapRange(blocks_[i], &blocks_[i][kHandleBlockSize]);
#endif
    isolate_->handle_scope_implementer()->ReturnBlock(blocks_[i]);
  }
}