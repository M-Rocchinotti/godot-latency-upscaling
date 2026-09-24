// Raw-C GDExtension no-op stub for platforms where the Reflex / DLSS hook is not implemented.
//
// The real GDExtension is Windows-x64 only (it patches Godot's Vulkan loader on Windows). Without
// a library for the other platforms Godot prints "No GDExtension library found" when a game that
// ships this addon is exported to them. This stub is a tiny library, built for each of those
// platforms with `zig cc`, that loads cleanly and registers the same "LatencyUpscaling" singleton
// so a game can query it uniformly; every method reports that the platform is not supported.
//
// It uses only the GDExtension C ABI (no godot-cpp, no libc beyond memcpy), so the identical logic
// runs on every 64-bit target. Only 64-bit platforms are built, so the opaque built-in handles
// (String, StringName, Dictionary) are one pointer (8 bytes) each.
//
// Build: python stub/build_stubs.py   (see that script for the toolchain and targets)

#include <gdextension_interface.h>

#include <stddef.h>
#include <stdint.h>

#define LU_STUB_VERSION "0.1.0"
#define LU_NOT_SUPPORTED "not supported on this platform"

// Opaque 64-bit built-in handles.
typedef struct {
	uint8_t data[8];
} OpaqueString;
typedef struct {
	uint8_t data[8];
} OpaqueStringName;
typedef struct {
	uint8_t data[8];
} OpaqueDictionary;

// --- resolved interface functions -----------------------------------------------------------
static GDExtensionClassLibraryPtr g_library;
static GDExtensionInterfaceStringNameNewWithUtf8Chars fn_sn_new;
static GDExtensionInterfaceStringNewWithUtf8Chars fn_string_new;
static GDExtensionInterfaceClassdbRegisterExtensionClass4 fn_register_class;
static GDExtensionInterfaceClassdbRegisterExtensionClassMethod fn_register_method;
static GDExtensionInterfaceClassdbConstructObject2 fn_construct_object;
static GDExtensionInterfaceObjectSetInstance fn_object_set_instance;
static GDExtensionInterfaceGlobalGetSingleton fn_global_get_singleton;
static GDExtensionInterfaceClassdbGetMethodBind fn_get_method_bind;
static GDExtensionInterfaceObjectMethodBindPtrcall fn_method_bind_ptrcall;
static GDExtensionInterfaceGetVariantFromTypeConstructor fn_variant_from_type;
static GDExtensionInterfaceVariantGetPtrConstructor fn_get_ptr_constructor;
static GDExtensionInterfaceVariantGetPtrKeyedSetter fn_get_keyed_setter;

// Cached names and the constructed singleton.
static OpaqueStringName g_class_name; // "LatencyUpscaling"
static OpaqueStringName g_object_name; // "Object"
static OpaqueStringName g_empty_sn; // reused for PropertyInfo name/class_name
static OpaqueString g_empty_str; // reused for PropertyInfo hint_string
static GDExtensionObjectPtr g_singleton;

// Method-name storage must outlive registration.
static OpaqueStringName g_name_is_supported;
static OpaqueStringName g_name_is_active;
static OpaqueStringName g_name_reflex_status;
static OpaqueStringName g_name_get_version;
static OpaqueStringName g_name_get_status;

static void make_sn(OpaqueStringName *dst, const char *text) {
	fn_sn_new(dst, text);
}

// --- return-value helpers (write a Variant into r_return) -----------------------------------
static void return_bool(GDExtensionVariantPtr r_return, int value) {
	GDExtensionVariantFromTypeConstructorFunc ctor = fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_BOOL);
	GDExtensionBool v = value ? 1 : 0;
	ctor(r_return, &v);
}

static void return_string(GDExtensionVariantPtr r_return, const char *text) {
	OpaqueString str;
	fn_string_new(&str, text);
	GDExtensionVariantFromTypeConstructorFunc ctor = fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_STRING);
	ctor(r_return, &str);
}

// Builds {"platform_supported": false, "reason": "not supported on this platform"} and writes it
// into r_return as a Variant.
static void return_status_dictionary(GDExtensionVariantPtr r_return) {
	OpaqueDictionary dict;
	GDExtensionPtrConstructor dict_ctor = fn_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_DICTIONARY, 0);
	dict_ctor(&dict, NULL); // default (empty) Dictionary
	GDExtensionPtrKeyedSetter set = fn_get_keyed_setter(GDEXTENSION_VARIANT_TYPE_DICTIONARY);

	// Dictionary keys and values are Variants. Build them from native Strings/bool.
	GDExtensionVariantFromTypeConstructorFunc str_to_var = fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_STRING);
	GDExtensionVariantFromTypeConstructorFunc bool_to_var = fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_BOOL);

	OpaqueString key_str;
	uint8_t key_var[24]; // a Variant is 24 bytes on 64-bit
	uint8_t val_var[24];

	fn_string_new(&key_str, "platform_supported");
	str_to_var(key_var, &key_str);
	GDExtensionBool supported = 0;
	bool_to_var(val_var, &supported);
	set(&dict, key_var, val_var);

	fn_string_new(&key_str, "reason");
	str_to_var(key_var, &key_str);
	OpaqueString reason_str;
	fn_string_new(&reason_str, LU_NOT_SUPPORTED);
	str_to_var(val_var, &reason_str);
	set(&dict, key_var, val_var);

	GDExtensionVariantFromTypeConstructorFunc dict_to_var = fn_variant_from_type(GDEXTENSION_VARIANT_TYPE_DICTIONARY);
	dict_to_var(r_return, &dict);
}

// --- method call callbacks (variant path, used by dynamic GDScript calls) --------------------
static void call_is_supported(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	(void)ud;
	(void)inst;
	(void)args;
	(void)argc;
	if (err) {
		err->error = GDEXTENSION_CALL_OK;
	}
	return_bool(r_return, 0);
}

static void call_get_reflex_status(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	(void)ud;
	(void)inst;
	(void)args;
	(void)argc;
	if (err) {
		err->error = GDEXTENSION_CALL_OK;
	}
	return_string(r_return, LU_NOT_SUPPORTED);
}

static void call_get_version(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	(void)ud;
	(void)inst;
	(void)args;
	(void)argc;
	if (err) {
		err->error = GDEXTENSION_CALL_OK;
	}
	return_string(r_return, LU_STUB_VERSION);
}

static void call_get_status(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *err) {
	(void)ud;
	(void)inst;
	(void)args;
	(void)argc;
	if (err) {
		err->error = GDEXTENSION_CALL_OK;
	}
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

static void register_method(OpaqueStringName *name_storage, const char *name, GDExtensionClassMethodCall call,
		GDExtensionVariantType return_type) {
	make_sn(name_storage, name);
	GDExtensionPropertyInfo return_info;
	return_info.type = return_type;
	return_info.name = &g_empty_sn; // Godot copies these; NULL crashes it
	return_info.class_name = &g_empty_sn;
	return_info.hint = 0;
	return_info.hint_string = &g_empty_str;
	return_info.usage = 6; // PROPERTY_USAGE_DEFAULT

	GDExtensionClassMethodInfo info;
	info.name = name_storage;
	info.method_userdata = NULL;
	info.call_func = call;
	info.ptrcall_func = NULL; // dynamic GDScript calls use call_func; typed ptrcalls are not offered
	info.method_flags = GDEXTENSION_METHOD_FLAG_NORMAL | GDEXTENSION_METHOD_FLAG_CONST;
	info.has_return_value = (return_type == GDEXTENSION_VARIANT_TYPE_NIL) ? 0 : 1;
	info.return_value_info = &return_info;
	info.return_value_metadata = GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
	info.argument_count = 0;
	info.arguments_info = NULL;
	info.arguments_metadata = NULL;
	info.default_argument_count = 0;
	info.default_arguments = NULL;
	fn_register_method(g_library, &g_class_name, &info);
}

static void register_class_and_singleton(void) {
	make_sn(&g_class_name, "LatencyUpscaling");
	make_sn(&g_object_name, "Object");
	make_sn(&g_empty_sn, "");
	fn_string_new(&g_empty_str, "");

	GDExtensionClassCreationInfo4 ci;
	// Zero every field, then set the ones a minimal class needs.
	uint8_t *raw = (uint8_t *)&ci;
	for (size_t i = 0; i < sizeof(ci); ++i) {
		raw[i] = 0;
	}
	ci.is_exposed = 1;
	ci.create_instance_func = create_instance;
	ci.free_instance_func = free_instance;
	ci.get_virtual_func = get_virtual;
	fn_register_class(g_library, &g_class_name, &g_object_name, &ci);

	register_method(&g_name_is_supported, "is_reflex_supported", call_is_supported, GDEXTENSION_VARIANT_TYPE_BOOL);
	register_method(&g_name_is_active, "is_reflex_active", call_is_supported, GDEXTENSION_VARIANT_TYPE_BOOL);
	register_method(&g_name_reflex_status, "get_reflex_status", call_get_reflex_status, GDEXTENSION_VARIANT_TYPE_STRING);
	register_method(&g_name_get_version, "get_version", call_get_version, GDEXTENSION_VARIANT_TYPE_STRING);
	register_method(&g_name_get_status, "get_status", call_get_status, GDEXTENSION_VARIANT_TYPE_DICTIONARY);

	// Construct one instance and register it as the engine singleton.
	g_singleton = fn_construct_object(&g_class_name);
	OpaqueStringName engine_name;
	make_sn(&engine_name, "Engine");
	GDExtensionObjectPtr engine = fn_global_get_singleton(&engine_name);
	OpaqueStringName method_name;
	make_sn(&method_name, "register_singleton");
	GDExtensionMethodBindPtr bind = fn_get_method_bind(&engine_name, &method_name, 965313290);
	if (engine && bind) {
		const GDExtensionConstTypePtr call_args[2] = { &g_class_name, &g_singleton };
		fn_method_bind_ptrcall(bind, engine, call_args, NULL);
	}
}

static void unregister_singleton(void) {
	if (!g_singleton) {
		return;
	}
	OpaqueStringName engine_name;
	make_sn(&engine_name, "Engine");
	GDExtensionObjectPtr engine = fn_global_get_singleton(&engine_name);
	OpaqueStringName method_name;
	make_sn(&method_name, "unregister_singleton");
	GDExtensionMethodBindPtr bind = fn_get_method_bind(&engine_name, &method_name, 3304788590u);
	if (engine && bind) {
		const GDExtensionConstTypePtr call_args[1] = { &g_class_name };
		fn_method_bind_ptrcall(bind, engine, call_args, NULL);
	}
	g_singleton = NULL;
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
		unregister_singleton();
	}
}

#if defined(_WIN32)
#define LU_EXPORT __declspec(dllexport)
#else
#define LU_EXPORT __attribute__((visibility("default")))
#endif

LU_EXPORT GDExtensionBool godot_latency_upscaling_init(GDExtensionInterfaceGetProcAddress get_proc_address,
		GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
	g_library = library;
	fn_sn_new = (GDExtensionInterfaceStringNameNewWithUtf8Chars)get_proc_address("string_name_new_with_utf8_chars");
	fn_string_new = (GDExtensionInterfaceStringNewWithUtf8Chars)get_proc_address("string_new_with_utf8_chars");
	fn_register_class = (GDExtensionInterfaceClassdbRegisterExtensionClass4)get_proc_address("classdb_register_extension_class4");
	fn_register_method = (GDExtensionInterfaceClassdbRegisterExtensionClassMethod)get_proc_address("classdb_register_extension_class_method");
	fn_construct_object = (GDExtensionInterfaceClassdbConstructObject2)get_proc_address("classdb_construct_object2");
	fn_object_set_instance = (GDExtensionInterfaceObjectSetInstance)get_proc_address("object_set_instance");
	fn_global_get_singleton = (GDExtensionInterfaceGlobalGetSingleton)get_proc_address("global_get_singleton");
	fn_get_method_bind = (GDExtensionInterfaceClassdbGetMethodBind)get_proc_address("classdb_get_method_bind");
	fn_method_bind_ptrcall = (GDExtensionInterfaceObjectMethodBindPtrcall)get_proc_address("object_method_bind_ptrcall");
	fn_variant_from_type = (GDExtensionInterfaceGetVariantFromTypeConstructor)get_proc_address("get_variant_from_type_constructor");
	fn_get_ptr_constructor = (GDExtensionInterfaceVariantGetPtrConstructor)get_proc_address("variant_get_ptr_constructor");
	fn_get_keyed_setter = (GDExtensionInterfaceVariantGetPtrKeyedSetter)get_proc_address("variant_get_ptr_keyed_setter");

	if (!fn_sn_new || !fn_string_new || !fn_register_class || !fn_register_method || !fn_construct_object ||
			!fn_object_set_instance || !fn_global_get_singleton || !fn_get_method_bind || !fn_method_bind_ptrcall ||
			!fn_variant_from_type || !fn_get_ptr_constructor || !fn_get_keyed_setter) {
		return 0; // an entry point is missing: fail cleanly rather than crash
	}

	initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
	initialization->userdata = NULL;
	initialization->initialize = initialize;
	initialization->deinitialize = deinitialize;
	return 1;
}
