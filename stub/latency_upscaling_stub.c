// EXPERIMENTAL, NOT SHIPPED. Raw-C GDExtension no-op stub for platforms without the hook.
//
// Kept for a later release; see stub/README.md for why it is not shipped yet (no ptrcall paths, no
// properties, macOS binaries not verified). The shipped addon has no library for non-Windows-x64
// platforms and documents excluding it from those exports instead.
//
// What it does: registers the same "LatencyUpscaling" singleton with a few methods that report the
// platform is not supported. It uses only the GDExtension C ABI (no godot-cpp, no libc), so one
// source compiles for every target; only 64-bit targets are built, so the opaque built-in handles
// (String, StringName, Dictionary) are one pointer (8 bytes) each and a Variant is 24 bytes.
// Every temporary String/StringName/Dictionary/Variant it creates is destroyed again, and the
// singleton object and the class are released at SCENE deinitialisation.
//
// Build: python stub/build_stubs.py (into build/stubs/)   Check: python stub/test_stub.py

#include <gdextension_interface.h>

#include <stddef.h>
#include <stdint.h>

#define LU_STUB_VERSION "0.1.0"
#define LU_NOT_SUPPORTED "not supported on this platform"

typedef struct {
	uint8_t data[8];
} OpaqueString;
typedef struct {
	uint8_t data[8];
} OpaqueStringName;
typedef struct {
	uint8_t data[8];
} OpaqueDictionary;
typedef struct {
	uint8_t data[24];
} OpaqueVariant;

// --- resolved interface functions -----------------------------------------------------------
static GDExtensionClassLibraryPtr g_library;
static GDExtensionInterfaceStringNameNewWithUtf8Chars fn_sn_new;
static GDExtensionInterfaceStringNewWithUtf8Chars fn_string_new;
static GDExtensionInterfaceClassdbRegisterExtensionClass4 fn_register_class;
static GDExtensionInterfaceClassdbUnregisterExtensionClass fn_unregister_class;
static GDExtensionInterfaceClassdbRegisterExtensionClassMethod fn_register_method;
static GDExtensionInterfaceClassdbConstructObject2 fn_construct_object;
static GDExtensionInterfaceObjectSetInstance fn_object_set_instance;
static GDExtensionInterfaceObjectDestroy fn_object_destroy;
static GDExtensionInterfaceGlobalGetSingleton fn_global_get_singleton;
static GDExtensionInterfaceClassdbGetMethodBind fn_get_method_bind;
static GDExtensionInterfaceObjectMethodBindPtrcall fn_method_bind_ptrcall;
static GDExtensionInterfaceGetVariantFromTypeConstructor fn_variant_from_type;
static GDExtensionInterfaceVariantGetPtrConstructor fn_get_ptr_constructor;
static GDExtensionInterfaceVariantGetPtrDestructor fn_get_ptr_destructor;
static GDExtensionInterfaceVariantGetPtrKeyedSetter fn_get_keyed_setter;
static GDExtensionInterfaceVariantDestroy fn_variant_destroy;

// Names that must stay valid while the class is registered.
static OpaqueStringName g_class_name; // "LatencyUpscaling"
static OpaqueStringName g_object_name; // "Object"
static int g_class_registered;
static GDExtensionObjectPtr g_singleton;

static void destroy_string(OpaqueString *s) {
	fn_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING)(s);
}

static void destroy_string_name(OpaqueStringName *s) {
	fn_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING_NAME)(s);
}

// --- return-value helpers (write a Variant into r_return) -----------------------------------
static void return_bool(GDExtensionVariantPtr r_return, int value) {
	GDExtensionBool v = value ? 1 : 0;
	fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_BOOL)(r_return, &v);
}

static void return_string(GDExtensionVariantPtr r_return, const char *text) {
	OpaqueString str;
	fn_string_new(&str, text);
	fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_STRING)(r_return, &str); // copies
	destroy_string(&str);
}

static void set_string_key(OpaqueDictionary *dict, const char *key, OpaqueVariant *value) {
	OpaqueString key_str;
	OpaqueVariant key_var;
	fn_string_new(&key_str, key);
	fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_STRING)(&key_var, &key_str);
	fn_get_keyed_setter(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(dict, &key_var, value); // copies both
	fn_variant_destroy(&key_var);
	destroy_string(&key_str);
}

// {"platform_supported": false, "reason": "not supported on this platform"}
static void return_status_dictionary(GDExtensionVariantPtr r_return) {
	OpaqueDictionary dict;
	fn_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY, 0)(&dict, NULL);

	OpaqueVariant value;
	GDExtensionBool supported = 0;
	fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_BOOL)(&value, &supported);
	set_string_key(&dict, "platform_supported", &value);
	fn_variant_destroy(&value);

	OpaqueString reason;
	fn_string_new(&reason, LU_NOT_SUPPORTED);
	fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_STRING)(&value, &reason);
	set_string_key(&dict, "reason", &value);
	fn_variant_destroy(&value);
	destroy_string(&reason);

	fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(r_return, &dict); // copies
	fn_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY)(&dict);
}

// --- method call callbacks (variant path, used by dynamic calls) -----------------------------
#define LU_CALL_PREAMBLE \
	(void)ud;            \
	(void)inst;          \
	(void)args;          \
	(void)argc;          \
	if (err) {           \
		err->error = GDEXTENSION_CALL_OK; \
	}

static void call_false(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	LU_CALL_PREAMBLE
	return_bool(r_return, 0);
}

static void call_get_reflex_status(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	LU_CALL_PREAMBLE
	return_string(r_return, LU_NOT_SUPPORTED);
}

static void call_get_version(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	LU_CALL_PREAMBLE
	return_string(r_return, LU_STUB_VERSION);
}

static void call_get_status(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	LU_CALL_PREAMBLE
	return_status_dictionary(r_return);
}

// --- class lifecycle -------------------------------------------------------------------------
static GDExtensionObjectPtr create_instance(void *class_userdata, GDExtensionBool notify) {
	(void)class_userdata;
	(void)notify;
	GDExtensionObjectPtr object = fn_construct_object(&g_object_name); // a bare Object
	fn_object_set_instance(object, &g_class_name, (GDExtensionClassInstancePtr)(void *)object);
	return object;
}

static void free_instance(void *class_userdata, GDExtensionClassInstancePtr instance) {
	(void)class_userdata;
	(void)instance; // the instance token is the Object itself; nothing extra was allocated
}

static GDExtensionClassCallVirtual get_virtual(void *class_userdata, GDExtensionConstStringNamePtr name, uint32_t hash) {
	(void)class_userdata;
	(void)name;
	(void)hash;
	return NULL; // no virtual methods are overridden
}

static void register_method(const char *name, GDExtensionClassMethodCall call, GDExtensionVariantType return_type) {
	OpaqueStringName method_name;
	OpaqueStringName empty_name;
	OpaqueString empty_string;
	fn_sn_new(&method_name, name);
	fn_sn_new(&empty_name, "");
	fn_string_new(&empty_string, "");

	GDExtensionPropertyInfo return_info;
	return_info.type = return_type;
	return_info.name = &empty_name; // Godot copies these during registration; NULL crashes it
	return_info.class_name = &empty_name;
	return_info.hint = 0;
	return_info.hint_string = &empty_string;
	return_info.usage = 6; // PROPERTY_USAGE_DEFAULT

	GDExtensionClassMethodInfo info;
	info.name = &method_name;
	info.method_userdata = NULL;
	info.call_func = call;
	info.ptrcall_func = NULL; // KNOWN GAP (why this is not shipped): a typed ptrcall crashes
	info.method_flags = GDEXTENSION_METHOD_FLAG_NORMAL | GDEXTENSION_METHOD_FLAG_CONST;
	info.has_return_value = 1;
	info.return_value_info = &return_info;
	info.return_value_metadata = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
	info.argument_count = 0;
	info.arguments_info = NULL;
	info.arguments_metadata = NULL;
	info.default_argument_count = 0;
	info.default_arguments = NULL;
	fn_register_method(g_library, &g_class_name, &info);

	destroy_string(&empty_string);
	destroy_string_name(&empty_name);
	destroy_string_name(&method_name);
}

// Calls Engine.register_singleton / unregister_singleton through their method binds.
static void engine_call(const char *method, int64_t hash, const GDExtensionConstTypePtr *call_args) {
	OpaqueStringName engine_name;
	OpaqueStringName method_name;
	fn_sn_new(&engine_name, "Engine");
	fn_sn_new(&method_name, method);
	GDExtensionObjectPtr engine = fn_global_get_singleton(&engine_name);
	GDExtensionMethodBindPtr bind = fn_get_method_bind(&engine_name, &method_name, hash);
	if (engine && bind) {
		fn_method_bind_ptrcall(bind, engine, call_args, NULL);
	}
	destroy_string_name(&method_name);
	destroy_string_name(&engine_name);
}

static void register_class_and_singleton(void) {
	fn_sn_new(&g_class_name, "LatencyUpscaling");
	fn_sn_new(&g_object_name, "Object");

	GDExtensionClassCreationInfo4 ci;
	uint8_t *raw = (uint8_t *)&ci;
	for (size_t i = 0; i < sizeof(ci); ++i) {
		raw[i] = 0;
	}
	ci.is_exposed = 1;
	ci.create_instance_func = create_instance;
	ci.free_instance_func = free_instance;
	ci.get_virtual_func = get_virtual;
	fn_register_class(g_library, &g_class_name, &g_object_name, &ci);
	g_class_registered = 1;

	register_method("is_reflex_supported", call_false, GDEXTENSION_VARIANT_TYPE_BOOL);
	register_method("is_reflex_active", call_false, GDEXTENSION_VARIANT_TYPE_BOOL);
	register_method("get_reflex_status", call_get_reflex_status, GDEXTENSION_VARIANT_TYPE_STRING);
	register_method("get_version", call_get_version, GDEXTENSION_VARIANT_TYPE_STRING);
	register_method("get_status", call_get_status, GDEXTENSION_VARIANT_TYPE_DICTIONARY);

	g_singleton = fn_construct_object(&g_class_name);
	const GDExtensionConstTypePtr call_args[2] = { &g_class_name, &g_singleton };
	engine_call("register_singleton", 965313290, call_args);
}

static void release_class_and_singleton(void) {
	if (g_singleton) {
		const GDExtensionConstTypePtr call_args[1] = { &g_class_name };
		engine_call("unregister_singleton", 3304788590u, call_args);
		fn_object_destroy(g_singleton);
		g_singleton = NULL;
	}
	if (g_class_registered) {
		fn_unregister_class(g_library, &g_class_name);
		g_class_registered = 0;
		destroy_string_name(&g_object_name);
		destroy_string_name(&g_class_name);
	}
}

// --- initialization --------------------------------------------------------------------------
static void initialize(void *userdata, GDExtensionInitializationLevel level) {
	(void)userdata;
	if (level == GDEXTENSION_INITIALIZATION_SCENE) {
		register_class_and_singleton();
	}
}

static void deinitialize(void *userdata, GDExtensionInitializationLevel level) {
	(void)userdata;
	if (level == GDEXTENSION_INITIALIZATION_SCENE) {
		release_class_and_singleton();
	}
}

#if defined(_WIN32)
#define LU_EXPORT __declspec(dllexport)
#else
#define LU_EXPORT __attribute__((visibility("default")))
#endif

#define LU_RESOLVE(var, type, name) var = (type)get_proc_address(name)

LU_EXPORT GDExtensionBool godot_latency_upscaling_init(GDExtensionInterfaceGetProcAddress get_proc_address,
		GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
	g_library = library;
	LU_RESOLVE(fn_sn_new, GDExtensionInterfaceStringNameNewWithUtf8Chars, "string_name_new_with_utf8_chars");
	LU_RESOLVE(fn_string_new, GDExtensionInterfaceStringNewWithUtf8Chars, "string_new_with_utf8_chars");
	LU_RESOLVE(fn_register_class, GDExtensionInterfaceClassdbRegisterExtensionClass4, "classdb_register_extension_class4");
	LU_RESOLVE(fn_unregister_class, GDExtensionInterfaceClassdbUnregisterExtensionClass, "classdb_unregister_extension_class");
	LU_RESOLVE(fn_register_method, GDExtensionInterfaceClassdbRegisterExtensionClassMethod, "classdb_register_extension_class_method");
	LU_RESOLVE(fn_construct_object, GDExtensionInterfaceClassdbConstructObject2, "classdb_construct_object2");
	LU_RESOLVE(fn_object_set_instance, GDExtensionInterfaceObjectSetInstance, "object_set_instance");
	LU_RESOLVE(fn_object_destroy, GDExtensionInterfaceObjectDestroy, "object_destroy");
	LU_RESOLVE(fn_global_get_singleton, GDExtensionInterfaceGlobalGetSingleton, "global_get_singleton");
	LU_RESOLVE(fn_get_method_bind, GDExtensionInterfaceClassdbGetMethodBind, "classdb_get_method_bind");
	LU_RESOLVE(fn_method_bind_ptrcall, GDExtensionInterfaceObjectMethodBindPtrcall, "object_method_bind_ptrcall");
	LU_RESOLVE(fn_variant_from_type, GDExtensionInterfaceGetVariantFromTypeConstructor, "get_variant_from_type_constructor");
	LU_RESOLVE(fn_get_ptr_constructor, GDExtensionInterfaceVariantGetPtrConstructor, "variant_get_ptr_constructor");
	LU_RESOLVE(fn_get_ptr_destructor, GDExtensionInterfaceVariantGetPtrDestructor, "variant_get_ptr_destructor");
	LU_RESOLVE(fn_get_keyed_setter, GDExtensionInterfaceVariantGetPtrKeyedSetter, "variant_get_ptr_keyed_setter");
	LU_RESOLVE(fn_variant_destroy, GDExtensionInterfaceVariantDestroy, "variant_destroy");

	if (!fn_sn_new || !fn_string_new || !fn_register_class || !fn_unregister_class || !fn_register_method ||
			!fn_construct_object || !fn_object_set_instance || !fn_object_destroy || !fn_global_get_singleton ||
			!fn_get_method_bind || !fn_method_bind_ptrcall || !fn_variant_from_type || !fn_get_ptr_constructor ||
			!fn_get_ptr_destructor || !fn_get_keyed_setter || !fn_variant_destroy) {
		return 0; // an entry point is missing: fail cleanly rather than crash
	}

	initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
	initialization->userdata = NULL;
	initialization->initialize = initialize;
	initialization->deinitialize = deinitialize;
	return 1;
}
