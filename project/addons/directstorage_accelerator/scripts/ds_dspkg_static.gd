@tool
extends RefCounted

## Static bridge to the native DSPkg key bindings.
##
## The export plugin loads this script at runtime only after ClassDB reports the
## DSPkg class. Keeping every direct native call here lets the plugin itself
## parse and register even when the GDExtension binary is missing.


## Installs the archive key for the current process. Returns false when the
## native binding rejects the key.
static func set_global_key(key_hex: String) -> bool:
	return DSPkg.set_global_encryption_key(key_hex) == OK


## Id of the key currently installed in this process, or an empty string when
## no key is installed.
static func get_global_key_id() -> String:
	return DSPkg.get_global_encryption_key_id()


## Installs a scoped override key and returns its restore token (0 on bad key).
static func begin_key_scope(key_bytes: PackedByteArray) -> int:
	return int(DSPkg.begin_global_key_scope(key_bytes))


## Restores the key state saved by begin_key_scope(). False for a stale token.
static func end_key_scope(token: int) -> bool:
	return bool(DSPkg.end_global_key_scope(token))
