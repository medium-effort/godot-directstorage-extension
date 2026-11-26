@tool
class_name DemoAssetGenerator
extends RefCounted

## Generates procedural FastNoiseLite textures + .import sidecars + 3D level scenes
## into gitignored res://demo_generated/ for both Vanilla (.ctex) and DirectStorage (.dstex).

const GENERATED_ROOT := "res://demo_generated"
const VANILLA_DIR := GENERATED_ROOT + "/vanilla"
const DS_DIR := GENERATED_ROOT + "/ds"
const VANILLA_SCENE_PATH := VANILLA_DIR + "/demo_level_vanilla.scn"
const DS_SCENE_PATH := DS_DIR + "/demo_level_ds.scn"
const LEVEL_CONTROLLER_PATH := "res://demo/demo_level_controller.gd"

const TEX_COUNT := 48
const TEX_SIZE := 2048


static func assets_exist() -> bool:
	if not FileAccess.file_exists(VANILLA_SCENE_PATH) or not FileAccess.file_exists(DS_SCENE_PATH):
		return false
	for i in TEX_COUNT:
		var v_png := "%s/tex_%02d.png" % [VANILLA_DIR, i]
		var d_png := "%s/tex_%02d.png" % [DS_DIR, i]
		if not FileAccess.file_exists(v_png) or not FileAccess.file_exists(d_png):
			return false
		if not FileAccess.file_exists(v_png + ".import") or not FileAccess.file_exists(d_png + ".import"):
			return false
	if not FileAccess.file_exists(VANILLA_DIR + "/normal.png") or not FileAccess.file_exists(DS_DIR + "/normal.png"):
		return false
	if not FileAccess.file_exists(VANILLA_DIR + "/normal.png.import") or not FileAccess.file_exists(DS_DIR + "/normal.png.import"):
		return false
	return true


static func textures_imported() -> bool:
	for i in TEX_COUNT:
		var v_imp := "%s/tex_%02d.png.import" % [VANILLA_DIR, i]
		var d_imp := "%s/tex_%02d.png.import" % [DS_DIR, i]
		if not _has_imported_remap(v_imp, "ctex") or not _has_imported_remap(d_imp, "dstex"):
			return false
	if not _has_imported_remap(VANILLA_DIR + "/normal.png.import", "ctex") or not _has_imported_remap(DS_DIR + "/normal.png.import", "dstex"):
		return false
	return true


static func assets_imported() -> bool:
	return assets_exist() and textures_imported()


static func _has_imported_remap(import_path: String, expected_ext: String) -> bool:
	var cfg := ConfigFile.new()
	if cfg.load(import_path) != OK:
		return false
	var dest_files: Variant = cfg.get_value("deps", "dest_files", [])
	if dest_files is Array:
		for f in (dest_files as Array):
			var p := String(f)
			if p.get_extension().to_lower() == expected_ext and FileAccess.file_exists(p):
				return true
	var remapped := String(cfg.get_value("remap", "path", ""))
	return not remapped.is_empty() and remapped.get_extension().to_lower() == expected_ext and FileAccess.file_exists(remapped)


static func get_variant_disk_mib(is_ds: bool) -> float:
	var dir := DS_DIR if is_ds else VANILLA_DIR
	var expected_ext := "dstex" if is_ds else "ctex"
	var total_bytes := 0
	for i in TEX_COUNT:
		total_bytes += _imported_file_size("%s/tex_%02d.png.import" % [dir, i], expected_ext)
	total_bytes += _imported_file_size(dir + "/normal.png.import", expected_ext)
	return float(total_bytes) / (1024.0 * 1024.0)


static func _imported_file_size(import_path: String, expected_ext: String) -> int:
	var cfg := ConfigFile.new()
	if cfg.load(import_path) != OK:
		return 0
	var dest_files: Variant = cfg.get_value("deps", "dest_files", [])
	if dest_files is Array:
		for f in (dest_files as Array):
			var p := String(f)
			if p.get_extension().to_lower() == expected_ext and FileAccess.file_exists(p):
				return maxi(0, FileAccess.get_size(p))
	var remapped := String(cfg.get_value("remap", "path", ""))
	if not remapped.is_empty() and remapped.get_extension().to_lower() == expected_ext and FileAccess.file_exists(remapped):
		return maxi(0, FileAccess.get_size(remapped))
	return 0


static func prepare_texture_dirs() -> Error:
	var err := DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(VANILLA_DIR))
	if err != OK:
		return err
	return DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(DS_DIR))


static func generate_single_texture(index: int) -> Error:
	var img := _make_procedural_image(TEX_SIZE, TEX_SIZE, 1000 + index * 37)
	if img == null or img.is_empty():
		return ERR_CANT_CREATE

	var v_png := "%s/tex_%02d.png" % [VANILLA_DIR, index]
	var d_png := "%s/tex_%02d.png" % [DS_DIR, index]
	var err := img.save_png(v_png)
	if err != OK:
		return err
	err = DirAccess.copy_absolute(ProjectSettings.globalize_path(v_png), ProjectSettings.globalize_path(d_png))
	if err != OK:
		return err

	err = _write_vanilla_import(v_png + ".import")
	if err != OK:
		return err
	return _write_ds_import(d_png + ".import")


static func generate_normal_texture() -> Error:
	var img := _make_procedural_image(TEX_SIZE, TEX_SIZE, 9999)
	if img == null or img.is_empty():
		return ERR_CANT_CREATE
	img.bump_map_to_normal_map(4.0)

	var v_png := VANILLA_DIR + "/normal.png"
	var d_png := DS_DIR + "/normal.png"
	var err := img.save_png(v_png)
	if err != OK:
		return err
	err = DirAccess.copy_absolute(ProjectSettings.globalize_path(v_png), ProjectSettings.globalize_path(d_png))
	if err != OK:
		return err

	err = _write_vanilla_import(v_png + ".import", true)
	if err != OK:
		return err
	return _write_ds_import(d_png + ".import", true)


static func generate_source_textures() -> Error:
	var err := prepare_texture_dirs()
	if err != OK:
		return err
	for i in TEX_COUNT:
		err = generate_single_texture(i)
		if err != OK:
			return err
	return generate_normal_texture()


static func build_level_scenes() -> Error:
	var err := _build_level_scene(VANILLA_DIR, VANILLA_SCENE_PATH, "Vanilla (.ctex)")
	if err != OK:
		return err
	return _build_level_scene(DS_DIR, DS_SCENE_PATH, "DirectStorage (.dstex)")


static func _build_level_scene(tex_dir: String, out_scene_path: String, variant_label: String) -> Error:
	var root := Node3D.new()
	root.name = "DemoLevel"
	var controller_script := load(LEVEL_CONTROLLER_PATH) as Script
	if controller_script != null:
		root.set_script(controller_script)
	root.set_meta("variant_label", variant_label)

	var sky_mat := ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.22, 0.40, 0.66)
	sky_mat.sky_horizon_color = Color(0.76, 0.72, 0.64)
	var sky := Sky.new()
	sky.sky_material = sky_mat

	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.tonemap_exposure = 1.4
	env.ssao_enabled = true
	env.ssao_intensity = 2.5
	env.ssao_light_affect = 0.55
	env.glow_enabled = true
	env.glow_intensity = 0.25
	env.glow_hdr_threshold = 0.9
	env.fog_enabled = true
	env.fog_density = 0.0025
	env.volumetric_fog_enabled = true
	env.volumetric_fog_density = 0.004
	env.adjustment_enabled = true
	env.adjustment_saturation = 1.25

	var world_env := WorldEnvironment.new()
	world_env.name = "WorldEnvironment"
	world_env.environment = env
	root.add_child(world_env)
	world_env.owner = root

	var probe := ReflectionProbe.new()
	probe.name = "ReflectionProbe"
	probe.position = Vector3(0.0, 4.0, 0.0)
	probe.size = Vector3(40.0, 20.0, 40.0)
	probe.box_projection = true
	root.add_child(probe)
	probe.owner = root

	for l_idx in 6:
		var spot := SpotLight3D.new()
		spot.name = "SpotLight_%02d" % l_idx
		spot.position = Vector3(-9.0 + float(l_idx) * 3.6, 8.5, 4.0 if l_idx % 2 == 0 else -4.0)
		spot.rotation_degrees = Vector3(-70.0, 0.0, 0.0)
		spot.light_energy = 18.0
		spot.light_volumetric_fog_energy = 5.0
		spot.shadow_enabled = true
		spot.spot_range = 24.0
		spot.spot_angle = 40.0
		root.add_child(spot)
		spot.owner = root

	for o_idx in 4:
		var omni := OmniLight3D.new()
		omni.name = "OmniLight_%02d" % o_idx
		omni.position = Vector3(-7.5 + float(o_idx) * 5.0, 3.5, 0.0)
		omni.light_color = Color(1.0, 0.45, 0.2) if o_idx % 2 == 0 else Color(0.3, 0.65, 1.0)
		omni.light_energy = 6.0
		omni.light_volumetric_fog_energy = 8.0
		omni.omni_range = 14.0
		root.add_child(omni)
		omni.owner = root

	var floor_mesh := BoxMesh.new()
	floor_mesh.size = Vector3(32.0, 0.5, 26.0)
	var floor_inst := MeshInstance3D.new()
	floor_inst.name = "Floor"
	floor_inst.mesh = floor_mesh
	floor_inst.position = Vector3(0.0, -0.25, 0.0)
	root.add_child(floor_inst)
	floor_inst.owner = root
	var floor_body := StaticBody3D.new()
	floor_body.name = "FloorBody"
	var floor_col := CollisionShape3D.new()
	floor_col.name = "FloorShape"
	var floor_shape := BoxShape3D.new()
	floor_shape.size = floor_mesh.size
	floor_col.shape = floor_shape
	floor_body.add_child(floor_col)
	floor_inst.add_child(floor_body)
	floor_body.owner = root
	floor_col.owner = root

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-45.0, 35.0, 0.0)
	sun.light_energy = 1.2
	root.add_child(sun)
	sun.owner = root

	var cam := Camera3D.new()
	cam.name = "Camera3D"
	cam.position = Vector3(0.0, 7.5, 19.0)
	cam.rotation_degrees = Vector3(-20.0, 0.0, 0.0)
	cam.current = true
	root.add_child(cam)
	cam.owner = root

	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3(1.8, 1.8, 1.8)

	var loaded_textures: Array[Texture2D] = []
	for i in TEX_COUNT:
		var tex_path := "%s/tex_%02d.png" % [tex_dir, i]
		var tex := ResourceLoader.load(tex_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE) as Texture2D
		if tex == null:
			root.free()
			return ERR_FILE_NOT_FOUND
		loaded_textures.append(tex)

	var normal_tex := ResourceLoader.load(tex_dir + "/normal.png", "Texture2D", ResourceLoader.CACHE_MODE_REUSE) as Texture2D
	if normal_tex == null:
		root.free()
		return ERR_FILE_NOT_FOUND

	# Arena-style procedural terrain (96x96 grid -> ~18.4k tris) + trimesh collider
	var terrain_mesh := _make_terrain_mesh(96, 36.0)
	var terrain_mat := StandardMaterial3D.new()
	terrain_mat.albedo_texture = loaded_textures[0]
	terrain_mat.normal_enabled = true
	terrain_mat.normal_texture = normal_tex
	terrain_mat.roughness_texture = loaded_textures[2]
	var terrain_inst := MeshInstance3D.new()
	terrain_inst.name = "ArenaTerrain"
	terrain_inst.mesh = terrain_mesh
	terrain_inst.material_override = terrain_mat
	root.add_child(terrain_inst)
	terrain_inst.owner = root
	var terrain_body := StaticBody3D.new()
	terrain_body.name = "TerrainBody"
	var terrain_col := CollisionShape3D.new()
	terrain_col.name = "TerrainShape"
	terrain_col.shape = terrain_mesh.create_trimesh_shape()
	terrain_body.add_child(terrain_col)
	terrain_inst.add_child(terrain_body)
	terrain_body.owner = root
	terrain_col.owner = root

	# Ambient dust GPUParticles3D (matches arena_v2)
	var dust_proc := ParticleProcessMaterial.new()
	dust_proc.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	dust_proc.emission_box_extents = Vector3(14.0, 3.0, 14.0)
	dust_proc.gravity = Vector3(0.15, -0.05, 0.05)
	dust_proc.initial_velocity_min = 0.1
	dust_proc.initial_velocity_max = 0.4
	var dust_quad := QuadMesh.new()
	dust_quad.size = Vector2(0.08, 0.08)
	var dust_mat := StandardMaterial3D.new()
	dust_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	dust_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	dust_mat.albedo_color = Color(1.0, 0.88, 0.72, 0.45)
	dust_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	dust_quad.material = dust_mat
	var dust := GPUParticles3D.new()
	dust.name = "AmbientDust"
	dust.amount = 256
	dust.lifetime = 4.0
	dust.process_material = dust_proc
	dust.draw_pass_1 = dust_quad
	dust.position = Vector3(0.0, 2.5, 0.0)
	root.add_child(dust)
	dust.owner = root

	var rock_mesh := _make_displaced_rock_mesh()
	var rock_shape := rock_mesh.create_convex_shape()
	var pillar_mesh := CylinderMesh.new()
	pillar_mesh.top_radius = 0.7
	pillar_mesh.bottom_radius = 0.85
	pillar_mesh.height = 3.8
	pillar_mesh.radial_segments = 24
	var pillar_shape := CylinderShape3D.new()
	pillar_shape.radius = 0.85
	pillar_shape.height = 3.8
	var barrier_mesh := BoxMesh.new()
	barrier_mesh.size = Vector3(2.4, 1.8, 0.16)
	var barrier_shape := BoxShape3D.new()
	barrier_shape.size = barrier_mesh.size
	var cube_shape := BoxShape3D.new()
	cube_shape.size = box_mesh.size

	var cols := 8
	var spacing := 2.6
	for i in TEX_COUNT:
		var row := floori(float(i) / float(cols))
		var col := i % cols
		var tex := loaded_textures[i]
		var rough_tex := loaded_textures[(i + 2) % TEX_COUNT]

		var mat := StandardMaterial3D.new()
		mat.albedo_texture = tex
		mat.normal_enabled = true
		mat.normal_texture = normal_tex
		mat.roughness_texture = rough_tex

		var prop_mesh: Mesh = box_mesh
		var prop_shape: Shape3D = cube_shape
		var y_pos := 1.0
		match i % 4:
			0: # Metallic + Emissive beacon pillar
				prop_mesh = pillar_mesh
				prop_shape = pillar_shape
				y_pos = 1.9
				mat.metallic = 0.75
				mat.emission_enabled = true
				mat.emission_texture = tex
				mat.emission_energy_multiplier = 0.8
			1: # Alpha-scissor foliage/banner
				mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
				mat.alpha_scissor_threshold = 0.45
				mat.cull_mode = BaseMaterial3D.CULL_DISABLED
			2: # Alpha-transparent energy barrier
				prop_mesh = barrier_mesh
				prop_shape = barrier_shape
				mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
				mat.albedo_color = Color(0.55, 0.85, 1.0, 0.65)
				mat.emission_enabled = true
				mat.emission = Color(0.2, 0.55, 0.9)
				mat.emission_energy_multiplier = 0.5
			3: # Displaced rock (high-poly opaque PBR)
				prop_mesh = rock_mesh
				prop_shape = rock_shape
				y_pos = 1.1

		var mesh_inst := MeshInstance3D.new()
		mesh_inst.name = "ArenaProp_%02d" % i
		mesh_inst.mesh = prop_mesh
		mesh_inst.material_override = mat
		mesh_inst.position = Vector3(
			(float(col) - (float(cols - 1) * 0.5)) * spacing,
			y_pos,
			(float(row) - 1.0) * spacing
		)
		root.add_child(mesh_inst)
		mesh_inst.owner = root

		var body := StaticBody3D.new()
		body.name = "PropBody_%02d" % i
		var col_shape := CollisionShape3D.new()
		col_shape.name = "PropShape_%02d" % i
		col_shape.shape = prop_shape
		body.add_child(col_shape)
		mesh_inst.add_child(body)
		body.owner = root
		col_shape.owner = root

	var packed := PackedScene.new()
	var pack_err := packed.pack(root)
	root.free()
	if pack_err != OK:
		return pack_err
	return ResourceSaver.save(packed, out_scene_path)


static func _make_terrain_mesh(grid_res: int, extent: float) -> ArrayMesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var half := extent * 0.5
	var step := extent / float(grid_res)
	for gz in grid_res:
		for gx in grid_res:
			var x0 := -half + float(gx) * step
			var z0 := -half + float(gz) * step
			var x1 := x0 + step
			var z1 := z0 + step
			var u0 := float(gx) / float(grid_res)
			var v0 := float(gz) / float(grid_res)
			var u1 := float(gx + 1) / float(grid_res)
			var v1 := float(gz + 1) / float(grid_res)
			var y00 := sin(x0 * 0.25) * cos(z0 * 0.25) * 0.35
			var y10 := sin(x1 * 0.25) * cos(z0 * 0.25) * 0.35
			var y01 := sin(x0 * 0.25) * cos(z1 * 0.25) * 0.35
			var y11 := sin(x1 * 0.25) * cos(z1 * 0.25) * 0.35
			st.set_uv(Vector2(u0, v0)); st.add_vertex(Vector3(x0, y00, z0))
			st.set_uv(Vector2(u1, v0)); st.add_vertex(Vector3(x1, y10, z0))
			st.set_uv(Vector2(u1, v1)); st.add_vertex(Vector3(x1, y11, z1))
			st.set_uv(Vector2(u0, v0)); st.add_vertex(Vector3(x0, y00, z0))
			st.set_uv(Vector2(u1, v1)); st.add_vertex(Vector3(x1, y11, z1))
			st.set_uv(Vector2(u0, v1)); st.add_vertex(Vector3(x0, y01, z1))
	st.generate_normals()
	st.generate_tangents()
	return st.commit()


static func _make_displaced_rock_mesh() -> ArrayMesh:
	var sphere := SphereMesh.new()
	sphere.radius = 1.05
	sphere.height = 2.1
	sphere.rings = 24
	sphere.radial_segments = 36
	var arrays := sphere.get_mesh_arrays()
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	for idx in verts.size():
		var v := verts[idx]
		var disp := 1.0 + 0.12 * sin(v.x * 5.0 + v.y * 4.0) * cos(v.z * 5.0)
		verts[idx] = v * disp
	arrays[Mesh.ARRAY_VERTEX] = verts
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _make_procedural_image(w: int, h: int, seed_val: int) -> Image:
	var noise := FastNoiseLite.new()
	noise.seed = seed_val
	noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	noise.frequency = 0.01 + float(seed_val % 5) * 0.003
	noise.fractal_type = FastNoiseLite.FRACTAL_FBM
	noise.fractal_octaves = 3
	noise.fractal_gain = 0.55

	var img := noise.get_image(w, h, false, false)
	if img == null or img.is_empty():
		return null
	img.convert(Image.FORMAT_RGBA8)
	return img


static func _write_vanilla_import(import_path: String, is_normal: bool = false) -> Error:
	var cfg := ConfigFile.new()
	cfg.set_value("remap", "importer", "texture")
	cfg.set_value("remap", "type", "CompressedTexture2D")
	cfg.set_value("params", "compress/mode", 2)
	cfg.set_value("params", "compress/high_quality", false)
	cfg.set_value("params", "compress/normal_map", 1 if is_normal else 2)
	cfg.set_value("params", "roughness/mode", 1)
	cfg.set_value("params", "detect_3d/compress_to", 0)
	cfg.set_value("params", "mipmaps/generate", true)
	return cfg.save(import_path)


static func _write_ds_import(import_path: String, is_normal: bool = false) -> Error:
	var cfg := ConfigFile.new()
	cfg.set_value("remap", "importer", "directstorage_texture")
	cfg.set_value("remap", "type", "Texture2D")
	cfg.set_value("params", "compression/mode", 0)
	cfg.set_value("params", "compression/high_quality", false)
	cfg.set_value("params", "compression/color_space", 0 if is_normal else 1)
	cfg.set_value("params", "compress/normal_map", 1 if is_normal else 0)
	cfg.set_value("params", "compression/gdeflate_level", 3)
	cfg.set_value("params", "compression/chunk_size_kb", 8192)
	cfg.set_value("params", "mipmaps/generate", true)
	return cfg.save(import_path)
