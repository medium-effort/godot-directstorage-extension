extends GutTest

const ExportPlugin = preload("res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd")


func test_dsgd_notice_header_includes_source_url_and_is_deterministic() -> void:
	var sections: Array[Dictionary] = ExportPlugin._collect_dsgd_license_sections()
	var titles: Array[String] = []
	for section in sections:
		titles.append(String(section["title"]))

	assert_has(titles, "DSGD license (MPL-2.0)")
	assert_has(titles, "DSGD runtime GDScript license (MIT)")
	assert_has(titles, "Third-party components of dsgd_64.dll")

	var dsgd_header: PackedStringArray = ExportPlugin._notice_header("DSGD - license and notices", sections, true)
	var dsgd_header_text: String = "\n".join(dsgd_header)
	assert_string_contains(dsgd_header_text, ExportPlugin.DSGD_SOURCE_URL)
	assert_string_contains(dsgd_header_text, "modified MPL-2.0 source files")

	var runtime_header: PackedStringArray = ExportPlugin._notice_header("DirectStorage - license and notices", sections, false)
	var runtime_header_text: String = "\n".join(runtime_header)
	assert_false(runtime_header_text.contains(ExportPlugin.DSGD_SOURCE_URL), "Standalone DirectStorage notice must not emit DSGD source URL")

	var bytes_a: PackedByteArray = ExportPlugin._compose_notice_file(dsgd_header, sections)
	var bytes_b: PackedByteArray = ExportPlugin._compose_notice_file(dsgd_header, sections)
	assert_eq(bytes_a, bytes_b, "_compose_notice_file must be byte-deterministic across repeated calls")
