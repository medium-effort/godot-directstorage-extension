extends SceneTree

func _init() -> void:
	print("--- Running DirectStorage Error Reporting Test ---")

	# Test Command Type Strings
	var req_name: String = DSErrorHandler.describe_command_type(0)
	print("Command Type 0: ", req_name)
	assert(req_name == "REQUEST", "Expected command type 0 to be REQUEST")

	var status_name: String = DSErrorHandler.describe_command_type(1)
	print("Command Type 1: ", status_name)
	assert(status_name == "STATUS", "Expected command type 1 to be STATUS")

	var signal_name: String = DSErrorHandler.describe_command_type(2)
	print("Command Type 2: ", signal_name)
	assert(signal_name == "SIGNAL", "Expected command type 2 to be SIGNAL")

	var event_name: String = DSErrorHandler.describe_command_type(3)
	print("Command Type 3: ", event_name)
	assert(event_name == "EVENT", "Expected command type 3 to be EVENT")

	# Test HRESULT Descriptions
	var invalid_arg_desc: String = DSErrorHandler.describe_hresult(0x80070057)
	print("HRESULT 0x80070057: ", invalid_arg_desc)
	assert(invalid_arg_desc.contains("E_INVALIDARG") or invalid_arg_desc.contains("parameter is incorrect"), "Expected E_INVALIDARG description")

	var eof_desc: String = DSErrorHandler.describe_hresult(0x89240007)
	print("HRESULT 0x89240007: ", eof_desc)
	assert(eof_desc.contains("exceed the file size"), "Expected DirectStorage EOF description")

	var dest_type_desc: String = DSErrorHandler.describe_hresult(0x89240040)
	print("HRESULT 0x89240040: ", dest_type_desc)
	assert(dest_type_desc.contains("destination type"), "Expected DirectStorage invalid destination type description")

	print("[SUCCESS] All DirectStorage error reporting tests passed.")
	quit(0)
