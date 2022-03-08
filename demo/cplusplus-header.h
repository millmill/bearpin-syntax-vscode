#ifndef V8_API_H_
#define V8_API_H_

#include "src/objects/shared-function-info.h"

#include "src/objects/templates.h"

namespace v8 {

// Constants used in the implementation of the API.  The most natural thing

class Consts {
 public:
  enum TemplateType {
    FUNCTION_TEMPLATE = 0,
    OBJECT_TEMPLATE = 1
  };
};

template <typename T>
inline T ToCData(v8::internal::Object* obj);

template <>
inline v8::internal::Address ToCData(v8::internal::Object* obj);

template <typename T>
inline v8::internal::Handle<v8::internal::Object> FromCData(
    v8::internal::Isolate* isolate, T obj);

template <>
inline v8::internal::Handle<v8::internal::Object> FromCData(
    v8::internal::Isolate* isolate, v8::internal::Address obj);

class ApiFunction {
 public:
  explicit ApiFunction(v8::internal::Address addr) : addr_(addr) { }
  v8::internal::Address address() { return addr_; }
 private:
  v8::internal::Address addr_;
};



class RegisteredExtension {
 public:
  explicit RegisteredExtension(Extension* extension);
  static void Register(RegisteredExtension* that);
  static void UnregisterAll();
  Extension* extension() { return extension_; }
  RegisteredExtension* next() { return next_; }
  static RegisteredExtension* first_extension() { return first_extension_; }
 private:
  Extension* extension_;
  RegisteredExtension* next_;
  static RegisteredExtension* first_extension_;
};

#define OPEN_HANDLE_LIST(V)                    \
  V(Template, TemplateInfo)                    \
  V(FunctionTemplate, FunctionTemplateInfo)    \
  V(debug::GeneratorObject, JSGeneratorObject) \
  V(debug::Script, Script)                     \
  V(debug::WeakMap, JSWeakMap)                 \
  V(ScriptOrModule, Script)

class Utils {
 public:
  static inline bool ApiCheck(bool condition,
                              const char* location,
                              const char* message) {
    if (!condition) Utils::ReportApiFailure(location, message);
    return condition;
  }
  static void ReportOOMFailure(v8::internal::Isolate* isolate,
                               const char* location, bool is_heap_oom);

  static inline Local<Context> ToLocal(
      v8::internal::Handle<v8::internal::Context> obj);

#define DECLARE_OPEN_HANDLE(From, To) \
  static inline v8::internal::Handle<v8::internal::To> \
      OpenHandle(const From* that, bool allow_empty_handle = false);

OPEN_HANDLE_LIST(DECLARE_OPEN_HANDLE)

#undef DECLARE_OPEN_HANDLE

template <class From, class To>
static inline Local<To> Convert(v8::internal::Handle<From> obj);

template <class T>
static inline v8::internal::Handle<v8::internal::Object> OpenPersistent(
    const v8::Persistent<T>& persistent) {
  return v8::internal::Handle<v8::internal::Object>(
      reinterpret_cast<v8::internal::Object**>(persistent.val_));
  }

 private:
  static void ReportApiFailure(const char* location, const char* message);
};


template <class T>
inline bool ToLocal(v8::internal::MaybeHandle<v8::internal::Object> maybe,
                    Local<T>* local) {
  v8::internal::Handle<v8::internal::Object> handle;
  if (maybe.ToHandle(&handle)) {
    *local = Utils::Convert<v8::internal::Object, T>(handle);
    return true;
  }
  return false;
}

namespace internal {

class V8_EXPORT_PRIVATE DeferredHandles {
 public:
  ~DeferredHandles();

 private:
  DeferredHandles(Object** first_block_limit, Isolate* isolate)
      : next_(nullptr),
        previous_(nullptr),
        first_block_limit_(first_block_limit),
        isolate_(isolate) {
    isolate->LinkDeferredHandles(this);
  }

  void Iterate(RootVisitor* v);

  std::vector<Object**> blocks_;
  DeferredHandles* next_;
  DeferredHandles* previous_;
  Object** first_block_limit_;
  Isolate* isolate_;

  friend class HandleScopeImplementer;
  friend class Isolate;
};


// This class is here in order to be able to declare it a friend of
// HandleScope.  Moving these methods to be members of HandleScope would be

class HandleScopeImplementer {
 public:
  explicit HandleScopeImplementer(Isolate* isolate)
      : isolate_(isolate),
        microtask_context_(nullptr),
        spare_(nullptr),
        call_depth_(0),
        microtasks_depth_(0),
        microtasks_suppressions_(0),
        entered_contexts_count_(0),
        entered_context_count_during_microtasks_(0),
#ifdef DEBUG
        debug_microtasks_depth_(0),
#endif
        microtasks_policy_(v8::MicrotasksPolicy::kAuto),
        last_handle_before_deferred_block_(nullptr) {
  }

  ~HandleScopeImplementer() {
    DeleteArray(spare_);
  }

  // Threading support for handle data.
  static int ArchiveSpacePerThread();
  char* RestoreThread(char* from);
  char* ArchiveThread(char* to);
  void FreeThreadResources();

  // Garbage collection support.
  void Iterate(v8::internal::RootVisitor* v);
  static char* Iterate(v8::internal::RootVisitor* v, char* data);

  inline internal::Object** GetSpareOrNewBlock();
  inline void DeleteExtensions(internal::Object** prev_limit);

  // Call depth represents nested v8 api calls.
  inline void IncrementCallDepth() {call_depth_++;}
  inline void DecrementCallDepth() {call_depth_--;}
  inline bool CallDepthIsZero() { return call_depth_ == 0; }

#ifdef DEBUG
  // In debug we check that calls not intended to invoke microtasks are
  // still correctly wrapped with microtask scopes.
  inline void IncrementDebugMicrotasksScopeDepth() {debug_microtasks_depth_++;}
  inline void DecrementDebugMicrotasksScopeDepth() {debug_microtasks_depth_--;}
  inline bool DebugMicrotasksScopeDepthIsZero() {
    return debug_microtasks_depth_ == 0;
  }
#endif

  inline void set_microtasks_policy(v8::MicrotasksPolicy policy);
  inline v8::MicrotasksPolicy microtasks_policy() const;

  inline void EnterContext(Handle<Context> context);

  inline bool MicrotaskContextIsLastEnteredContext() const {
    return microtask_context_ &&
           entered_context_count_during_microtasks_ == entered_contexts_.size();
  }

  inline void SaveContext(Context* context);
  inline Context* RestoreContext();
  inline bool HasSavedContexts();

  inline DetachableVector<Object**>* blocks() { return &blocks_; }
  Isolate* isolate() const { return isolate_; }

  void ReturnBlock(Object** block) {
    DCHECK_NOT_NULL(block);
    if (spare_ != nullptr) DeleteArray(spare_);
    spare_ = block;
  }

 private:
  void ResetAfterArchive() {
    blocks_.detach();
    entered_contexts_.detach();
    saved_contexts_.detach();
    microtask_context_ = nullptr;
    entered_context_count_during_microtasks_ = 0;
    spare_ = nullptr;
    last_handle_before_deferred_block_ = nullptr;
    call_depth_ = 0;
  }

  void Free() {
    DCHECK(blocks_.empty());
    DCHECK(entered_contexts_.empty());
    DCHECK(saved_contexts_.empty());
    DCHECK(!microtask_context_);

    blocks_.free();
    entered_contexts_.free();
    saved_contexts_.free();
    if (spare_ != nullptr) {
      DeleteArray(spare_);
      spare_ = nullptr;
    }
    DCHECK_EQ(call_depth_, 0);
  }

  void BeginDeferredScope();
  DeferredHandles* Detach(Object** prev_limit);

  Isolate* isolate_;
  DetachableVector<Object**> blocks_;
  // Used as a stack to keep track of entered contexts.
  DetachableVector<Context*> entered_contexts_;
  // Used as a stack to keep track of saved contexts.
  DetachableVector<Context*> saved_contexts_;
  Context* microtask_context_;
  Object** spare_;
  int call_depth_;
  int microtasks_depth_;
  int microtasks_suppressions_;
  size_t entered_contexts_count_;
  size_t entered_context_count_during_microtasks_;
#ifdef DEBUG
  int debug_microtasks_depth_;
#endif
  v8::MicrotasksPolicy microtasks_policy_;
  Object** last_handle_before_deferred_block_;
  // This is only used for threading support.
  HandleScopeData handle_scope_data_;

  void IterateThis(RootVisitor* v);
  char* RestoreThreadHelper(char* from);
  char* ArchiveThreadHelper(char* to);

  friend class DeferredHandles;
  friend class DeferredHandleScope;
  friend class HandleScopeImplementerOffsets;

  DISALLOW_COPY_AND_ASSIGN(HandleScopeImplementer);
};

class HandleScopeImplementerOffsets {
 public:
  enum Offsets {
    kMicrotaskContext = offsetof(HandleScopeImplementer, microtask_context_),
    kEnteredContexts = offsetof(HandleScopeImplementer, entered_contexts_),
    kEnteredContextsCount =
        offsetof(HandleScopeImplementer, entered_contexts_count_),
    kEnteredContextCountDuringMicrotasks = offsetof(
        HandleScopeImplementer, entered_context_count_during_microtasks_)
  };

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(HandleScopeImplementerOffsets);
};

const int kHandleBlockSize = v8::internal::KB - 2;  // fit in one page


void HandleScopeImplementer::set_microtasks_policy(
    v8::MicrotasksPolicy policy) {
  microtasks_policy_ = policy;
}


v8::MicrotasksPolicy HandleScopeImplementer::microtasks_policy() const {
  return microtasks_policy_;
}

Context* HandleScopeImplementer::RestoreContext() {
  Context* last_context = saved_contexts_.back();
  saved_contexts_.pop_back();
  return last_context;
}

bool HandleScopeImplementer::LastEnteredContextWas(Handle<Context> context) {
  return !entered_contexts_.empty() && entered_contexts_.back() == *context;
}

void HandleScopeImplementer::DeleteExtensions(internal::Object** prev_limit) {
  while (!blocks_.empty()) {
    internal::Object** block_start = blocks_.back();
    internal::Object** block_limit = block_start + kHandleBlockSize;

    // SealHandleScope may make the prev_limit to point inside the block.
    if (block_start <= prev_limit && prev_limit <= block_limit) {
#ifdef ENABLE_HANDLE_ZAPPING
      internal::HandleScope::ZapRange(prev_limit, block_limit);
#endif
      break;
    }

    blocks_.pop_back();
#ifdef ENABLE_HANDLE_ZAPPING
    internal::HandleScope::ZapRange(block_start, block_limit);
#endif
    if (spare_ != nullptr) {
      DeleteArray(spare_);
    }
    spare_ = block_start;
  }
  DCHECK((blocks_.empty() && prev_limit == nullptr) ||
         (!blocks_.empty() && prev_limit != nullptr));
}

}  // namespace internal
}  // namespace v8

#endif  // V8_API_H_